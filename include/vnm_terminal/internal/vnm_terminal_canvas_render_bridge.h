#pragma once

#include <memory>

class VNM_TerminalCanvas;

namespace vnm_terminal::internal {

class Hierarchical_profiler;
class Qsg_atlas_recorder;

class VNM_TerminalCanvas_render_bridge
{
public:
    // Acquire on the canvas GUI thread. The read-only recorder has its own
    // lifetime and a synchronized snapshot(), so render callbacks can observe
    // committed identities without reading the GUI's polled status fields.
    static std::shared_ptr<const Qsg_atlas_recorder> qsg_atlas_recorder(
        const VNM_TerminalCanvas& canvas);

    // Set on the canvas GUI thread. Subsequent captures retain the optional
    // profiler; recording requires VNM_TERMINAL_ENABLE_PROFILING.
    static void set_render_profiler(
        VNM_TerminalCanvas&                    canvas,
        std::shared_ptr<Hierarchical_profiler> profiler);
};

} // namespace vnm_terminal::internal
