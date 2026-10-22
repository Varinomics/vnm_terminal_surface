#include "vnm_terminal/internal/viewport_contract.h"

#include <algorithm>
#include <limits>

namespace vnm_terminal::internal {

namespace {

int state_max_offset_from_tail(const Terminal_viewport_state& state)
{
    return presentation_max_offset_from_tail(state);
}

void clamp_state_offset(Terminal_viewport_state& state)
{
    state.offset_from_tail =
        std::clamp(state.offset_from_tail, 0, state_max_offset_from_tail(state));
    state.follow_tail = state.offset_from_tail == 0;
}

void apply_scrollback_rows(Terminal_viewport_state& state, int rows)
{
    const int old_rows = state.scrollback_rows;
    state.scrollback_rows = std::max(0, rows);
    if (state.presentation.policy == Terminal_presentation_policy::CONTENT_TAIL) {
        const int delta = state.scrollback_rows - old_rows;
        state.presentation.content_end_row_exclusive += delta;
        state.presentation.clear_page_start_row = std::clamp(
            state.presentation.clear_page_start_row + std::min(0, delta),
            0,
            state.scrollback_rows);
    }

    if (!state.follow_tail && state.scrollback_rows > old_rows) {
        state.offset_from_tail += state.scrollback_rows - old_rows;
    }

    if (state.follow_tail) {
        state.offset_from_tail = 0;
    }

    clamp_state_offset(state);
}

std::optional<Terminal_presentation_anchor> detached_anchor(const Terminal_viewport_state& state)
{
    if (state.follow_tail) {
        return std::nullopt;
    }
    const int end_row = state.presentation.policy == Terminal_presentation_policy::CONTENT_TAIL
        ? state.presentation.content_end_row_exclusive
        : state.scrollback_rows + state.visible_rows;
    return presentation_first_displayed_row(state, 0, end_row);
}

void apply_visible_rows(Terminal_viewport_state& state, int rows)
{
    const auto anchor = detached_anchor(state);
    state.visible_rows = rows;
    if (anchor.has_value()) {
        state.offset_from_tail =
            presentation_offset_for_logical_row(state, anchor->logical_row, anchor->display_row);
    }
    clamp_state_offset(state);
}

}

Terminal_viewport_result Terminal_viewport_controller::set_visible_rows(int rows)
{
    if (rows <= 0) {
        return {Terminal_viewport_result_code::INVALID_VISIBLE_ROWS};
    }

    apply_visible_rows(m_state, rows);
    apply_visible_rows(m_primary_state, rows);
    return {};
}

void Terminal_viewport_controller::set_scrollback_rows(int rows)
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        apply_scrollback_rows(m_primary_state, rows);
        return;
    }

    apply_scrollback_rows(m_state, rows);
}

// A detached view keeps its first displayed row at the same display position
// under the new mapping, clamped to the new bounds; a view at the tail stays
// there. While the alternate screen is active this is the saved primary view.
void Terminal_viewport_controller::set_presentation(Terminal_presentation_mapping presentation)
{
    Terminal_viewport_state& primary = m_state.active_buffer == Terminal_buffer_id::ALTERNATE
        ? m_primary_state
        : m_state;
    const auto anchor = detached_anchor(primary);
    primary.presentation = presentation;
    if (anchor.has_value()) {
        primary.offset_from_tail =
            presentation_offset_for_logical_row(primary, anchor->logical_row, anchor->display_row);
    }
    clamp_state_offset(primary);
}

Terminal_viewport_scroll_result Terminal_viewport_controller::scroll_lines(int line_delta)
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        if (m_state.alternate_screen_scroll_policy ==
            Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT)
        {
            return {Terminal_viewport_scroll_action::TERMINAL_INPUT, 0};
        }

        return {};
    }

    const int previous_offset = m_state.offset_from_tail;
    const long long requested_offset =
        static_cast<long long>(previous_offset) + line_delta;
    m_state.offset_from_tail = static_cast<int>(
        std::clamp(requested_offset, 0LL, static_cast<long long>(state_max_offset_from_tail(m_state))));
    m_state.follow_tail      = m_state.offset_from_tail == 0;

    if (m_state.offset_from_tail == previous_offset) {
        return {};
    }

    return {
        Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        m_state.offset_from_tail - previous_offset,
    };
}

Terminal_viewport_scroll_result Terminal_viewport_controller::scroll_pages(int page_delta)
{
    const long long requested_delta =
        static_cast<long long>(page_delta) * m_state.visible_rows;
    const int line_delta = static_cast<int>(std::clamp(
        requested_delta,
        static_cast<long long>(std::numeric_limits<int>::min()),
        static_cast<long long>(std::numeric_limits<int>::max())));

    return scroll_lines(line_delta);
}

void Terminal_viewport_controller::return_to_tail()
{
    m_state.offset_from_tail = 0;
    m_state.follow_tail      = true;
}

void Terminal_viewport_controller::notify_user_input()
{
    return_to_tail();
}

void Terminal_viewport_controller::enter_alternate_screen()
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return;
    }

    m_primary_state          = m_state;
    m_state.active_buffer    = Terminal_buffer_id::ALTERNATE;
    m_state.scrollback_rows  = 0;
    m_state.offset_from_tail = 0;
    m_state.follow_tail      = true;
    m_state.presentation     = {};
}

void Terminal_viewport_controller::leave_alternate_screen()
{
    if (m_state.active_buffer == Terminal_buffer_id::PRIMARY) {
        clamp_state_offset(m_state);
        return;
    }

    const int visible_rows = m_state.visible_rows;
    const Terminal_alternate_screen_scroll_policy alternate_policy =
        m_state.alternate_screen_scroll_policy;

    m_state                                = m_primary_state;
    m_state.active_buffer                  = Terminal_buffer_id::PRIMARY;
    m_state.visible_rows                   = visible_rows;
    m_state.alternate_screen_scroll_policy = alternate_policy;
    clamp_state_offset(m_state);
}

void Terminal_viewport_controller::set_alternate_screen_scroll_policy(
    Terminal_alternate_screen_scroll_policy policy)
{
    m_state.alternate_screen_scroll_policy = policy;
    m_primary_state.alternate_screen_scroll_policy = policy;
}

int Terminal_viewport_controller::max_offset_from_tail() const
{
    return state_max_offset_from_tail(m_state);
}

int Terminal_viewport_controller::first_visible_logical_row() const
{
    return presentation_requested_logical_row(m_state, 0);
}

Terminal_viewport_coordinate_result
Terminal_viewport_controller::viewport_row_to_logical_row(int viewport_row) const
{
    if (viewport_row < 0 || viewport_row >= m_state.visible_rows) {
        return {Terminal_viewport_coordinate_result_code::INVALID_VIEWPORT_ROW, 0};
    }

    const int logical_row = presentation_requested_logical_row(m_state, viewport_row);
    if (logical_row < 0) {
        return {Terminal_viewport_coordinate_result_code::INVALID_VIEWPORT_ROW, 0};
    }
    return {
        Terminal_viewport_coordinate_result_code::OK,
        logical_row,
    };
}

}
