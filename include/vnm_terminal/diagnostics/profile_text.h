#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include <QTextStream>

class VNM_TerminalSurface;

namespace vnm_terminal::diagnostics {

// Profile-text section builders relocated from the example app so a host can
// emit the profile/dirty-row TEXT report without including surface internal
// headers. Each function appends exactly the bytes of one report section to
// `out`: it owns no leading or trailing blank line. The caller frames the
// document (header lines, format/time_unit) and emits the inter-section
// separators between these calls.

void append_dirty_row_stats_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_dirty_row_timeline_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_model_profile_stats_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_session_profile_stats_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_renderer_stats_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_cumulative_renderer_stats_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out);

void append_qsg_atlas_profile_text(const VNM_TerminalSurface& surface, QTextStream& out);

void append_slow_text_layout_diagnostics_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out);

void append_surface_geometry_profile_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out);

void append_render_thread_profile_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out);

}

#endif
