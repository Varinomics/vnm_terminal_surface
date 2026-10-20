#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include "helpers/test_check.h"

#include <QString>

#include <iostream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool test_follow_tail_and_detached_output()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    ok &= check(viewport.set_visible_rows(3).code ==
        term::Terminal_viewport_result_code::ACCEPTED,
        "viewport accepts visible row count");

    viewport.set_scrollback_rows(5);
    ok &= check(viewport.state().offset_from_tail == 0,
        "follow-tail remains at tail after output");
    ok &= check(viewport.first_visible_logical_row() == 5,
        "tail viewport starts after scrollback rows");

    const term::Terminal_viewport_scroll_result scroll_result = viewport.scroll_lines(2);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "scrollback movement reports viewport movement");
    ok &= check(scroll_result.applied_line_delta == 2,
        "scrollback movement reports applied line delta");
    ok &= check(!viewport.state().follow_tail, "scrollback movement detaches from tail");
    ok &= check(viewport.first_visible_logical_row() == 3,
        "detached viewport maps first visible row");

    viewport.set_scrollback_rows(8);
    ok &= check(viewport.state().offset_from_tail == 5,
        "detached viewport preserves viewed logical row when output grows");
    ok &= check(viewport.first_visible_logical_row() == 3,
        "detached output growth preserves first visible row");

    viewport.set_scrollback_rows(1);
    ok &= check(viewport.state().offset_from_tail == 1,
        "detached viewport clamps when scrollback shrinks");
    ok &= check(viewport.first_visible_logical_row() == 0,
        "scrollback shrink keeps viewport inside retained bounds");

    viewport.notify_user_input();
    ok &= check(viewport.state().follow_tail, "user input returns viewport to tail");
    ok &= check(viewport.state().offset_from_tail == 0,
        "tail restoration clears viewport offset");

    return ok;
}

bool test_page_scroll_and_bounds()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(4);
    viewport.set_scrollback_rows(9);

    term::Terminal_viewport_scroll_result result = viewport.scroll_pages(1);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "page scroll moves viewport");
    ok &= check(result.applied_line_delta == 4, "page scroll uses visible row count");
    ok &= check(viewport.state().offset_from_tail == 4, "page scroll updates offset");

    result  = viewport.scroll_pages(4);
    ok     &= check(result.applied_line_delta == 5,
        "page scroll clamps to top of scrollback");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "page scroll clamps at maximum offset");

    result  = viewport.scroll_lines(1);
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "scroll at top reports boundary");
    ok     &= check(result.applied_line_delta == 0,
        "boundary scroll has no applied delta");

    result  = viewport.scroll_pages(-3);
    ok     &= check(result.applied_line_delta == -9,
        "negative page scroll returns to tail");
    ok     &= check(viewport.state().follow_tail, "negative page scroll follows tail at zero");

    viewport.set_scrollback_rows(20);
    viewport.scroll_lines(3);
    result  = viewport.scroll_lines(std::numeric_limits<int>::max());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum positive line delta from detached offset moves viewport");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "maximum positive line delta from detached offset clamps at scrollback top");

    result  = viewport.scroll_lines(std::numeric_limits<int>::min());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum negative line delta moves viewport");
    ok     &= check(viewport.state().offset_from_tail == 0,
        "maximum negative line delta clamps at tail");

    result  = viewport.scroll_pages(std::numeric_limits<int>::max());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum positive page delta moves viewport");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "maximum positive page delta clamps at scrollback top");

    return ok;
}

bool test_alternate_screen_policy_and_restore()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(5);
    viewport.set_scrollback_rows(20);
    viewport.scroll_lines(6);

    viewport.enter_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate screen becomes active");
    ok &= check(viewport.state().scrollback_rows == 0,
        "alternate screen exposes no primary scrollback");
    ok &= check(viewport.state().offset_from_tail == 0 && viewport.state().follow_tail,
        "alternate screen starts at tail");

    term::Terminal_viewport_scroll_result result = viewport.scroll_lines(3);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "alternate keep-tail policy does not move viewport");

    viewport.set_alternate_screen_scroll_policy(
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT);
    result = viewport.scroll_lines(3);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::TERMINAL_INPUT,
        "alternate wheel policy requests terminal input");

    viewport.set_scrollback_rows(20);
    viewport.leave_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::PRIMARY,
        "leaving alternate returns to primary");
    ok &= check(viewport.state().scrollback_rows == 20,
        "leaving alternate restores primary scrollback rows");
    ok &= check(viewport.state().offset_from_tail == 6,
        "leaving alternate restores primary viewport");
    ok &= check(viewport.state().alternate_screen_scroll_policy ==
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT,
        "alternate scroll policy remains recorded");

    return ok;
}

