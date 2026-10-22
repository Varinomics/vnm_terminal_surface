#pragma once

#include <algorithm>
#include <optional>

namespace vnm_terminal::internal {

enum class Terminal_buffer_id
{
    PRIMARY,
    ALTERNATE,
};

// The alternate screen keeps its native layout. A primary transcript is its
// native rows through the current semantic content end; a document that fits
// the view starts on the top row, and a longer one ends on the bottom row.
enum class Terminal_presentation_policy
{
    NATIVE,
    CONTENT_TAIL,
};

// Immutable native logical boundaries, acquired with the frame's rows. The
// end includes occupied cells/images and the logical cursor even when hidden.
// The latest explicit page clear (ED 2 or a host clear) establishes the
// boundary before that page; the history above it stays in the scroll range.
// Consumer capacity determines how many identity-free slots follow the end.
struct Terminal_presentation_mapping
{
    Terminal_presentation_policy policy = Terminal_presentation_policy::NATIVE;
    int                          content_end_row_exclusive = 0;
    int                          clear_page_start_row = 0;

    bool operator==(const Terminal_presentation_mapping&) const = default;
};

enum class Terminal_viewport_result_code
{
    ACCEPTED,
    INVALID_VISIBLE_ROWS,
};

enum class Terminal_alternate_screen_scroll_policy
{
    KEEP_AT_TAIL,
    WHEEL_TO_TERMINAL_INPUT,
};

enum class Terminal_viewport_scroll_action
{
    VIEWPORT_MOVED,
    AT_BOUNDARY,
    DEFERRED_INTENT_RECORDED,
    TERMINAL_INPUT,
};

enum class Terminal_viewport_coordinate_result_code
{
    OK,
    INVALID_VIEWPORT_ROW,
};

struct Terminal_viewport_state
{
    Terminal_buffer_id active_buffer    = Terminal_buffer_id::PRIMARY;
    int                scrollback_rows  = 0;
    int                visible_rows     = 1;
    int                offset_from_tail = 0;
    bool               follow_tail      = true;
    Terminal_alternate_screen_scroll_policy alternate_screen_scroll_policy =
        Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL;
    // Which native rows the offset presents; the offset is measured in the
    // presentation, so every clamp uses presentation_max_offset_from_tail().
    Terminal_presentation_mapping presentation;
};

// The mapping fields decide which logical rows the physical viewport rows
// name; the remaining fields (follow_tail, alternate-screen scroll policy)
// are scroll behavior, not row identity. The presentation is not part of the
// native mapping: it changes which rows are displayed, not what the native
// rows are.
inline bool viewport_mappings_match(
    const Terminal_viewport_state& left,
    const Terminal_viewport_state& right)
{
    return
        left.active_buffer    == right.active_buffer    &&
        left.visible_rows     == right.visible_rows     &&
        left.scrollback_rows  == right.scrollback_rows  &&
        left.offset_from_tail == right.offset_from_tail;
}

inline Terminal_presentation_mapping make_presentation_mapping(
    Terminal_buffer_id active_buffer,
    int                scrollback_rows,
    int                content_bottom_row_exclusive,
    int                clear_page_start_row)
{
    if (active_buffer == Terminal_buffer_id::ALTERNATE) {
        return {};
    }

    return {
        Terminal_presentation_policy::CONTENT_TAIL,
        scrollback_rows + content_bottom_row_exclusive,
        clear_page_start_row,
    };
}

// The document runs from D, the later of the first retained row and the
// clear page boundary, to the content end E. Retained rows start at logical
// row 0 and the boundary is rebased with them, so D is the boundary. A
// document shorter than the view is followed by this many unused slots: the
// blank rest of a cleared or fresh screen.
inline int presentation_padding_rows(const Terminal_viewport_state& viewport)
{
    if (viewport.presentation.policy != Terminal_presentation_policy::CONTENT_TAIL) {
        return 0;
    }

    const auto& mapping = viewport.presentation;
    return std::max(0, viewport.visible_rows -
        (mapping.content_end_row_exclusive - mapping.clear_page_start_row));
}

inline long long presentation_stream_end_row_exclusive(const Terminal_viewport_state& viewport)
{
    if (viewport.presentation.policy == Terminal_presentation_policy::CONTENT_TAIL) {
        return static_cast<long long>(viewport.presentation.content_end_row_exclusive) +
            presentation_padding_rows(viewport);
    }
    return static_cast<long long>(viewport.scrollback_rows) + viewport.visible_rows;
}

// Every allowed window ends at or before the same semantic end plus its
// slots. The tail window of a short document starts at D; the history above
// D stays reachable by scrolling up.
inline int presentation_max_offset_from_tail(const Terminal_viewport_state& viewport)
{
    return static_cast<int>(std::max(
        0LL, presentation_stream_end_row_exclusive(viewport) - viewport.visible_rows));
}

inline int presentation_clamped_offset_from_tail(const Terminal_viewport_state& viewport)
{
    return std::clamp(viewport.offset_from_tail, 0, presentation_max_offset_from_tail(viewport));
}

// The presented stream is native [0,E), then P unused slots, so a native
// row's stream position is its logical row. P is derived for this consumer,
// so a phone and desktop can share E/Q without sharing their height. Returns
// the native logical row a display row presents, or -1 for a slot.
inline int presentation_requested_logical_row(
    const Terminal_viewport_state& viewport,
    int                            display_row)
{
    if (display_row < 0 || display_row >= viewport.visible_rows) {
        return -1;
    }

    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return display_row;
    }

