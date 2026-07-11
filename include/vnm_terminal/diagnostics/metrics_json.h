#pragma once

#include <QJsonObject>

class VNM_TerminalSurface;

namespace vnm_terminal::diagnostics {

// Fill `out` with legacy renderer compatibility metadata and frame counters for
// `surface`. New renderer diagnostics should use append_atlas_metrics_json and
// the public frame counters on VNM_TerminalSurface; this helper remains for
// source compatibility with hosts that still frame a "renderer" object.
void append_renderer_metrics_json(const VNM_TerminalSurface& surface, QJsonObject& out);

// Fill `out` with the QSG atlas frame-report metrics for `surface`. The caller
// owns the surrounding object: production places this content under the
// "qsg_atlas" key of the runtime metrics document.
void append_atlas_metrics_json(const VNM_TerminalSurface& surface, QJsonObject& out);

// Fill `out` with GUI-thread render invalidation counters for `surface`. The
// caller owns the surrounding object: production places this content under the
// "render_invalidation" key of the runtime metrics document.
void append_render_invalidation_metrics_json(
    const VNM_TerminalSurface&  surface,
    QJsonObject&                out);

// Fill `out` with backend callback drain/pump counters for `surface`. The
// caller owns the surrounding object: production places this content under the
// "backend_drain" key of the runtime metrics document.
void append_backend_drain_metrics_json(
    const VNM_TerminalSurface&  surface,
    QJsonObject&                out);

// Fill `out` with retained-history storage measurements, live compaction
// counters, and the codec-owned prefix-plain-ASCII retention estimate.
void append_retained_history_metrics_json(
    const VNM_TerminalSurface& surface,
    QJsonObject&               out);

}
