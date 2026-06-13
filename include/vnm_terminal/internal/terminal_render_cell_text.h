#pragma once

#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <memory>
#include <optional>
#include <utility>
#include <cstdint>

namespace vnm_terminal::internal {

enum class Terminal_render_cell_text_category
{
    EMPTY           = 0,
    PRINTABLE_ASCII = 1,
    OTHER_ASCII     = 2,
    NON_ASCII       = 3,
    UNKNOWN         = 4,
};

enum class Terminal_render_cell_text_storage : std::uint8_t
{
    EMPTY,
    INLINE_PRINTABLE_ASCII,
    INLINE_SINGLE_BMP,
    FALLBACK_QSTRING,
};

class Terminal_render_cell_text
{
public:
    Terminal_render_cell_text() noexcept
    :
        m_storage(Terminal_render_cell_text_storage::EMPTY)
    {}

    Terminal_render_cell_text(QString text)
    :
        Terminal_render_cell_text(from_qstring(std::move(text)))
    {}

    Terminal_render_cell_text(const Terminal_render_cell_text& other)
    :
        m_storage(Terminal_render_cell_text_storage::EMPTY)
    {
        copy_from(other);
    }

    Terminal_render_cell_text(Terminal_render_cell_text&& other) noexcept
    :
        m_storage(Terminal_render_cell_text_storage::EMPTY)
    {
        move_from(std::move(other));
    }

    ~Terminal_render_cell_text() = default;

    Terminal_render_cell_text& operator=(const Terminal_render_cell_text& other)
    {
        if (this != &other) {
            copy_from(other);
        }
        return *this;
    }

    Terminal_render_cell_text& operator=(Terminal_render_cell_text&& other) noexcept
    {
        if (this != &other) {
            move_from(std::move(other));
        }
        return *this;
    }

    Terminal_render_cell_text& operator=(QString text)
    {
        *this = from_qstring(std::move(text));
        return *this;
    }

    static Terminal_render_cell_text empty() noexcept
    {
        return {};
    }

    static Terminal_render_cell_text inline_printable_ascii(ushort code_unit) noexcept
    {
        Q_ASSERT(is_printable_ascii_code_unit(code_unit));

        Terminal_render_cell_text text;
        text.m_storage          = Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII;
        text.m_inline_code_unit = code_unit;
        return text;
    }

    static Terminal_render_cell_text fallback(QString text)
    {
        if (text.isEmpty()) {
            return {};
        }

        Terminal_render_cell_text value;
        value.m_storage       = Terminal_render_cell_text_storage::FALLBACK_QSTRING;
        value.m_fallback_text = std::make_unique<QString>(std::move(text));
        return value;
    }

    static Terminal_render_cell_text from_source_cell(
        const QString& text,
        int            display_width,
        bool           wide_continuation)
    {
        if (wide_continuation || text.isEmpty()) {
            return {};
        }

        if (display_width == 1 && text.size() == 1) {
            const ushort code_unit = text.at(0).unicode();
            if (is_printable_ascii_code_unit(code_unit)) {
                return inline_printable_ascii(code_unit);
            }
        }

        if ((display_width == 1 || display_width == 2) && text.size() == 1) {
            const ushort code_unit = text.at(0).unicode();
            if (is_single_bmp_source_cell_code_unit(code_unit)) {
                return inline_single_bmp(code_unit);
            }
        }

        return fallback_copy(text);
    }

    static Terminal_render_cell_text from_source_cell(
        QString&&  text,
        int        display_width,
        bool       wide_continuation) = delete;

    static Terminal_render_cell_text from_source_cell(
        QStringView  text,
        int          display_width,
        bool         wide_continuation) = delete;

    Terminal_render_cell_text_storage storage() const noexcept
    {
        return m_storage;
    }

    bool is_empty() const noexcept
    {
        return m_storage == Terminal_render_cell_text_storage::EMPTY;
    }

    bool is_inline_printable_ascii() const noexcept
    {
        return m_storage == Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII;
    }

    bool is_inline_single_bmp() const noexcept
    {
        return m_storage == Terminal_render_cell_text_storage::INLINE_SINGLE_BMP;
    }

    bool is_fallback_qstring() const noexcept
    {
        return m_storage == Terminal_render_cell_text_storage::FALLBACK_QSTRING;
    }

    qsizetype code_unit_count() const noexcept
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return 0;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return 1;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr ? m_fallback_text->size() : 0;
        }

