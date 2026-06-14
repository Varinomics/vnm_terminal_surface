#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/profile_text.h"
#include "helpers/test_check.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_render_cell_text.h"
#include "vnm_terminal/internal/terminal_session.h"
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

bool profile_text_contains_counter(const QString& text, const char* key)
{
    return text.contains(
        QStringLiteral("  ") + QString::fromLatin1(key) + QStringLiteral("="));
}

QString profile_text_section(
    const QString& text,
    const QString& begin_marker,
    const QString& end_marker)
{
    const qsizetype begin = text.indexOf(begin_marker);
    if (begin < 0) {
        return {};
    }

    const qsizetype content_begin = begin + begin_marker.size();
    const qsizetype end = end_marker.isEmpty()
        ? text.size()
        : text.indexOf(end_marker, content_begin);
    if (end < 0) {
        return text.mid(content_begin);
    }

    return text.mid(content_begin, end - content_begin);
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
    ok &= check(text.contains(QStringLiteral("  consumer_materialization_counters_available=true")),
        "session_profile_stats reports available consumer materialization counters");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_schema_semantics="
            "batch_6_materialization_boundaries")),
        "session_profile_stats reports consumer materialization counter owner semantics");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_owner_batch=Batch 6")),
        "session_profile_stats reports consumer materialization counter owner batch");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_calls=")),
        "session_profile_stats reports geometry-derived materialization calls");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_rows=")),
        "session_profile_stats reports geometry-derived materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_cells=")),
        "session_profile_stats reports geometry-derived materialization cells");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_row_view_parity_test_calls=")),
        "session_profile_stats reports row-view parity materialization calls");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_row_view_parity_test_rows=")),
        "session_profile_stats reports row-view parity materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_row_view_parity_test_cells=")),
        "session_profile_stats reports row-view parity materialization cells");
    ok &= check(text.contains(QStringLiteral(
            "  lazy_snapshot_fallback_reason_counters_available=true")),
        "session_profile_stats reports available lazy snapshot reason counters");
    ok &= check(text.contains(QStringLiteral(
            "  lazy_snapshot_fallback_reason_counters_schema_semantics="
            "batch_5_lazy_eligibility")),
        "session_profile_stats reports lazy snapshot reason counter owner semantics");
    ok &= check(text.contains(QStringLiteral(
            "  lazy_snapshot_fallback_reason_counters_owner_batch=Batch 5")),
        "session_profile_stats reports lazy snapshot reason counter owner batch");
    const char* const lazy_profile_counter_keys[] = {
        "lazy_snapshot_eligibility_checks",
        "lazy_snapshot_eligible_checks",
        "lazy_snapshot_full_fallbacks",
        "lazy_snapshot_dirty_rows_visible",
        "lazy_snapshot_previous_snapshot_borrow_candidate_rows",
        "lazy_snapshot_previous_snapshot_borrowed_rows",
        "lazy_snapshot_producer_owned_rows",
        "lazy_snapshot_producer_materialized_rows",
        "lazy_snapshot_producer_cells_scanned",
        "lazy_snapshot_producer_cells_emitted",
        "lazy_snapshot_materialization_mismatches_for_testing",
    };
    for (const char* key : lazy_profile_counter_keys) {
        ok &= check(profile_text_contains_counter(text, key), key);
    }
    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        ok &= check(
            profile_text_contains_counter(text, descriptor.profile_key),
            descriptor.profile_key);
    }
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
    const QString last_renderer_stats = profile_text_section(
        text,
        QStringLiteral("last_renderer_stats\n"),
        QStringLiteral("cumulative_renderer_stats\n"));
    ok &= check(last_renderer_stats.contains(QStringLiteral("  frame_visible_rows=")),
        "last_renderer_stats reports frame_visible_rows");
    ok &= check(
        last_renderer_stats.contains(QStringLiteral("  frame_dirty_row_lookup_count=")),
        "last_renderer_stats reports frame_dirty_row_lookup_count");
    ok &= check(
        profile_text_contains_counter(last_renderer_stats, "frame_row_descriptors_built"),
        "last_renderer_stats reports frame row descriptor builds");
    ok &= check(
        profile_text_contains_counter(last_renderer_stats, "frame_layer_descriptors_built"),
        "last_renderer_stats reports frame layer descriptor builds");
    ok &= check(last_renderer_stats.contains(QStringLiteral("  qsg_layer_descriptors=0\n")),
        "last_renderer_stats reports zero QSG layer descriptors");
    ok &= check(text.contains(QStringLiteral("cumulative_renderer_stats\n")),
        "append_cumulative_renderer_stats_text emits the cumulative_renderer_stats header");
    const QString cumulative_renderer_stats = profile_text_section(
        text,
        QStringLiteral("cumulative_renderer_stats\n"),
        QStringLiteral("qsg_atlas\n"));
    ok &= check(cumulative_renderer_stats.contains(QStringLiteral("  qsg_layer_descriptors=0\n")),
        "cumulative_renderer_stats reports zero QSG layer descriptors");
    ok &= check(text.contains(QStringLiteral("qsg_atlas\n")),
        "append_qsg_atlas_profile_text emits the qsg_atlas header");
    const QString qsg_atlas = profile_text_section(
        text,
        QStringLiteral("qsg_atlas\n"),
        QStringLiteral("slow_text_layouts threshold_ns="));
    ok &= check(qsg_atlas.contains(QStringLiteral("  renderer=atlas\n")),
        "qsg_atlas section tags the renderer as atlas");
    ok &= check(profile_text_contains_counter(qsg_atlas, "frame_row_descriptors"),
        "qsg_atlas section reports frame row descriptors");
    ok &= check(profile_text_contains_counter(qsg_atlas, "frame_layer_descriptors"),
        "qsg_atlas section reports frame layer descriptors");
    ok &= check(qsg_atlas.contains(QStringLiteral("  qsg_layer_descriptors=0\n")),
        "qsg_atlas section reports zero QSG layer descriptors");
    ok &= check(qsg_atlas.contains(QStringLiteral("  rect_row_capacity=")),
        "qsg_atlas section reports rect row-stable capacity");
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
