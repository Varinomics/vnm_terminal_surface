#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_backing_delta_viewport_sync.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include "helpers/test_check.h"

#include <QString>

#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::terminal_backing_delta_t primary_history_delta(
    term::Terminal_backing_delta_kind kind,
    int                               rows_before,
    int                               rows_after,
    int                               appended_rows,
    int                               evicted_rows,
    int                               discarded_rows)
{
    term::terminal_backing_delta_t delta;
    delta.kind                       = kind;
    delta.buffer_id                  = term::Terminal_buffer_id::PRIMARY;
    delta.active_buffer_before       = term::Terminal_buffer_id::PRIMARY;
    delta.active_buffer_after        = term::Terminal_buffer_id::PRIMARY;
    delta.scrollback_rows_before     = rows_before;
    delta.scrollback_rows_after      = rows_after;
    delta.appended_scrollback_rows   = appended_rows;
    delta.evicted_scrollback_rows    = evicted_rows;
    delta.discarded_scrollback_rows  = discarded_rows;
    return delta;
}

term::terminal_backing_delta_t mode_transition_delta(
    term::Terminal_buffer_id active_buffer_before,
    term::Terminal_buffer_id active_buffer_after)
{
    term::terminal_backing_delta_t delta;
    delta.kind                 = term::Terminal_backing_delta_kind::MODE_TRANSITIONED;
    delta.buffer_id            = active_buffer_after;
    delta.active_buffer_before = active_buffer_before;
    delta.active_buffer_after  = active_buffer_after;
    return delta;
}

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

    viewport.sync_scrollback_rows(8, 2);
    ok &= check(viewport.state().offset_from_tail == 7,
        "detached viewport tracks bounded scrollback eviction");
    ok &= check(viewport.first_visible_logical_row() == 1,
        "scrollback eviction rebases preserved first visible row");

    viewport.notify_user_input();
    ok &= check(viewport.state().follow_tail, "user input returns viewport to tail");
    ok &= check(viewport.state().offset_from_tail == 0,
        "tail restoration clears viewport offset");

    return ok;
}

bool test_backing_delta_viewport_sync_prefers_primary_history_deltas()
{
    bool ok = true;

    term::Terminal_screen_model_result append_then_evict;
    append_then_evict.scrollback_rows         = 99;
    append_then_evict.evicted_scrollback_rows = 77;
    append_then_evict.backing_deltas = {
        primary_history_delta(
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
            2,
            3,
            1,
            0,
            0),
        primary_history_delta(
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED,
            3,
            2,
            0,
            1,
            0),
    };

    const term::terminal_backing_delta_viewport_sync_t append_then_evict_sync =
        term::viewport_sync_from_backing_deltas(append_then_evict);
    ok &= check(append_then_evict_sync.used_primary_history_delta,
        "viewport sync uses primary-history backing deltas");
    ok &= check(append_then_evict_sync.scrollback_rows == 2,
        "viewport sync takes row count from final primary-history delta");
    ok &= check(append_then_evict_sync.evicted_scrollback_rows == 1,
        "viewport sync derives evictions from primary-history deltas");

    term::Terminal_screen_model_result clear_result;
    clear_result.scrollback_rows         = 99;
    clear_result.evicted_scrollback_rows = 77;
    clear_result.backing_deltas = {
        primary_history_delta(
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_CLEARED,
            5,
            0,
            0,
            5,
            0),
    };

    const term::terminal_backing_delta_viewport_sync_t clear_sync =
        term::viewport_sync_from_backing_deltas(clear_result);
    ok &= check(clear_sync.scrollback_rows == 0,
        "viewport sync clamps to clear delta row count");
    ok &= check(clear_sync.evicted_scrollback_rows == 5,
        "viewport sync carries clear delta row-origin movement");

    term::Terminal_screen_model_result discard_result;
    discard_result.scrollback_rows         = 99;
    discard_result.evicted_scrollback_rows = 77;
    discard_result.backing_deltas = {
        primary_history_delta(
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_DISCARDED,
            0,
            0,
            0,
            0,
            1),
    };

    const term::terminal_backing_delta_viewport_sync_t discard_sync =
        term::viewport_sync_from_backing_deltas(discard_result);
    ok &= check(discard_sync.scrollback_rows == 0,
        "viewport sync keeps zero-limit discard at the current row count");
    ok &= check(discard_sync.evicted_scrollback_rows == 1,
        "viewport sync treats discarded rows as row-origin movement");

    return ok;
}

