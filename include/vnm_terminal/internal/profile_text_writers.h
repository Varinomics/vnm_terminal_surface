#pragma once

#if VNM_TERMINAL_PROFILING_ENABLED

#include "vnm_terminal/internal/hierarchical_profiler.h"

#include <QString>
#include <QTextStream>

namespace vnm_terminal::diagnostics {

// Profiler-snapshot writers shared with the first-party application, which owns its own
// GUI-thread Hierarchical_profiler and has to serialize it in the same TEXT format the
// surface uses for the render thread. They take internal snapshot types, so they are not
// part of the installed diagnostics API; see docs/public_surface.md.
//
// Each call appends whole lines and no leading or trailing blank line. The caller frames
// the section and emits the separators between sections.

void append_profile_node_text(
    QTextStream&                            stream,
    const internal::Profile_node_snapshot&  node,
    int                                     depth);

void append_profile_timeline_text(
    QTextStream&                                stream,
    const QString&                              label,
    const internal::Profile_timeline_snapshot&  timeline);

}

#endif
