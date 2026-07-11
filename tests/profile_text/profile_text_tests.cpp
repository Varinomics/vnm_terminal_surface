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

qsizetype profile_text_line_marker_index(
    const QString& text,
    const QString& marker,
    qsizetype      from)
{
    qsizetype cursor = from;
    while (cursor < text.size()) {
        const qsizetype marker_index = text.indexOf(marker, cursor);
        if (marker_index < 0) {
            return -1;
        }
        if (marker_index == 0 ||
            text.at(marker_index - 1) == QLatin1Char('\n')) {
            return marker_index;
        }
        cursor = marker_index + 1;
    }
    return -1;
}

QString profile_text_section(
    const QString& text,
    const QString& begin_marker,
    const QString& end_marker)
{
    const qsizetype begin = profile_text_line_marker_index(text, begin_marker, 0);
    if (begin < 0) {
        return {};
    }

    const qsizetype content_begin = begin + begin_marker.size();
    const qsizetype end = end_marker.isEmpty()
        ? text.size()
        : profile_text_line_marker_index(text, end_marker, content_begin);
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
    diag::append_retained_history_profile_text(surface, stream);
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
    const QString retained_history = profile_text_section(
        text,
        QStringLiteral("retained_history\n"),
        QStringLiteral("session_profile_stats\n"));
    ok &= check(
        retained_history.contains(QStringLiteral("  byte_budget=0\n")) &&
        retained_history.contains(QStringLiteral("  average_retained_row_bytes=0\n")) &&
        retained_history.contains(QStringLiteral("  prefix_plain_ascii_estimate\n")) &&
        retained_history.contains(QStringLiteral("    contract_version=0\n")) &&
        retained_history.contains(QStringLiteral("    source_width_columns=0\n")) &&
        retained_history.contains(QStringLiteral("    max_columns_at_target_rows=0\n")),
        "retained-history profile text preserves the nested descriptor shape");
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
            "geometry_derived_snapshot_materialization_counters")),
        "session_profile_stats reports consumer materialization counter schema semantics");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_owner_semantics="
            "terminal_session_profile_stats")),
        "session_profile_stats reports consumer materialization counter owner semantics");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_calls=")),
        "session_profile_stats reports geometry-derived materialization calls");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_rows=")),
        "session_profile_stats reports geometry-derived materialization rows");
    ok &= check(text.contains(QStringLiteral(
            "  consumer_materialization_counters_geometry_derived_snapshot_cells=")),
        "session_profile_stats reports geometry-derived materialization cells");
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
        "append_renderer_stats_text emits the legacy renderer header");
    const QString last_renderer_stats = profile_text_section(
        text,
        QStringLiteral("last_renderer_stats\n"),
        QStringLiteral("cumulative_renderer_stats\n"));
    ok &= check(
        last_renderer_stats.contains(
            QStringLiteral("  compatibility_scope=legacy_renderer_frame_counters\n")),
        "last_renderer_stats reports legacy compatibility scope");
    ok &= check(
        last_renderer_stats.contains(
            QStringLiteral("  canonical_renderer_section=qsg_atlas\n")),
        "last_renderer_stats points to qsg_atlas as canonical");
    ok &= check(
        last_renderer_stats.contains(QStringLiteral("  paint_completed=")),
        "last_renderer_stats reports paint completion compatibility state");
    ok &= check(
        !profile_text_contains_counter(last_renderer_stats, "frame_row_descriptors_built"),
        "last_renderer_stats does not expose legacy frame descriptor details");
    ok &= check(text.contains(QStringLiteral("cumulative_renderer_stats\n")),
        "append_cumulative_renderer_stats_text emits the legacy cumulative renderer header");
    const QString cumulative_renderer_stats = profile_text_section(
        text,
        QStringLiteral("cumulative_renderer_stats\n"),
        QStringLiteral("qsg_atlas\n"));
    ok &= check(
        cumulative_renderer_stats.contains(
            QStringLiteral("  compatibility_scope=legacy_renderer_frame_counters\n")),
        "cumulative_renderer_stats reports legacy compatibility scope");
    ok &= check(
        cumulative_renderer_stats.contains(
            QStringLiteral("  canonical_renderer_section=qsg_atlas\n")),
        "cumulative_renderer_stats points to qsg_atlas as canonical");
    ok &= check(profile_text_contains_counter(cumulative_renderer_stats, "frames_published"),
        "cumulative_renderer_stats reports frame publication count");
    ok &= check(
        profile_text_contains_counter(cumulative_renderer_stats, "paint_completed_frames"),
        "cumulative_renderer_stats reports paint completion count");
    ok &= check(
        profile_text_contains_counter(cumulative_renderer_stats, "qsg_atlas_render_count"),
        "cumulative_renderer_stats reports atlas render count");
    ok &= check(
        !profile_text_contains_counter(cumulative_renderer_stats, "qsg_layer_descriptors"),
        "cumulative_renderer_stats does not expose legacy QSG layer descriptor details");
    ok &= check(text.contains(QStringLiteral("qsg_atlas\n")),
        "append_qsg_atlas_profile_text emits the qsg_atlas header");
    const QString qsg_atlas = profile_text_section(
        text,
        QStringLiteral("qsg_atlas\n"),
        QStringLiteral("slow_text_layouts threshold_ns="));
    ok &= check(qsg_atlas.contains(QStringLiteral("  renderer=atlas\n")),
        "qsg_atlas section tags the renderer as atlas");
    ok &= check(profile_text_contains_counter(qsg_atlas, "prepare_elapsed_ns"),
        "qsg_atlas section reports prepare elapsed counter");
    ok &= check(profile_text_contains_counter(qsg_atlas, "render_elapsed_ns"),
        "qsg_atlas section reports render elapsed counter");
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
