#pragma once

#include <algorithm>
#include <optional>

namespace vnm_terminal::internal {

enum class Terminal_buffer_id
{
    PRIMARY,
    ALTERNATE,
};

// How a frame presents the primary screen's native rows. NATIVE shows them
// as they are, AUTOMATIC replaces trailing resize vacancy with history, and COMPANION
// relocates the unwritten tail into history. The alternate screen is always
// NATIVE.
enum class Terminal_presentation_policy
{
    NATIVE,
    AUTOMATIC,
    COMPANION,
};

// The policy inputs of a frame's mapping record. Native S, H and the
// presented offset o stay in the viewport fields; hidden_rows is P, the
// trailing grid rows the presentation never shows, and insertion_row is
// Companion's I = S - K, before which its P padding rows are re-inserted.
struct Terminal_presentation_mapping
{
    Terminal_presentation_policy policy        = Terminal_presentation_policy::NATIVE;
    int                          hidden_rows   = 0;
    int                          insertion_row = 0;

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

// The mapping record's policy and its P and I. The Companion toggle takes
// precedence; resize vacancy selects AUTOMATIC with P = min(V, S); the
// alternate screen is always NATIVE. Automatic displacement needs real
// history above the native screen: without it, growth must leave the first
// output at the top instead of inserting blank rows before it. Companion
// relocates at least the vacancy, since styled vacancy can be occupied down
// to the bottom row.
inline Terminal_presentation_mapping make_presentation_mapping(
    Terminal_buffer_id active_buffer,
    bool               companion_enabled,
    int                visible_rows,
    int                scrollback_rows,
    int                resize_vacancy_rows,
    int                content_bottom_row_exclusive,
    int                resize_displaced_scrollback_rows)
{
    if (active_buffer == Terminal_buffer_id::ALTERNATE) {
        return {};
    }

    if (companion_enabled) {
        return {
            Terminal_presentation_policy::COMPANION,
            std::max(visible_rows - content_bottom_row_exclusive, resize_vacancy_rows),
            scrollback_rows - resize_displaced_scrollback_rows,
        };
    }

    if (resize_vacancy_rows > 0) {
        return {Terminal_presentation_policy::AUTOMATIC, std::min(resize_vacancy_rows, scrollback_rows), 0};
    }

    return {};
}

// The policy maximum of the presented offset. NATIVE and COMPANION keep the
// native S; AUTOMATIC never scrolls its hidden rows into view.
inline int presentation_max_offset_from_tail(const Terminal_viewport_state& viewport)
{
    const int scrollback_rows = std::max(0, viewport.scrollback_rows);
    return viewport.presentation.policy == Terminal_presentation_policy::AUTOMATIC
        ? std::max(0, scrollback_rows - viewport.presentation.hidden_rows)
        : scrollback_rows;
}

inline int presentation_clamped_offset_from_tail(const Terminal_viewport_state& viewport)
{
    return std::clamp(viewport.offset_from_tail, 0, presentation_max_offset_from_tail(viewport));
}

// The presentation stream lays the policy's rows out in display order: display
// row d of the window shows stream row S - o + d. AUTOMATIC shifts native rows
// down by P, bringing available history into the enlarged window; COMPANION
// inserts its P padding rows before insertion_row.
inline int presentation_stream_row_for_logical_row(
    const Terminal_presentation_mapping& mapping,
    int                                  logical_row)
{
    switch (mapping.policy) {
        case Terminal_presentation_policy::NATIVE:    return logical_row;
        case Terminal_presentation_policy::AUTOMATIC: return logical_row + mapping.hidden_rows;
        case Terminal_presentation_policy::COMPANION:
            return logical_row < mapping.insertion_row ? logical_row : logical_row + mapping.hidden_rows;
        default:                                      return logical_row;
    }
}

// The native logical row the window requests at display_row, or -1 for an
// identity-free slot: AUTOMATIC's unused space above native row zero, or
// Companion's relocated padding.
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

    const Terminal_presentation_mapping& mapping = viewport.presentation;
    const int stream_row =
        viewport.scrollback_rows - presentation_clamped_offset_from_tail(viewport) + display_row;
    switch (mapping.policy) {
        case Terminal_presentation_policy::NATIVE:
            return stream_row;
        case Terminal_presentation_policy::AUTOMATIC:
            return stream_row >= mapping.hidden_rows ? stream_row - mapping.hidden_rows : -1;
        case Terminal_presentation_policy::COMPANION:
            if (stream_row < mapping.insertion_row) {
                return stream_row;
            }
            return stream_row < mapping.insertion_row + mapping.hidden_rows
                ? -1
                : stream_row - mapping.hidden_rows;
        default:
            return stream_row;
    }
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
    if (mapping.policy != Terminal_presentation_policy::NATIVE &&
        logical_row >= viewport.scrollback_rows + viewport.visible_rows - mapping.hidden_rows)
    {
        return std::nullopt;
    }

    const int first_stream_row =
        viewport.scrollback_rows - presentation_clamped_offset_from_tail(viewport);
    return presentation_stream_row_for_logical_row(mapping, logical_row) - first_stream_row;
}

// The presented offset that shows native logical_row at display_row, clamped
// to the policy's bounds: the detached-anchor rule.
inline int presentation_offset_for_logical_row(
    const Terminal_viewport_state& viewport,
    int                            logical_row,
    int                            display_row)
{
    const int stream_row = presentation_stream_row_for_logical_row(viewport.presentation, logical_row);
    return std::clamp(
        viewport.scrollback_rows - stream_row + display_row,
        0,
        presentation_max_offset_from_tail(viewport));
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

// The native rows the window requests above the native view, which starts at
// logical row S - o: the rows a frame acquires besides its own.
inline int presentation_leading_row_count(const Terminal_viewport_state& viewport)
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return 0;
    }

    const int native_first_row = viewport.scrollback_rows - viewport.offset_from_tail;
    int first_requested_row = native_first_row;
    for (int row = 0; row < viewport.visible_rows; ++row) {
        const int requested_row = presentation_requested_logical_row(viewport, row);
        if (requested_row >= 0) {
            first_requested_row = std::min(first_requested_row, requested_row);
        }
    }
    return native_first_row - first_requested_row;
}

// Availability of the window against one contiguous coverage of native rows.
// A frozen basis resolves its missing leading rows as unused slots at their
// display positions; any other missing row leaves the window unresolvable.
inline bool presentation_window_resolvable(
    const Terminal_viewport_state& viewport,
    int                            first_covered_row,
    int                            covered_rows,
    bool                           frozen_basis)
{
    for (int row = 0; row < viewport.visible_rows; ++row) {
        const int requested_row = presentation_requested_logical_row(viewport, row);
        if (requested_row < 0) {
            continue;
        }

        // Compared as distances, so coverage ending at the largest logical
        // row cannot overflow.
        if (requested_row < first_covered_row) {
            if (!frozen_basis) {
                return false;
            }
            continue;
        }
        if (requested_row - first_covered_row >= covered_rows) {
            return false;
        }
    }
    return true;
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
