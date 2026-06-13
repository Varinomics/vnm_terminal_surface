#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/profile_text.h"
#include "helpers/test_check.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_render_cell_text.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QColor>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QString>
#include <QTextStream>
#include <QThread>

#include <cstdint>
#include <memory>
#include <utility>

namespace term = vnm_terminal::internal;

namespace diag = vnm_terminal::diagnostics;

namespace {

using vnm_terminal::test_helpers::check;

void pump_events(QGuiApplication& app, int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

std::shared_ptr<const term::Terminal_render_snapshot> make_smoke_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 12}, viewport, sequence);
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 2}};
    const QString marker_text = QStringLiteral("atlas");
    const int marker_width    = static_cast<int>(marker_text.size());
    snapshot.cells.push_back({
        {0, 0},
        term::Terminal_render_cell_text::from_source_cell(marker_text, marker_width, false),
        0U,
        marker_width,
        false,
        term::k_default_terminal_style_id,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
    });

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

bool test_profile_text_sections(QGuiApplication& app)
{
    bool ok = true;

    QQuickWindow window;
    window.setColor(QColor(32, 32, 32));
    window.resize(360, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 180.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(14.0);
    surface.set_dirty_row_stats_enabled(true);

    term::VNM_TerminalSurface_render_bridge::set_render_profiler(
        surface,
        std::make_shared<term::Hierarchical_profiler>());
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_smoke_snapshot(1U));

    window.show();
    pump_events(app);

    // Drive each public section builder into one document so the smoke also
    // exercises the interleaved order the host emits. The host owns the inter-
    // section separators; here the assertions only need the per-section header
    // token each builder appends, so a single newline between calls is enough.
    QString     text;
    QTextStream stream(&text);

    diag::append_surface_geometry_profile_text(surface, stream);
    stream << '\n';
    diag::append_dirty_row_stats_text(surface, stream);
    stream << '\n';
    diag::append_dirty_row_timeline_text(surface, stream);
    stream << '\n';
    diag::append_model_profile_stats_text(surface, stream);
    stream << '\n';
    diag::append_session_profile_stats_text(surface, stream);
    stream << '\n';
    diag::append_renderer_stats_text(surface, stream);
    stream << '\n';
    diag::append_cumulative_renderer_stats_text(surface, stream);
    stream << '\n';
    diag::append_qsg_atlas_profile_text(surface, stream);
    stream << '\n';
    diag::append_slow_text_layout_diagnostics_text(surface, stream);
    stream << '\n';
    diag::append_render_thread_profile_text(surface, stream);
    stream.flush();

    ok &= check(text.contains(QStringLiteral("surface_geometry\n")),
        "append_surface_geometry_profile_text emits the surface_geometry header");
    ok &= check(text.contains(QStringLiteral("  cell_width=")),
        "surface_geometry reports fresh-provider cell_width");
    ok &= check(text.contains(QStringLiteral("  cell_height=")),
        "surface_geometry reports fresh-provider cell_height");
    ok &= check(text.contains(QStringLiteral("dirty_rows\n")),
        "append_dirty_row_stats_text emits the dirty_rows header");
    ok &= check(text.contains(QStringLiteral("dirty_row_timeline bucket_width_ms=")),
        "append_dirty_row_timeline_text emits the dirty_row_timeline header");
    ok &= check(text.contains(QStringLiteral("model_profile_stats\n")),
        "append_model_profile_stats_text emits the model_profile_stats header");
    ok &= check(text.contains(QStringLiteral("  render_snapshot_cells_scanned=")),
        "model_profile_stats reports render_snapshot_cells_scanned");
    ok &= check(text.contains(QStringLiteral("  render_snapshot_rows_built_from_model_storage=")),
        "model_profile_stats reports render_snapshot_rows_built_from_model_storage");
    ok &= check(text.contains(QStringLiteral("  render_snapshot_model_row_accessor_borrows=")),
        "model_profile_stats reports render_snapshot_model_row_accessor_borrows");
    ok &= check(text.contains(QStringLiteral("  render_snapshot_inline_single_bmp_text_cells=")),
        "model_profile_stats reports render_snapshot_inline_single_bmp_text_cells");
    ok &= check(text.contains(QStringLiteral("session_profile_stats\n")),
        "append_session_profile_stats_text emits the session_profile_stats header");
    ok &= check(text.contains(QStringLiteral("  full_snapshot_publications=")),
        "session_profile_stats reports full_snapshot_publications");
    ok &= check(text.contains(QStringLiteral("  snapshots_consumed_by_bridge=")),
        "session_profile_stats reports snapshots_consumed_by_bridge");
    ok &= check(text.contains(QStringLiteral("  consumer_materialization_counters_available=false")),
        "session_profile_stats reports unavailable consumer materialization counters");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_schema_semantics="
            "unavailable_until_batch_3_materialization_boundaries")),
        "session_profile_stats reports consumer materialization counter owner semantics");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_owner_batch=Batch 3")),
        "session_profile_stats reports consumer materialization counter owner batch");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_frame_builder_rows=unavailable")),
        "session_profile_stats reports unavailable frame-builder materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_public_projection_rows=unavailable")),
        "session_profile_stats reports unavailable public-projection materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_transcript_rows=unavailable")),
        "session_profile_stats reports unavailable transcript materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_selection_rows=unavailable")),
        "session_profile_stats reports unavailable selection materialization rows");
    ok &= check(text.contains(QStringLiteral("  retained_snapshot_payload_bytes=")),
        "session_profile_stats reports retained_snapshot_payload_bytes");
    ok &= check(text.contains(QStringLiteral("  retained_snapshot_generation_count=")),
        "session_profile_stats reports retained_snapshot_generation_count");
    ok &= check(text.contains(QStringLiteral("  max_retained_snapshot_payload_bytes=")),
        "session_profile_stats reports max_retained_snapshot_payload_bytes");
    ok &= check(text.contains(QStringLiteral("  max_retained_snapshot_generation_count=")),
        "session_profile_stats reports max_retained_snapshot_generation_count");
    ok &= check(text.contains(QStringLiteral("  public_projection_scroll_publications=")),
        "session_profile_stats reports public_projection_scroll_publications");
    ok &= check(text.contains(QStringLiteral("last_renderer_stats\n")),
        "append_renderer_stats_text emits the last_renderer_stats header");
    ok &= check(text.contains(QStringLiteral("  frame_visible_rows=")),
        "last_renderer_stats reports frame_visible_rows");
    ok &= check(text.contains(QStringLiteral("  frame_dirty_row_lookup_count=")),
        "last_renderer_stats reports frame_dirty_row_lookup_count");
    ok &= check(text.contains(QStringLiteral("cumulative_renderer_stats\n")),
        "append_cumulative_renderer_stats_text emits the cumulative_renderer_stats header");
    ok &= check(text.contains(QStringLiteral("qsg_atlas\n")),
        "append_qsg_atlas_profile_text emits the qsg_atlas header");
    ok &= check(text.contains(QStringLiteral("  renderer=atlas\n")),
        "qsg_atlas section tags the renderer as atlas");
    ok &= check(text.contains(QStringLiteral("slow_text_layouts threshold_ns=")),
        "append_slow_text_layout_diagnostics_text emits the slow_text_layouts header");
    ok &= check(text.contains(QStringLiteral("render_thread sequence=")),
        "append_render_thread_profile_text emits the render_thread sequence header");
    ok &= check(text.contains(QStringLiteral("render_thread_timeline bucket_width_ms=")),
        "append_render_thread_profile_text emits the render_thread timeline");

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_profile_text_sections(app);
    return ok ? 0 : 1;
}

#else

int main()
{
    // Profiling is compiled out: the relocated diagnostics component is empty,
    // so there is nothing to exercise and the smoke trivially passes.
    return 0;
}

#endif
