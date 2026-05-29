#include "vnm_terminal/internal/terminal_backing_delta_viewport_sync.h"

namespace vnm_terminal::internal {

namespace {

bool primary_history_delta_affects_viewport(
    const terminal_backing_delta_t& delta)
{
    if (delta.buffer_id != Terminal_buffer_id::PRIMARY) {
        return false;
    }

    switch (delta.kind) {
        case Terminal_backing_delta_kind::BACKING_UNCHANGED:
        case Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED:
        case Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED:
        case Terminal_backing_delta_kind::PRIMARY_HISTORY_CLEARED:
        case Terminal_backing_delta_kind::PRIMARY_HISTORY_DISCARDED:
            return true;
        case Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED:
        case Terminal_backing_delta_kind::COLUMN_REFLOWED:
        case Terminal_backing_delta_kind::MODE_TRANSITIONED:
            return false;
    }

    return false;
}

}

terminal_backing_delta_viewport_sync_t viewport_sync_from_backing_deltas(
    const Terminal_screen_model_result& result)
{
    terminal_backing_delta_viewport_sync_t sync;
    sync.scrollback_rows           = result.scrollback_rows;
    sync.evicted_scrollback_rows   = result.evicted_scrollback_rows;
    sync.active_buffer_changed     = result.active_buffer_changed;

    int backing_delta_evicted_rows = 0;
    for (const terminal_backing_delta_t& delta : result.backing_deltas) {
        if (primary_history_delta_affects_viewport(delta)) {
            sync.used_primary_history_delta = true;
            sync.scrollback_rows            = delta.scrollback_rows_after;
            backing_delta_evicted_rows     += delta.evicted_scrollback_rows;
            backing_delta_evicted_rows     += delta.discarded_scrollback_rows;
        }

        if (delta.kind == Terminal_backing_delta_kind::MODE_TRANSITIONED) {
            sync.used_mode_transition_delta = true;
            sync.active_buffer_after = delta.active_buffer_after;
        }
    }

    if (sync.used_primary_history_delta) {
        sync.evicted_scrollback_rows = backing_delta_evicted_rows;
    }

    return sync;
}

}