    const auto& mapping = viewport.presentation;
    const long long stream_row = presentation_stream_end_row_exclusive(viewport) -
        viewport.visible_rows - presentation_clamped_offset_from_tail(viewport) + display_row;
    if (stream_row < 0) {
        return -1;
    }
    if (mapping.policy == Terminal_presentation_policy::CONTENT_TAIL &&
        stream_row >= mapping.content_end_row_exclusive)
    {
        return -1;
    }
    return static_cast<int>(stream_row);
}

// The display row presenting native logical_row, outside [0, H) when the
// window does not reach it, or nullopt for a trailing row the policy never
// presents.
inline std::optional<int> presentation_display_row_for_logical_row(
    const Terminal_viewport_state& viewport,
    int                            logical_row)
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return logical_row;
    }

    const Terminal_presentation_mapping& mapping = viewport.presentation;
    if (logical_row < 0 ||
        (mapping.policy == Terminal_presentation_policy::CONTENT_TAIL &&
         logical_row >= mapping.content_end_row_exclusive))
    {
        return std::nullopt;
    }

    const long long first_stream_row = presentation_stream_end_row_exclusive(viewport) -
        viewport.visible_rows - presentation_clamped_offset_from_tail(viewport);
    return static_cast<int>(logical_row - first_stream_row);
}

// The presented offset that shows native logical_row at display_row, clamped
// to the policy's bounds: the detached-anchor rule.
inline int presentation_offset_for_logical_row(
    const Terminal_viewport_state& viewport,
    int                            logical_row,
    int                            display_row)
{
    return static_cast<int>(std::clamp(
        presentation_stream_end_row_exclusive(viewport) - viewport.visible_rows - logical_row + display_row,
        0LL,
        static_cast<long long>(presentation_max_offset_from_tail(viewport))));
}

// The row a detached view keeps in place (DESIGN 5.5): the first display row
// whose requested native row lies in the coverage, and its display position.
struct Terminal_presentation_anchor
{
    int logical_row = 0;
    int display_row = 0;
};

inline std::optional<Terminal_presentation_anchor> presentation_first_displayed_row(
    const Terminal_viewport_state& viewport,
    int                            first_covered_row,
    int                            covered_rows)
{
    for (int row = 0; row < viewport.visible_rows; ++row) {
        const int logical_row = presentation_requested_logical_row(viewport, row);
        if (logical_row >= 0 &&
            logical_row >= first_covered_row &&
            logical_row - first_covered_row < covered_rows)
        {
            return Terminal_presentation_anchor{logical_row, row};
        }
    }
    return std::nullopt;
}

struct Terminal_presentation_row_range
{
    int first_row = 0;
    int row_count = 0;
};

// Padding has no native identity. Removing it from a displayed window leaves
// one contiguous native interval, which can extend on either side of a
// differently sized consumer's native source window.
inline Terminal_presentation_row_range presentation_requested_row_range(
    const Terminal_viewport_state& viewport)
{
    Terminal_presentation_row_range range;
    for (int row = 0; row < viewport.visible_rows; ++row) {
        const int requested_row = presentation_requested_logical_row(viewport, row);
        if (requested_row < 0) {
            continue;
        }
        if (range.row_count == 0) {
            range.first_row = requested_row;
        }
        range.row_count = requested_row - range.first_row + 1;
    }
    return range;
}

struct Terminal_viewport_result
{
    Terminal_viewport_result_code code = Terminal_viewport_result_code::ACCEPTED;
};

struct Terminal_viewport_scroll_result
{
    Terminal_viewport_scroll_action action = Terminal_viewport_scroll_action::AT_BOUNDARY;
    int                             applied_line_delta = 0;
};

struct Terminal_viewport_coordinate_result
{
    Terminal_viewport_coordinate_result_code code = Terminal_viewport_coordinate_result_code::OK;
    int logical_row = 0;
};

class Terminal_viewport_controller
{
public:
    const Terminal_viewport_state& state() const { return m_state; }

    Terminal_viewport_result set_visible_rows(int rows);

    void set_scrollback_rows(int rows);
    // The primary screen's mapping, which bounds its presented offset; a
    // detached view keeps its first displayed row in place.
    void set_presentation(Terminal_presentation_mapping presentation);

    Terminal_viewport_scroll_result scroll_lines(int line_delta);
    Terminal_viewport_scroll_result scroll_pages(int page_delta);

    void return_to_tail();
    void notify_user_input();

    void enter_alternate_screen();
    void leave_alternate_screen();

    void set_alternate_screen_scroll_policy(Terminal_alternate_screen_scroll_policy policy);

    int max_offset_from_tail() const;
    int first_visible_logical_row() const;

    Terminal_viewport_coordinate_result viewport_row_to_logical_row(int viewport_row) const;

private:
    Terminal_viewport_state m_state;
    Terminal_viewport_state m_primary_state;
};

}
