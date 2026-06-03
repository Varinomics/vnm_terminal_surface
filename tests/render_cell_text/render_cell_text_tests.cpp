#include "vnm_terminal/internal/terminal_render_cell_text.h"
#include "helpers/test_check.h"

#include <QChar>
#include <QString>
#include <string>
#include <string_view>
#include <utility>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool check_labeled(
    bool             condition,
    std::string_view label,
    std::string_view property)
{
    const std::string message =
        std::string(label) + " " + std::string(property);
    return check(condition, message);
}

QString appended_text(const term::Terminal_render_cell_text& value)
{
    QString text;
    value.append_to(text);
    return text;
}

bool check_payload(
    const term::Terminal_render_cell_text&  value,
    const QString&                          expected_text,
    term::Terminal_render_cell_text_storage expected_storage,
    term::Terminal_render_cell_text_category
                                            expected_category,
    qsizetype                               expected_code_units,
    std::string_view                        label)
{
    bool ok = true;

    ok &= check_labeled(value.is_valid(), label, "is valid");
    ok &= check_labeled(value.storage() == expected_storage, label, "storage");
    ok &= check_labeled(value.category() == expected_category, label, "category");
    ok &= check_labeled(value.code_unit_count() == expected_code_units,
        label, "code-unit count");
    ok &= check_labeled(value.to_qstring() == expected_text, label, "to_qstring");
    ok &= check_labeled(appended_text(value) == expected_text, label, "append_to");

    return ok;
}

bool test_default_empty_behavior()
{
    bool ok = true;

    const term::Terminal_render_cell_text value;
    ok &= check_payload(
        value,
        QString(),
        term::Terminal_render_cell_text_storage::EMPTY,
        term::Terminal_render_cell_text_category::EMPTY,
        0,
        "default empty");
    ok &= check(value.is_empty(), "default is empty");
    ok &= check(!value.is_inline_printable_ascii(), "default is not inline ASCII");
    ok &= check(!value.is_inline_single_bmp(), "default is not inline BMP");
    ok &= check(!value.is_fallback_qstring(), "default is not fallback QString");
    ok &= check(!value.single_code_unit().has_value(), "default has no code unit");
    ok &= check(!value.single_bmp_code_unit().has_value(),
        "default has no BMP code unit");
    ok &= check(!value.single_printable_ascii_code_unit().has_value(),
        "default has no printable ASCII code unit");
    ok &= check(!value.single_printable_ascii_char().has_value(),
        "default has no printable ASCII char");
    ok &= check(value.fallback_qstring_or_null() == nullptr,
        "default has no fallback QString");

    const term::Terminal_render_cell_text explicit_empty =
        term::Terminal_render_cell_text::empty();
    ok &= check_payload(
        explicit_empty,
        QString(),
        term::Terminal_render_cell_text_storage::EMPTY,
        term::Terminal_render_cell_text_category::EMPTY,
        0,
        "explicit empty");

    return ok;
}

bool test_inline_printable_ascii_behavior()
{
    bool ok = true;

    const term::Terminal_render_cell_text value(QStringLiteral("A"));
    ok &= check_payload(
        value,
        QStringLiteral("A"),
        term::Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        1,
        "inline printable ASCII");
    ok &= check(value.is_inline_printable_ascii(), "ASCII is inline");
    ok &= check(!value.is_inline_single_bmp(), "ASCII is not inline BMP");
    ok &= check(!value.is_fallback_qstring(), "ASCII is not fallback");
    ok &= check(value.single_code_unit().has_value() &&
        value.single_code_unit().value() == static_cast<ushort>('A'),
        "ASCII exposes single code unit");
    ok &= check(value.single_printable_ascii_code_unit().has_value() &&
        value.single_printable_ascii_code_unit().value() == static_cast<ushort>('A'),
        "ASCII exposes printable code unit");
    ok &= check(value.single_printable_ascii_char().has_value() &&
        value.single_printable_ascii_char().value() == QChar('A'),
        "ASCII exposes printable char");
    ok &= check(value.fallback_qstring_or_null() == nullptr,
        "ASCII has no fallback QString");
    ok &= check(value == QStringLiteral("A"), "ASCII equals QString");
    ok &= check(QStringLiteral("A") == value, "QString equals ASCII");
    ok &= check(value == term::Terminal_render_cell_text::inline_printable_ascii(
        static_cast<ushort>('A')),
        "ASCII equals another inline payload");
    ok &= check(value != QStringLiteral("B"), "ASCII differs from other QString");
    ok &= check(value != term::Terminal_render_cell_text(QStringLiteral("B")),
        "ASCII differs from other payload");

    ok &= check_payload(
        term::Terminal_render_cell_text(QStringLiteral(" ")),
        QStringLiteral(" "),
        term::Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        1,
        "space inline printable ASCII");
    ok &= check_payload(
        term::Terminal_render_cell_text(QStringLiteral("~")),
        QStringLiteral("~"),
        term::Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        1,
        "tilde inline printable ASCII");

    return ok;
}