bool test_coordinate_mapping_and_selection_rows()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(3);
    viewport.set_scrollback_rows(8);
    viewport.scroll_lines(2);

    term::Terminal_viewport_coordinate_result coordinate =
        viewport.viewport_row_to_logical_row(0);
    ok &= check(coordinate.code == term::Terminal_viewport_coordinate_result_code::OK,
        "top viewport row maps to logical row");
    ok &= check(coordinate.logical_row == 6, "top viewport row logical index");

    coordinate  = viewport.viewport_row_to_logical_row(2);
    ok         &= check(coordinate.code == term::Terminal_viewport_coordinate_result_code::OK,
        "bottom viewport row maps to logical row");
    ok         &= check(coordinate.logical_row == 8, "bottom viewport row logical index");

    coordinate = viewport.viewport_row_to_logical_row(3);
    ok &= check(coordinate.code ==
        term::Terminal_viewport_coordinate_result_code::INVALID_VIEWPORT_ROW,
        "row outside viewport is rejected");

    std::vector<QString> logical_rows = {
        QStringLiteral("row0"),
        QStringLiteral("row1"),
        QStringLiteral("row2"),
        QStringLiteral("row3"),
        QStringLiteral("scrollback-a"),
        QStringLiteral("scrollback-b"),
        QStringLiteral("scrollback-c"),
        QStringLiteral("scrollback-d"),
        QStringLiteral("visible-a"),
        QStringLiteral("visible-b"),
        QStringLiteral("visible-c"),
    };

    term::Selection_contract_controller selection;
    selection.begin({viewport.viewport_row_to_logical_row(0).logical_row, 0});
    selection.extend({viewport.viewport_row_to_logical_row(2).logical_row, 9});

    const term::Terminal_selection_result selected =
        selection.selected_text(std::span<const QString>(logical_rows));
    ok &= check(selected.code == term::Terminal_selection_result_code::OK,
        "selection extracts viewport-mapped logical rows");
    ok &= check(selected.text == QStringLiteral("scrollback-c\nscrollback-d\nvisible-a"),
        "selection text spans scrollback and visible rows");

    return ok;
}

bool test_model_continuity_fields_do_not_change_viewport_policy()
{
    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(4);
    viewport.set_scrollback_rows(9);
    viewport.scroll_lines(3);
    const term::Terminal_viewport_state before = viewport.state();

    term::Terminal_screen_model_result model_result;
    model_result.selection_continuity.emplace();
    const term::terminal_history_handle_t old_handle =
        term::terminal_history_handle_from_retained_identity(41U, 2U);
    const term::terminal_history_handle_t final_handle =
        term::terminal_history_handle_from_retained_identity(73U, 2U);
    model_result.selection_continuity->
        successors_by_old_retained_line_id[old_handle.row_sequence].push_back(
            {old_handle, final_handle, 5, 6, 1});

    return check(viewport.state().active_buffer == before.active_buffer &&
            viewport.state().scrollback_rows == before.scrollback_rows &&
            viewport.state().visible_rows == before.visible_rows &&
            viewport.state().offset_from_tail == before.offset_from_tail &&
            viewport.state().follow_tail == before.follow_tail,
        "model continuity result fields alone do not change viewport policy");
}

}

// Every clamp uses the presentation's maximum (DESIGN 5.4 Bounds): AUTOMATIC
// excludes its hidden rows, and COMPANION and NATIVE keep the native S.
bool test_presentation_bounds()
{
    bool ok = true;
    const auto mapping = [](bool companion, int vacancy) {
        return term::make_presentation_mapping(
            term::Terminal_buffer_id::PRIMARY, companion, 6, 10, vacancy, 2, 0);
    };

    term::Terminal_viewport_controller viewport;
    ok &= check(viewport.set_visible_rows(6).code == term::Terminal_viewport_result_code::ACCEPTED,
        "the bounds fixture accepts six visible rows");
    viewport.set_scrollback_rows(10);
    viewport.set_presentation(mapping(false, 4));
    ok &= check(viewport.max_offset_from_tail() == 6,
        "the automatic maximum excludes the hidden vacancy");
    const auto scrolled = viewport.scroll_lines(100);
    ok &= check(scrolled.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED &&
            scrolled.applied_line_delta == 6 && viewport.state().offset_from_tail == 6,
        "scrolling stops at the automatic maximum");
    ok &= check(viewport.scroll_pages(1).action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "page scrolling reports the automatic boundary");

    viewport.set_presentation(mapping(false, 2));
    ok &= check(viewport.max_offset_from_tail() == 8,
        "consumed vacancy widens the range");
    viewport.set_presentation(mapping(false, 5));
    ok &= check(viewport.max_offset_from_tail() == 5 && viewport.state().offset_from_tail <= 5,
        "a larger vacancy narrows the range and the view stays inside it");

    viewport.set_presentation(mapping(true, 5));
    ok &= check(viewport.max_offset_from_tail() == 10,
        "the Companion range keeps the native scrollback");

    viewport.set_presentation(mapping(false, 4));
    viewport.enter_alternate_screen();
    ok &= check(viewport.max_offset_from_tail() == 0 &&
            viewport.state().presentation.policy == term::Terminal_presentation_policy::NATIVE,
        "the alternate screen is native and has no range");
    viewport.set_presentation(mapping(false, 1));
    viewport.leave_alternate_screen();
    ok &= check(viewport.state().presentation.hidden_rows == 1 && viewport.max_offset_from_tail() == 9,
        "the primary mapping set while the alternate screen was active returns with it");
    return ok;
}