        return 0;
    }

    Terminal_render_cell_text_category category() const
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return Terminal_render_cell_text_category::EMPTY;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
                return Terminal_render_cell_text_category::PRINTABLE_ASCII;
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return Terminal_render_cell_text_category::NON_ASCII;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr
                    ? category_for_qstring(QStringView(*m_fallback_text))
                    : Terminal_render_cell_text_category::UNKNOWN;
        }

        return Terminal_render_cell_text_category::UNKNOWN;
    }

    std::optional<ushort> single_printable_ascii_code_unit() const noexcept
    {
        if (m_storage == Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII) {
            return m_inline_code_unit;
        }

        if (m_storage == Terminal_render_cell_text_storage::FALLBACK_QSTRING &&
            m_fallback_text != nullptr &&
            m_fallback_text->size() == 1)
        {
            const ushort code_unit = m_fallback_text->at(0).unicode();
            if (is_printable_ascii_code_unit(code_unit)) {
                return code_unit;
            }
        }

        return std::nullopt;
    }

    std::optional<QChar> single_printable_ascii_char() const noexcept
    {
        const std::optional<ushort> code_unit = single_printable_ascii_code_unit();
        return code_unit.has_value()
            ? std::optional<QChar>(QChar(*code_unit))
            : std::nullopt;
    }

    std::optional<ushort> single_code_unit() const noexcept
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return std::nullopt;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return m_inline_code_unit;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr && m_fallback_text->size() == 1
                    ? std::optional<ushort>(m_fallback_text->at(0).unicode())
                    : std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<ushort> single_bmp_code_unit() const noexcept
    {
        if (m_storage == Terminal_render_cell_text_storage::INLINE_SINGLE_BMP) {
            return m_inline_code_unit;
        }

        if (m_storage == Terminal_render_cell_text_storage::FALLBACK_QSTRING &&
            m_fallback_text != nullptr &&
            m_fallback_text->size() == 1)
        {
            const ushort code_unit = m_fallback_text->at(0).unicode();
            if (is_single_bmp_code_unit(code_unit)) {
                return code_unit;
            }
        }

        return std::nullopt;
    }

    const QString* fallback_qstring_or_null() const noexcept
    {
        return m_storage == Terminal_render_cell_text_storage::FALLBACK_QSTRING
            ? m_fallback_text.get()
            : nullptr;
    }

    void append_to(QString& target) const
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                target += QChar(m_inline_code_unit);
                return;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                if (m_fallback_text != nullptr) {
                    target += *m_fallback_text;
                }
                return;
        }
    }

    QString to_qstring() const
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return {};
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return QString(1, QChar(m_inline_code_unit));
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr ? *m_fallback_text : QString();
        }

        return {};
    }

    bool equals(QStringView text) const
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return text.isEmpty();
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return text.size() == 1 &&
                    text.at(0).unicode() == m_inline_code_unit;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr &&
                    QStringView(*m_fallback_text) == text;
        }

        return false;
    }

    bool is_valid() const
    {
        switch (m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return true;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
                return is_printable_ascii_code_unit(m_inline_code_unit);
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return is_single_bmp_code_unit(m_inline_code_unit);
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                return m_fallback_text != nullptr && !m_fallback_text->isEmpty();
        }

        return false;
    }