bool test_non_ascii_single_code_point_falls_back()
{
    bool ok = true;

    const QString cjk = QStringLiteral("\u754c");
    const term::Terminal_render_cell_text value(cjk);
    ok &= check_payload(
        value,
        cjk,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "single non-ASCII fallback");
    ok &= check(!value.is_inline_printable_ascii(),
        "single non-ASCII is not inline");
    ok &= check(value.is_fallback_qstring(),
        "single non-ASCII uses fallback QString");
    ok &= check(value.fallback_qstring_or_null() != nullptr &&
        *value.fallback_qstring_or_null() == cjk,
        "single non-ASCII owns fallback text");
    ok &= check(value.single_code_unit().has_value() &&
        value.single_code_unit().value() == static_cast<ushort>(0x754cU),
        "single non-ASCII exposes one UTF-16 code unit");
    ok &= check(!value.single_printable_ascii_code_unit().has_value(),
        "single non-ASCII has no printable ASCII helper");
    ok &= check(value.single_bmp_code_unit().has_value() &&
        value.single_bmp_code_unit().value() == static_cast<ushort>(0x754cU),
        "single non-ASCII fallback exposes BMP helper");
    ok &= check(value == cjk, "single non-ASCII equals QString");
    ok &= check(value == term::Terminal_render_cell_text::fallback(cjk),
        "single non-ASCII equals explicit fallback payload");

    return ok;
}

bool test_multi_code_unit_fallback_behavior()
{
    bool ok = true;

    const QString two_ascii = QStringLiteral("AB");
    const term::Terminal_render_cell_text ascii_value(two_ascii);
    ok &= check_payload(
        ascii_value,
        two_ascii,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        2,
        "multi ASCII fallback");
    ok &= check(!ascii_value.single_code_unit().has_value(),
        "multi ASCII has no single code unit");
    ok &= check(!ascii_value.single_printable_ascii_code_unit().has_value(),
        "multi ASCII has no printable helper");

    const QString other_ascii = QStringLiteral("A\n");
    ok &= check_payload(
        term::Terminal_render_cell_text(other_ascii),
        other_ascii,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::OTHER_ASCII,
        2,
        "other ASCII fallback");

    const QString emoji = QString::fromUcs4(U"\U0001F600");
    ok &= check_payload(
        term::Terminal_render_cell_text(emoji),
        emoji,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        2,
        "supplementary fallback");

    return ok;
}

bool test_from_source_cell_semantics()
{
    bool ok = true;

    const QString ascii = QStringLiteral("A");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(ascii, 1, false),
        ascii,
        term::Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        1,
        "source ASCII width one");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(ascii, 2, false),
        ascii,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        1,
        "source ASCII non-unit width");
    ok &= check(
        term::Terminal_render_cell_text::from_source_cell(ascii, 2, false).
            single_printable_ascii_code_unit().has_value(),
        "source ASCII fallback still exposes printable helper");

    const QString box_graphic   = QStringLiteral("\u2500");
    const QString block_graphic = QStringLiteral("\u2588");
    const QString cjk           = QStringLiteral("\u754c");
    const term::Terminal_render_cell_text box_source =
        term::Terminal_render_cell_text::from_source_cell(box_graphic, 1, false);
    ok &= check_payload(
        box_source,
        box_graphic,
        term::Terminal_render_cell_text_storage::INLINE_SINGLE_BMP,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source box drawing BMP");
    ok &= check(box_source.is_inline_single_bmp() &&
        !box_source.is_fallback_qstring() &&
        box_source.fallback_qstring_or_null() == nullptr &&
        box_source.single_bmp_code_unit().value_or(0U) == 0x2500U,
        "source box drawing stores only an inline BMP code unit");

    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(block_graphic, 1, false),
        block_graphic,
        term::Terminal_render_cell_text_storage::INLINE_SINGLE_BMP,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source block graphic BMP");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(cjk, 2, false),
        cjk,
        term::Terminal_render_cell_text_storage::INLINE_SINGLE_BMP,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source CJK leading BMP");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(cjk, 0, false),
        cjk,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source BMP width zero fallback");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(cjk, -1, false),
        cjk,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source BMP negative width fallback");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(cjk, 3, false),
        cjk,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source BMP oversized width fallback");

    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(ascii, 1, true),
        QString(),
        term::Terminal_render_cell_text_storage::EMPTY,
        term::Terminal_render_cell_text_category::EMPTY,
        0,
        "source wide continuation");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(cjk, 0, true),
        QString(),
        term::Terminal_render_cell_text_storage::EMPTY,
        term::Terminal_render_cell_text_category::EMPTY,
        0,
        "source BMP wide continuation");

    const QString combining_cluster = QStringLiteral("e\u0301");
    const QString combining_mark(QChar(0x0301));
    const QString class_zero_mark(QChar(0x20dd));
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(combining_cluster, 1, false),
        combining_cluster,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        2,
        "source combining cluster fallback");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(combining_mark, 1, false),
        combining_mark,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source standalone BMP combining mark fallback");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(class_zero_mark, 1, false),
        class_zero_mark,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source class-zero standalone BMP mark fallback");

    const QString emoji = QString::fromUcs4(U"\U0001F600");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(emoji, 2, false),
        emoji,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        2,
        "source emoji fallback");

    const QString surrogate_half(QChar(0xd800));
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(surrogate_half, 1, false),
        surrogate_half,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        1,
        "source surrogate half fallback");

    const QString multi_bmp = QStringLiteral("\u2500\u2500");
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(multi_bmp, 2, false),
        multi_bmp,
        term::Terminal_render_cell_text_storage::FALLBACK_QSTRING,
        term::Terminal_render_cell_text_category::NON_ASCII,
        2,
        "source multi-BMP fallback");

    const QString empty;
    ok &= check_payload(
        term::Terminal_render_cell_text::from_source_cell(empty, 1, false),
        QString(),
        term::Terminal_render_cell_text_storage::EMPTY,
        term::Terminal_render_cell_text_category::EMPTY,
        0,
        "source empty");

    return ok;
}

