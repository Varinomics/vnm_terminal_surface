#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vnm_terminal {

enum class Terminal_selection_buffer : std::uint8_t
{
    PRIMARY_SCREEN,
    ALTERNATE_SCREEN,
};

enum class Terminal_touch_selection_action
{
    SELECT_WORD,
    BEGIN_HANDLE,
    MOVE_HANDLE,
    SCROLL_EXTEND,
    END_GESTURE,
    CANCEL_GESTURE,
    CLEAR,
    COPY,
    INSPECT_WORD,
};

enum class Terminal_selection_handle { START, END };
enum class Terminal_touch_menu { COPY = 0, PASTE = 1, LEGACY_COPY = 3 };

enum class Terminal_touch_selection_status
{
    OK,
    STALE_SOURCE,
    STALE_SELECTION,
    UNAVAILABLE,
    OVER_LIMIT,
    INVALID_REQUEST,
    INDETERMINATE,
};

struct terminal_selection_source_t
{
    std::uint64_t session_epoch          = 0U;
    Terminal_selection_buffer buffer    = Terminal_selection_buffer::PRIMARY_SCREEN;
    std::uint64_t grid_reflow_generation = 0U;
    int           rows                  = 0;
    int           columns               = 0;

    bool operator==(const terminal_selection_source_t&) const = default;
};

struct terminal_selection_row_t
{
    std::uint64_t retained_line_id      = 0U;
    std::uint64_t content_generation    = 0U;
    int           visual_fragment_index = 0;

    bool operator==(const terminal_selection_row_t&) const = default;
};

struct terminal_selection_point_t
{
    terminal_selection_row_t row;
    int                      column = 0;

    bool operator==(const terminal_selection_point_t&) const = default;
};

struct Terminal_touch_selection_request
{
    std::uint64_t request_id = 0U;
    QString gesture_id;
    Terminal_touch_selection_action action = Terminal_touch_selection_action::SELECT_WORD;
    terminal_selection_source_t source;
    terminal_selection_point_t target;
    Terminal_selection_handle handle = Terminal_selection_handle::END;
    std::uint64_t expected_selection_generation = 0U;
    int scroll_lines = 0;

    bool operator==(const Terminal_touch_selection_request&) const = default;
};

struct Terminal_touch_selection_result
{
    std::uint64_t request_id = 0U;
    Terminal_touch_selection_action action = Terminal_touch_selection_action::SELECT_WORD;
    Terminal_touch_selection_status status = Terminal_touch_selection_status::UNAVAILABLE;
    std::uint64_t selection_generation = 0U;
    QString text;
    QString reason;
    std::optional<bool> word_available;

    bool operator==(const Terminal_touch_selection_result&) const = default;
};

struct terminal_canvas_selection_endpoint_t
{
    int  row     = 0;
    int  column  = 0;
    bool visible = false;

    bool operator==(const terminal_canvas_selection_endpoint_t&) const = default;
};

struct terminal_canvas_selection_span_t
{
    int row          = 0;
    int first_column = 0;
    int column_count = 0;

    bool operator==(const terminal_canvas_selection_span_t&) const = default;
};

inline constexpr std::uint16_t k_terminal_canvas_selection_version = 1U;
inline constexpr std::uint16_t k_terminal_word_query_version = 1U;
inline constexpr qsizetype k_terminal_touch_selection_max_text_utf8_bytes = 1024 * 1024;

// This optional record belongs to its enclosing immutable frame. Unknown
// versions leave touch selection unavailable without rejecting terminal text.
struct Terminal_canvas_selection
{
    std::uint16_t record_version = k_terminal_canvas_selection_version;
    terminal_selection_source_t source;
    std::vector<terminal_selection_row_t> visible_rows;
    std::uint64_t selection_generation = 0U;
    bool has_selection = false;
    bool touch_handles_visible = false;
    terminal_canvas_selection_endpoint_t start;
    terminal_canvas_selection_endpoint_t end;
    std::vector<terminal_canvas_selection_span_t> spans;
    quint32 background_rgba = 0xff3874f2U;
    quint32 foreground_rgba = 0xffffffffU;
    // A separate optional capability attachment carries this on the wire.
    // The existing selection record remains version 1 and keeps its shape.
    std::optional<std::uint16_t> word_query_version;

    bool operator==(const Terminal_canvas_selection&) const = default;
};

inline bool terminal_canvas_selection_is_valid(
    const Terminal_canvas_selection& selection, int rows, int columns)
{
    if (selection.record_version != k_terminal_canvas_selection_version) {
        return true;
    }
    if (selection.source.session_epoch == 0U || selection.source.rows != rows ||
        selection.source.columns != columns || selection.selection_generation == 0U ||
        selection.visible_rows.size() != (std::size_t)rows || selection.spans.size() > (std::size_t)rows ||
        (selection.source.buffer != Terminal_selection_buffer::PRIMARY_SCREEN &&
            selection.source.buffer != Terminal_selection_buffer::ALTERNATE_SCREEN) ||
        (!selection.has_selection && (selection.touch_handles_visible || !selection.spans.empty())))
    {
        return false;
    }
    for (const auto& row : selection.visible_rows) {
        if (row.retained_line_id == 0U || row.visual_fragment_index != 0) {
            return false;
        }
    }
    for (const auto endpoint : {selection.start, selection.end}) {
        if (endpoint.visible &&
            (endpoint.row < 0 || endpoint.row >= rows || endpoint.column < 0 || endpoint.column > columns))
        {
            return false;
        }
    }
    int previous_row = -1;
    for (const auto& span : selection.spans) {
        if (span.row <= previous_row || span.row >= rows || span.first_column < 0 ||
            span.column_count <= 0 || span.first_column > columns - span.column_count)
        {
            return false;
        }
        previous_row = span.row;
    }
    return true;
}

} // namespace vnm_terminal

Q_DECLARE_METATYPE(vnm_terminal::Terminal_touch_selection_request)
Q_DECLARE_METATYPE(vnm_terminal::Terminal_touch_selection_result)
Q_DECLARE_METATYPE(vnm_terminal::Terminal_touch_menu)