private:
    static bool is_printable_ascii_code_unit(ushort code_unit) noexcept
    {
        return code_unit >= 0x20U && code_unit <= 0x7eU;
    }

    static bool is_surrogate_code_unit(ushort code_unit) noexcept
    {
        return code_unit >= 0xd800U && code_unit <= 0xdfffU;
    }

    static bool is_single_bmp_code_unit(ushort code_unit) noexcept
    {
        return code_unit >= 0x80U && !is_surrogate_code_unit(code_unit);
    }

    static bool is_standalone_combining_code_unit(ushort code_unit) noexcept
    {
        const QChar codepoint(code_unit);
        switch (codepoint.category()) {
            case QChar::Mark_NonSpacing:
            case QChar::Mark_SpacingCombining:
            case QChar::Mark_Enclosing:
                return true;
            default:
                return codepoint.combiningClass() != 0U;
        }
    }

    static bool is_terminal_graphic_source_cell_code_unit(ushort code_unit) noexcept
    {
        return code_unit >= 0x2500U && code_unit <= 0x259fU;
    }

    static bool is_single_bmp_source_cell_code_unit(ushort code_unit) noexcept
    {
        return
            is_single_bmp_code_unit(code_unit) &&
            (is_terminal_graphic_source_cell_code_unit(code_unit) ||
             !is_standalone_combining_code_unit(code_unit));
    }

    static Terminal_render_cell_text inline_single_bmp(ushort code_unit) noexcept
    {
        Q_ASSERT(is_single_bmp_code_unit(code_unit));

        Terminal_render_cell_text text;
        text.m_storage          = Terminal_render_cell_text_storage::INLINE_SINGLE_BMP;
        text.m_inline_code_unit = code_unit;
        return text;
    }

    static Terminal_render_cell_text from_qstring(QString text)
    {
        if (text.isEmpty()) {
            return {};
        }

        if (text.size() == 1 && is_printable_ascii_code_unit(text.at(0).unicode())) {
            return inline_printable_ascii(text.at(0).unicode());
        }

        return fallback(std::move(text));
    }

    static Terminal_render_cell_text fallback_copy(const QString& text)
    {
        if (text.isEmpty()) {
            return {};
        }

        Terminal_render_cell_text value;
        value.m_storage       = Terminal_render_cell_text_storage::FALLBACK_QSTRING;
        value.m_fallback_text = std::make_unique<QString>(text);
        return value;
    }

    static Terminal_render_cell_text_category category_for_qstring(QStringView text)
    {
        if (text.isEmpty()) {
            return Terminal_render_cell_text_category::EMPTY;
        }

        constexpr unsigned int printable_ascii_first = 0x20U;
        constexpr unsigned int printable_ascii_last  = 0x7eU;

        unsigned int outside_printable_ascii = 0U;
        unsigned int non_ascii               = 0U;
        const qsizetype text_size            = text.size();
        const QChar* characters              = text.data();
        for (qsizetype index = 0; index < text_size; ++index) {
            const unsigned int code_unit = characters[index].unicode();
            outside_printable_ascii |= static_cast<unsigned int>(
                code_unit - printable_ascii_first >
                    printable_ascii_last - printable_ascii_first);
            non_ascii |= code_unit;
        }

        if (outside_printable_ascii == 0U) {
            return Terminal_render_cell_text_category::PRINTABLE_ASCII;
        }

        return (non_ascii & ~0x7fU) != 0U
            ? Terminal_render_cell_text_category::NON_ASCII
            : Terminal_render_cell_text_category::OTHER_ASCII;
    }

    void copy_from(const Terminal_render_cell_text& other)
    {
        m_storage          = other.m_storage;
        m_inline_code_unit = other.m_inline_code_unit;
        m_fallback_text.reset();
        switch (other.m_storage) {
            case Terminal_render_cell_text_storage::EMPTY:
                return;
            case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
            case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                return;
            case Terminal_render_cell_text_storage::FALLBACK_QSTRING:
                if (other.m_fallback_text != nullptr) {
                    m_fallback_text = std::make_unique<QString>(*other.m_fallback_text);
                }
                return;
        }
    }

    void move_from(Terminal_render_cell_text&& other)
    {
        m_storage          = other.m_storage;
        m_inline_code_unit = other.m_inline_code_unit;
        m_fallback_text    = std::move(other.m_fallback_text);

        other.m_storage          = Terminal_render_cell_text_storage::EMPTY;
        other.m_inline_code_unit = 0U;
    }

    Terminal_render_cell_text_storage m_storage =
        Terminal_render_cell_text_storage::EMPTY;
    ushort                            m_inline_code_unit = 0U;
    std::unique_ptr<QString>          m_fallback_text;
};

static_assert(sizeof(Terminal_render_cell_text) <= 16U);

inline bool operator==(
    const Terminal_render_cell_text& left,
    const Terminal_render_cell_text& right)
{
    if (left.storage() == Terminal_render_cell_text_storage::FALLBACK_QSTRING &&
        right.storage() == Terminal_render_cell_text_storage::FALLBACK_QSTRING)
    {
        return *left.fallback_qstring_or_null() == *right.fallback_qstring_or_null();
    }

    if (left.storage() == Terminal_render_cell_text_storage::FALLBACK_QSTRING) {
        return right.equals(QStringView(*left.fallback_qstring_or_null()));
    }

    if (right.storage() == Terminal_render_cell_text_storage::FALLBACK_QSTRING) {
        return left.equals(QStringView(*right.fallback_qstring_or_null()));
    }

    return left.single_code_unit() == right.single_code_unit();
}

inline bool operator!=(
    const Terminal_render_cell_text& left,
    const Terminal_render_cell_text& right)
{
    return !(left == right);
}

inline bool operator==(const Terminal_render_cell_text& left, const QString& right)
{
    return left.equals(QStringView(right));
}

inline bool operator==(const QString& left, const Terminal_render_cell_text& right)
{
    return right.equals(QStringView(left));
}

inline bool operator!=(const Terminal_render_cell_text& left, const QString& right)
{
    return !(left == right);
}

inline bool operator!=(const QString& left, const Terminal_render_cell_text& right)
{
    return !(left == right);
}

}
