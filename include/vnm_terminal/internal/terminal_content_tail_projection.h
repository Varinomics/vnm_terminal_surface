#pragma once

#include "vnm_terminal/internal/render_snapshot.h"

#include <memory>

namespace vnm_terminal::internal {

// The producer: presents a native frame through its mapping record. The
// requested positions come from the frame's viewport presentation (see
// presentation_requested_logical_row); each requested native row resolves
// against the same frame's native rows and additional row coverage.
//
// Returns null when a requested native row is unavailable, for both live and
// frozen frames. Only the shared endpoint/page mapping creates unused slots.
// Otherwise returns the source itself when the presentation is the identity. display_rows is this
// consumer's capacity; zero uses the source's row count. A negative offset
// uses the native source offset; an explicit one is in the consumer's domain.
// The returned grid is
// display-sized and display_source retains the native geometry and coverage.
// A null result is never published and never falls back to native presentation.
std::shared_ptr<const Terminal_render_snapshot> make_presented_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> source,
    int display_rows = 0,
    int display_offset_from_tail = -1);

// Whether make_presented_snapshot can present the frame, checked without
// composing it: a frame that fails is not published.
bool render_snapshot_presentable(
    const Terminal_render_snapshot& source,
    int display_rows = 0,
    int display_offset_from_tail = -1);

}