std::pair<int, int> first_displayed_row(const term::Terminal_viewport_state& state)
{
    const auto anchor = term::presentation_first_displayed_row(
        state, 0, state.scrollback_rows + state.visible_rows);
    return anchor ? std::pair<int, int>{anchor->logical_row, anchor->display_row} : std::pair<int, int>{-1, -1};
}

// A detached view keeps its first displayed row at the same display position
// when the mapping changes (DESIGN 5.5). A view at the tail stays there, a
// position the new mapping cannot show clamps, and the saved primary view
// follows the same rule while the alternate screen is active.
bool test_presentation_anchor()
{
    bool ok = true;
    const auto automatic = [](int vacancy) {
        return term::make_presentation_mapping(
            term::Terminal_buffer_id::PRIMARY, false, 6, 10, vacancy, 6 - vacancy, 0);
    };

    term::Terminal_viewport_controller viewport;
    ok &= check(viewport.set_visible_rows(6).code == term::Terminal_viewport_result_code::ACCEPTED,
        "the anchor fixture accepts six visible rows");
    viewport.set_scrollback_rows(10);
    viewport.set_presentation(automatic(2));
    viewport.set_presentation(automatic(4));
    ok &= check(viewport.state().follow_tail && viewport.state().offset_from_tail == 0,
        "a view at the tail stays at the tail");

    viewport.set_presentation(automatic(2));
    (void)viewport.scroll_lines(3);
    ok &= check(first_displayed_row(viewport.state()) == std::pair<int, int>{5, 0},
        "three rows up the view starts with native row 5");
    viewport.set_presentation(automatic(4));
    ok &= check(viewport.state().offset_from_tail == 1 &&
            first_displayed_row(viewport.state()) == std::pair<int, int>{5, 0},
        "more hidden rows keep the first displayed row");
    viewport.set_presentation(automatic(1));
    ok &= check(viewport.state().offset_from_tail == 4 &&
            first_displayed_row(viewport.state()) == std::pair<int, int>{5, 0},
        "fewer hidden rows keep the first displayed row");
    (void)viewport.scroll_lines(-3);
    viewport.set_presentation(automatic(5));
    ok &= check(viewport.state().offset_from_tail == 0 && viewport.state().follow_tail,
        "a row the new mapping can only show below the tail clamps the view to the tail");

    term::Terminal_viewport_controller saved;
    ok &= check(saved.set_visible_rows(6).code == term::Terminal_viewport_result_code::ACCEPTED,
        "the saved-primary fixture accepts six visible rows");
    saved.set_scrollback_rows(10);
    saved.set_presentation(automatic(2));
    (void)saved.scroll_lines(3);
    saved.enter_alternate_screen();
    saved.set_presentation(automatic(4));
    saved.leave_alternate_screen();
    ok &= check(saved.state().offset_from_tail == 1 &&
            first_displayed_row(saved.state()) == std::pair<int, int>{5, 0},
        "the saved primary view returns with its first displayed row");

    // Companion with K = 3 of S = 3 inserts its three padding rows before
    // native row 0, so the top of its range starts with padding; AUTOMATIC
    // hides one row and cannot show native row 0 below row 0.
    term::Terminal_viewport_controller companion;
    ok &= check(companion.set_visible_rows(6).code == term::Terminal_viewport_result_code::ACCEPTED,
        "the Companion fixture accepts six visible rows");
    companion.set_scrollback_rows(3);
    companion.set_presentation(term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, true, 6, 3, 1, 3, 3));
    (void)companion.scroll_lines(3);
    ok &= check(first_displayed_row(companion.state()) == std::pair<int, int>{0, 3},
        "the Companion view's first native row follows three padding rows");
    companion.set_presentation(term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, false, 6, 3, 1, 3, 3));
    ok &= check(companion.state().offset_from_tail == 2 &&
            first_displayed_row(companion.state()) == std::pair<int, int>{0, 0},
        "a display position above the new maximum clamps to it");
    return ok;
}

int main()
{
    bool ok = true;
    ok &= test_follow_tail_and_detached_output();
    ok &= test_page_scroll_and_bounds();
    ok &= test_alternate_screen_policy_and_restore();
    ok &= test_coordinate_mapping_and_selection_rows();
    ok &= test_model_continuity_fields_do_not_change_viewport_policy();
    ok &= test_presentation_bounds();
    ok &= test_presentation_anchor();
    return ok ? 0 : 1;
}
