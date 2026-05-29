#pragma once

#include "vnm_terminal/internal/terminal_screen_model.h"

namespace vnm_terminal::internal {

struct terminal_backing_delta_viewport_sync_t
{
    int                              scrollback_rows = 0;
    int                              evicted_scrollback_rows = 0;
    bool                             used_primary_history_delta = false;
    bool                             used_mode_transition_delta = false;
    bool                             active_buffer_changed = false;
    std::optional<Terminal_buffer_id> active_buffer_after;
};

terminal_backing_delta_viewport_sync_t viewport_sync_from_backing_deltas(
    const Terminal_screen_model_result& result);

}