bool test_copy_move_assignment_value_semantics()
{
    bool ok = true;

    const QString fallback_text = QStringLiteral("a\u0301");
    term::Terminal_render_cell_text fallback_original(fallback_text);
    term::Terminal_render_cell_text fallback_copy(fallback_original);
    fallback_original = QStringLiteral("Z");
    ok &= check(fallback_copy == fallback_text,
        "fallback copy keeps original text after source reassignment");
    ok &= check(fallback_original == QStringLiteral("Z"),
        "fallback source can be reassigned to inline text");

    term::Terminal_render_cell_text fallback_assigned;
    fallback_assigned = fallback_copy;
    fallback_copy = QStringLiteral("Y");
    ok &= check(fallback_assigned == fallback_text,
        "fallback copy assignment owns independent text");

    term::Terminal_render_cell_text fallback_moved(std::move(fallback_assigned));
    ok &= check(fallback_moved == fallback_text, "fallback move constructor preserves text");
    ok &= check(fallback_assigned.is_empty(), "fallback move constructor clears source");

    term::Terminal_render_cell_text fallback_move_assigned(QStringLiteral("Q"));
    fallback_move_assigned = std::move(fallback_moved);
    ok &= check(fallback_move_assigned == fallback_text,
        "fallback move assignment preserves text");
    ok &= check(fallback_moved.is_empty(), "fallback move assignment clears source");

    term::Terminal_render_cell_text inline_original(QStringLiteral("A"));
    term::Terminal_render_cell_text inline_copy(inline_original);
    inline_original = fallback_text;
    ok &= check(inline_copy == QStringLiteral("A"),
        "inline copy keeps original text after source reassignment");
    ok &= check(inline_original == fallback_text,
        "inline source can be reassigned to fallback text");

    term::Terminal_render_cell_text inline_assigned;
    inline_assigned = inline_copy;
    inline_copy = QStringLiteral("B");
    ok &= check(inline_assigned == QStringLiteral("A"),
        "inline copy assignment keeps original text");

    term::Terminal_render_cell_text inline_moved(std::move(inline_assigned));
    ok &= check(inline_moved == QStringLiteral("A"), "inline move constructor preserves text");
    ok &= check(inline_assigned.is_empty(), "inline move constructor clears source");

    term::Terminal_render_cell_text inline_move_assigned(fallback_text);
    inline_move_assigned = std::move(inline_moved);
    ok &= check(inline_move_assigned == QStringLiteral("A"),
        "inline move assignment preserves text");
    ok &= check(inline_moved.is_empty(), "inline move assignment clears source");

    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_default_empty_behavior();
    ok &= test_inline_printable_ascii_behavior();
    ok &= test_non_ascii_single_code_point_falls_back();
    ok &= test_multi_code_unit_fallback_behavior();
    ok &= test_from_source_cell_semantics();
    ok &= test_copy_move_assignment_value_semantics();
    return ok ? 0 : 1;
}
