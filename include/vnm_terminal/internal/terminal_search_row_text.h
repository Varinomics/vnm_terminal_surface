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

// Searchable text of one physical row, in the geometry the live grid uses.
// spans carries exactly one entry per code unit. identity_spans stays true while
// every entry is {index, index + 1}, the ordinary single-width single-code-unit
// case, which lets a store keep no spans at all for that row.
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
            identity_spans = identity_spans &&
                span == terminal_search_column_span_t{index, index + 1};
            units.push_back(code_unit.unicode());
            spans.push_back(span);
        }
    }

    void append_padding_column(std::int32_t column)
    {
        append_cell_text(QStringView(u" "), column, column + 1);
    }
};

}