bool test_backing_delta_viewport_sync_accepts_recovered_primary_history_deltas()
{
    bool ok = true;

    term::Terminal_screen_model_result recovery_result;
    recovery_result.scrollback_rows         = 9;
    recovery_result.evicted_scrollback_rows = 4;
    recovery_result.backing_deltas = {
        primary_history_delta(
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
            2,
            3,
            1,
            0,
            0),
    };

    const term::terminal_backing_delta_viewport_sync_t recovery_sync =
        term::viewport_sync_from_backing_deltas(recovery_result);
    ok &= check(recovery_sync.used_primary_history_delta,
        "viewport sync accepts recovered primary-history append deltas");
    ok &= check(recovery_sync.scrollback_rows == 3,
        "viewport sync takes recovered row count from the accepted delta");
    ok &= check(recovery_sync.evicted_scrollback_rows == 0,
        "viewport sync does not carry stale scalar evictions after accepted recovery");

    return ok;
}

bool test_backing_delta_viewport_sync_keeps_scalar_fallback_narrow()
{
    bool ok = true;

    term::Terminal_screen_model_result no_delta_result;
    no_delta_result.scrollback_rows         = 8;
    no_delta_result.evicted_scrollback_rows = 2;

    const term::terminal_backing_delta_viewport_sync_t no_delta_sync =
        term::viewport_sync_from_backing_deltas(no_delta_result);
    ok &= check(!no_delta_sync.used_primary_history_delta,
        "viewport sync records no primary-history delta for fallback");
    ok &= check(no_delta_sync.scrollback_rows == 8,
        "viewport sync fallback preserves scalar row count");
    ok &= check(no_delta_sync.evicted_scrollback_rows == 2,
        "viewport sync fallback preserves scalar eviction count");

    term::Terminal_screen_model_result active_grid_result;
    active_grid_result.scrollback_rows = 6;
    term::terminal_backing_delta_t resize_delta;
    resize_delta.kind = term::Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED;
    active_grid_result.backing_deltas.push_back(resize_delta);

    const term::terminal_backing_delta_viewport_sync_t active_grid_sync =
        term::viewport_sync_from_backing_deltas(active_grid_result);
    ok &= check(!active_grid_sync.used_primary_history_delta,
        "active-grid deltas do not masquerade as primary-history deltas");
    ok &= check(active_grid_sync.scrollback_rows == 6,
        "active-grid deltas leave scrollback count on compatibility scalar");

    return ok;
}

bool test_backing_delta_viewport_sync_uses_mode_transition_deltas()
{
    bool ok = true;

    term::Terminal_screen_model_result result;
    result.active_buffer_changed = false;
    result.backing_deltas = {
        mode_transition_delta(
            term::Terminal_buffer_id::PRIMARY,
            term::Terminal_buffer_id::ALTERNATE),
    };

    const term::terminal_backing_delta_viewport_sync_t sync =
        term::viewport_sync_from_backing_deltas(result);
    ok &= check(sync.used_mode_transition_delta,
        "viewport sync records mode-transition delta use");
    ok &= check(!sync.active_buffer_changed,
        "viewport sync keeps published active-buffer change gating separate");
    ok &= check(sync.active_buffer_after.has_value() &&
        *sync.active_buffer_after == term::Terminal_buffer_id::ALTERNATE,
        "viewport sync exposes delta active-buffer destination");

    result.active_buffer_changed = true;
    const term::terminal_backing_delta_viewport_sync_t published_sync =
        term::viewport_sync_from_backing_deltas(result);
    ok &= check(published_sync.active_buffer_changed,
        "viewport sync preserves published active-buffer change scalar");

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

    viewport.sync_scrollback_rows(20, 5);
    viewport.leave_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::PRIMARY,
        "leaving alternate returns to primary");
    ok &= check(viewport.state().scrollback_rows == 20,
        "leaving alternate restores primary scrollback rows");
    ok &= check(viewport.state().offset_from_tail == 11,
        "leaving alternate restores primary viewport after eviction");
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

}

int main()
{
    bool ok = true;
    ok &= test_follow_tail_and_detached_output();
    ok &= test_backing_delta_viewport_sync_prefers_primary_history_deltas();
    ok &= test_backing_delta_viewport_sync_accepts_recovered_primary_history_deltas();
    ok &= test_backing_delta_viewport_sync_keeps_scalar_fallback_narrow();
    ok &= test_backing_delta_viewport_sync_uses_mode_transition_deltas();
    ok &= test_page_scroll_and_bounds();
    ok &= test_alternate_screen_policy_and_restore();
    ok &= test_coordinate_mapping_and_selection_rows();
    return ok ? 0 : 1;
}
