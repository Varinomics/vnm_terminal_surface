#pragma once

#include "vnm_terminal/internal/render_snapshot.h"

#include <memory>

namespace vnm_terminal::internal {

// The producer: presents a native frame through its mapping record. The
// requested positions come from the frame's viewport presentation (see
// presentation_requested_logical_row); each requested native row resolves
// against the same frame's coverage, its own rows and its leading rows.
//
// Returns the source itself when the presentation is the identity. Returns
// null when a requested row is unavailable and cannot become an unused slot:
// a live frame must supply every requested row, and a frozen frame only
// tolerates missing rows above its coverage. A null result is never
// published, and it never falls back to the native presentation.
std::shared_ptr<const Terminal_render_snapshot> make_presented_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> source);

}
