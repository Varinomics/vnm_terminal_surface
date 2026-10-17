#pragma once

#include "vnm_terminal/internal/render_snapshot.h"

#include <memory>

namespace vnm_terminal::internal {

// Relocate the unwritten native suffix before the resize-displaced prefix.
// The resulting stream retains its native length and viewport offsets.
int content_tail_display_row_to_logical_row(const Terminal_render_snapshot& native_snapshot, int display_row);
int content_tail_logical_row_to_display_row(const Terminal_render_snapshot& native_snapshot, int logical_row);
std::shared_ptr<const Terminal_render_snapshot> make_content_tail_display_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> native_snapshot);

}
