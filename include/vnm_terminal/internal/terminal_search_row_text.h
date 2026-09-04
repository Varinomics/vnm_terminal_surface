#pragma once

#include <QStringView>

#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

// Grid columns that produced one emitted UTF-16 code unit. end_column is the
// exclusive end of the producing cell, so a wide cell reports its full span.
struct terminal_search_column_span_t
{
    std::int32_t                   first_column = 0;
    std::int32_t                   end_column   = 0;

    friend bool operator==(
        const terminal_search_column_span_t&,
        const terminal_search_column_span_t&) = default;
};

// Searchable text of one physical row. source_width records its geometry so a
// consumer can bound stored rows to the live grid. Non-identity spans carry
// exactly one entry per code unit. identity_spans stays true while every entry
// would be {index, index + 1}, the ordinary single-width single-code-unit case,
// so those rows keep no explicit spans.
struct Terminal_search_row_text
{
    std::vector<char16_t>                      units;
    std::vector<terminal_search_column_span_t> spans;
    bool                                       identity_spans = true;
    // Width of the row as stored, before projection to the live geometry. For an
    // active-grid row this is the live column count; for a retained row it is the
    // width the row was sealed at.
    std::int32_t                               source_width   = 0;

    void clear()
    {
        units.clear();
        spans.clear();
        identity_spans = true;
        source_width   = 0;
    }

    QStringView view() const
    {
        return QStringView(units.data(), static_cast<qsizetype>(units.size()));
    }

    void append_cell_text(
        QStringView    text,
        std::int32_t   first_column,
        std::int32_t   end_column)
    {
        const terminal_search_column_span_t span{first_column, end_column};
        for (QChar code_unit : text) {
            const std::int32_t index = static_cast<std::int32_t>(units.size());
            if (identity_spans &&
                span != terminal_search_column_span_t{index, index + 1})
            {
                identity_spans = false;
                spans.reserve(units.size() + static_cast<std::size_t>(text.size()));
                for (std::int32_t prior = 0; prior < index; ++prior) {
                    spans.push_back({prior, prior + 1});
                }
            }
            if (!identity_spans) {
                spans.push_back(span);
            }
            units.push_back(code_unit.unicode());
        }
    }

    void append_padding_column(std::int32_t column)
    {
        append_cell_text(QStringView(u" "), column, column + 1);
    }
};

}
