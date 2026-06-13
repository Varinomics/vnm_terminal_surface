#include "vnm_terminal/diagnostics/profile_text.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "atlas_metric_descriptors.h"
#include "metric_descriptor.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QQuickWindow>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace vnm_terminal::diagnostics {

namespace term = vnm_terminal::internal;

namespace {

qint64 profile_nanoseconds(std::chrono::nanoseconds duration)
{
    return static_cast<qint64>(duration.count());
}

qint64 profile_mean_nanoseconds(
    std::chrono::nanoseconds   total_time,
    std::uint64_t              call_count)
{
    return call_count == 0U
        ? 0
        : static_cast<qint64>(
            total_time.count() / static_cast<std::int64_t>(call_count));
}

void append_profile_counter(
    QTextStream&               stream,
    const char*                name,
    std::uint64_t              value)
{
    stream << "  " << name << '=' << static_cast<qulonglong>(value) << '\n';
}

void append_profile_bool(
    QTextStream&               stream,
    const char*                name,
    bool                       value)
{
    stream << "  " << name << '=' << (value ? "true" : "false") << '\n';
}

template<typename Frame_stats>
void append_renderer_frame_stats_text(
    QTextStream&       stream,
    const Frame_stats& stats)
{
    append_profile_counter(
        stream,
        "frame_visible_rows",
        static_cast<std::uint64_t>(stats.visible_rows));
    append_profile_counter(
        stream,
        "frame_dirty_rows",
        static_cast<std::uint64_t>(stats.dirty_rows));
    append_profile_counter(
        stream,
        "frame_full_dirty_rows",
        static_cast<std::uint64_t>(stats.full_dirty_rows));
    append_profile_counter(
        stream,
        "frame_cell_pass_input_cells",
        static_cast<std::uint64_t>(stats.cell_pass_input_cells));
    append_profile_counter(
        stream,
        "frame_dirty_row_lookup_count",
        static_cast<std::uint64_t>(stats.dirty_row_lookup_count));
    append_profile_counter(
        stream,
        "frame_cells_considered",
        static_cast<std::uint64_t>(stats.cells_considered));
    append_profile_counter(
        stream,
        "frame_cells_skipped_invalid",
        static_cast<std::uint64_t>(stats.cells_skipped_invalid));
    append_profile_counter(
        stream,
        "frame_cells_skipped_wide_continuation",
        static_cast<std::uint64_t>(stats.cells_skipped_wide_continuation));
    append_profile_counter(
        stream,
        "frame_cells_rendered",
        static_cast<std::uint64_t>(stats.cells_rendered));
    append_profile_counter(
        stream,
        "frame_text_cells_empty",
        static_cast<std::uint64_t>(stats.text_cells_empty));
    append_profile_counter(
        stream,
        "frame_text_cells_rendered_as_text",
        static_cast<std::uint64_t>(stats.text_cells_rendered_as_text));
    append_profile_counter(
        stream,
        "frame_text_cells_printable_ascii",
        static_cast<std::uint64_t>(stats.text_cells_printable_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_other_ascii",
        static_cast<std::uint64_t>(stats.text_cells_other_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_non_ascii",
        static_cast<std::uint64_t>(stats.text_cells_non_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_simple_ascii",
        static_cast<std::uint64_t>(stats.text_cells_simple_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_single_width",
        static_cast<std::uint64_t>(stats.text_cells_single_width));
    append_profile_counter(
        stream,
        "frame_text_cells_multi_width",
        static_cast<std::uint64_t>(stats.text_cells_multi_width));
    append_profile_counter(
        stream,
        "frame_text_cells_with_decorations",
        static_cast<std::uint64_t>(stats.text_cells_with_decorations));
    append_profile_counter(
        stream,
        "frame_text_cells_with_hyperlink",
        static_cast<std::uint64_t>(stats.text_cells_with_hyperlink));
    append_profile_counter(
        stream,
        "frame_text_style_changes",
        static_cast<std::uint64_t>(stats.text_style_changes));
    append_profile_counter(
        stream,
        "frame_text_distinct_styles",
        static_cast<std::uint64_t>(stats.text_distinct_styles));
    append_profile_counter(
        stream,
        "frame_background_rects_emitted",
        static_cast<std::uint64_t>(stats.background_rects_emitted));
    append_profile_counter(
        stream,
        "frame_selection_rects_emitted",
        static_cast<std::uint64_t>(stats.selection_rects_emitted));
    append_profile_counter(
        stream,
        "frame_graphic_rects_emitted",
        static_cast<std::uint64_t>(stats.graphic_rects_emitted));
    append_profile_counter(
        stream,
        "frame_graphic_arcs_emitted",
        static_cast<std::uint64_t>(stats.graphic_arcs_emitted));
    append_profile_counter(
        stream,
        "frame_text_runs_emitted",
        static_cast<std::uint64_t>(stats.text_runs_emitted));
    append_profile_counter(
        stream,
        "frame_cursor_text_runs_emitted",
        static_cast<std::uint64_t>(stats.cursor_text_runs_emitted));
    append_profile_counter(
        stream,
        "frame_decoration_rects_emitted",
        static_cast<std::uint64_t>(stats.decoration_rects_emitted));
    append_profile_counter(
        stream,
        "frame_cursor_rects_emitted",
        static_cast<std::uint64_t>(stats.cursor_rects_emitted));
    append_profile_counter(
        stream,
        "frame_overlay_rects_emitted",
        static_cast<std::uint64_t>(stats.overlay_rects_emitted));
}

QString profile_string_literal(const QString& value)
{
    QString out;
    out.reserve(value.size() + 2);
    out += QLatin1Char('"');
    for (const QChar character : value) {
        const ushort code_unit = character.unicode();
        switch (code_unit) {
            case '\\': out += QStringLiteral("\\\\"); break;
            case '"':  out += QStringLiteral("\\\""); break;
            case '\n': out += QStringLiteral("\\n");  break;
            case '\r': out += QStringLiteral("\\r");  break;
            case '\t': out += QStringLiteral("\\t");  break;
            default:
                if (code_unit < 0x20U || code_unit == 0x7FU) {
                    out += QStringLiteral("\\u%1")
                        .arg(code_unit, 4, 16, QLatin1Char('0'))
                        .toUpper();
                }
                else {
                    out += character;
                }
                break;
        }
    }
    out += QLatin1Char('"');
    return out;
}

void append_profile_node_text(
    QTextStream&                           stream,
    const term::Profile_node_snapshot&     node,
    int                                    depth)
{
    const QString indent(depth * 2, QLatin1Char(' '));
    stream
        << indent
        << QString::fromStdString(node.name)
        << " calls="    << static_cast<qulonglong>(node.call_count)
        << " total_ns=" << profile_nanoseconds(node.total_time)
        << " mean_ns="  << profile_mean_nanoseconds(node.total_time, node.call_count)
        << " self_ns="  << profile_nanoseconds(node.self_time)
        << " child_ns=" << profile_nanoseconds(node.child_time)
        << " min_ns="   << profile_nanoseconds(node.min_time)
        << " max_ns="   << profile_nanoseconds(node.max_time)
        << '\n';

    for (const term::Profile_node_snapshot& child : node.children) {
        append_profile_node_text(stream, child, depth + 1);
    }
}

void append_profile_timeline_text(
    QTextStream&                           stream,
    const QString&                         label,
    const term::Profile_timeline_snapshot& timeline)
{
    stream
        << label
        << "_timeline bucket_width_ms="
        << static_cast<qulonglong>(timeline.bucket_width.count())
        << " buckets=" << static_cast<qulonglong>(timeline.buckets.size())
        << '\n';

    for (const term::Profile_timeline_bucket_snapshot& bucket : timeline.buckets) {
        if (bucket.scopes.empty()) {
            continue;
        }

        stream
            << "  bucket start_ms="
            << static_cast<qulonglong>(bucket.start_time.count())
            << " end_ms=" << static_cast<qulonglong>(bucket.end_time.count())
            << " scopes=" << static_cast<qulonglong>(bucket.scopes.size())
            << '\n';
        for (const term::Profile_timeline_scope_snapshot& scope : bucket.scopes) {
            stream
                << "    " << QString::fromStdString(scope.name)
                << " calls="    << static_cast<qulonglong>(scope.call_count)
                << " total_ns=" << profile_nanoseconds(scope.total_time)
                << " mean_ns="
                << profile_mean_nanoseconds(scope.total_time, scope.call_count)
                << " min_ns="   << profile_nanoseconds(scope.min_time)
                << " max_ns="   << profile_nanoseconds(scope.max_time)
                << '\n';
        }
    }
}

void append_dirty_row_stats_section(
    QTextStream&           stream,
    const term::Terminal_screen_model_dirty_row_stats&
                           stats)
{
    stream << "dirty_rows\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "mark_requests", stats.mark_requests);
    append_profile_counter(
        stream,
        "duplicate_mark_requests",
        stats.duplicate_mark_requests);
    append_profile_counter(
        stream,
        "out_of_bounds_mark_requests",
        stats.out_of_bounds_mark_requests);
    append_profile_counter(
        stream,
        "unique_pending_row_marks",
        stats.unique_pending_row_marks);
    append_profile_counter(stream, "mark_all_dirty_calls", stats.mark_all_dirty_calls);
    append_profile_counter(
        stream,
        "dirty_rows_snapshot_calls",
        stats.dirty_rows_snapshot_calls);
    append_profile_counter(
        stream,
        "dirty_rows_snapshot_rows",
        stats.dirty_rows_snapshot_rows);
    append_profile_counter(
        stream,
        "collect_synchronized_calls",
        stats.collect_synchronized_calls);
    append_profile_counter(
        stream,
        "collect_synchronized_rows",
        stats.collect_synchronized_rows);
    append_profile_counter(stream, "publish_pending_calls", stats.publish_pending_calls);
    append_profile_counter(stream, "published_unique_rows", stats.published_unique_rows);
    append_profile_counter(
        stream,
        "release_synchronized_calls",
        stats.release_synchronized_calls);
    append_profile_counter(
        stream,
        "released_synchronized_rows",
        stats.released_synchronized_rows);
    append_profile_counter(
        stream,
        "max_pending_dirty_rows",
        stats.max_pending_dirty_rows);
    append_profile_counter(
        stream,
        "max_synchronized_dirty_rows",
        stats.max_synchronized_dirty_rows);
}

bool dirty_row_bucket_has_activity(
    const term::Terminal_screen_model_dirty_row_bucket_stats& bucket)
{
    return
        bucket.mark_requests              != 0U ||
        bucket.dirty_rows_snapshot_calls  != 0U ||
        bucket.collect_synchronized_calls != 0U ||
        bucket.publish_pending_calls      != 0U ||
        bucket.release_synchronized_calls != 0U;
}

void append_dirty_row_timeline_section(
    QTextStream&           stream,
    const term::Terminal_screen_model_dirty_row_timeline&
                           timeline)
{
    stream
        << "dirty_row_timeline bucket_width_ms="
        << static_cast<qulonglong>(timeline.bucket_width_ms)
        << " buckets=" << static_cast<qulonglong>(timeline.buckets.size())
        << '\n';

    for (const term::Terminal_screen_model_dirty_row_bucket_stats& bucket :
        timeline.buckets)
    {
        if (!dirty_row_bucket_has_activity(bucket)) {
            continue;
        }

        stream
            << "  bucket start_ms=" << static_cast<qulonglong>(bucket.start_ms)
            << " end_ms="           << static_cast<qulonglong>(bucket.end_ms)
            << " mark_requests="    << static_cast<qulonglong>(bucket.mark_requests)
            << " duplicate_mark_requests="
            << static_cast<qulonglong>(bucket.duplicate_mark_requests)
            << " unique_pending_row_marks="
            << static_cast<qulonglong>(bucket.unique_pending_row_marks)
            << " mark_all_dirty_calls="
            << static_cast<qulonglong>(bucket.mark_all_dirty_calls)
            << " dirty_rows_snapshot_calls="
            << static_cast<qulonglong>(bucket.dirty_rows_snapshot_calls)
            << " dirty_rows_snapshot_rows="
            << static_cast<qulonglong>(bucket.dirty_rows_snapshot_rows)
            << " collect_synchronized_calls="
            << static_cast<qulonglong>(bucket.collect_synchronized_calls)
            << " collect_synchronized_rows="
            << static_cast<qulonglong>(bucket.collect_synchronized_rows)
            << " publish_pending_calls="
            << static_cast<qulonglong>(bucket.publish_pending_calls)
            << " published_unique_rows="
            << static_cast<qulonglong>(bucket.published_unique_rows)
            << " release_synchronized_calls="
            << static_cast<qulonglong>(bucket.release_synchronized_calls)
            << " released_synchronized_rows="
            << static_cast<qulonglong>(bucket.released_synchronized_rows)
            << " max_pending_dirty_rows="
            << static_cast<qulonglong>(bucket.max_pending_dirty_rows)
            << " max_synchronized_dirty_rows="
            << static_cast<qulonglong>(bucket.max_synchronized_dirty_rows)
            << '\n';
    }
}

void append_model_profile_stats_section(
    QTextStream&                                      stream,
    const term::Terminal_screen_model_profile_stats&  stats)
{
    stream << "model_profile_stats\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "print_text_calls", stats.print_text_calls);
    append_profile_counter(stream, "printable_ascii_span_calls", stats.printable_ascii_span_calls);
    append_profile_counter(stream, "printable_ascii_span_characters", stats.printable_ascii_span_characters);
    append_profile_counter(stream, "printable_ascii_cells_written", stats.printable_ascii_cells_written);
    append_profile_counter(
        stream,
        "max_printable_ascii_span_characters",
        stats.max_printable_ascii_span_characters);
    append_profile_counter(
        stream,
        "printable_ascii_local_cells_inspected",
        stats.printable_ascii_local_cells_inspected);
    append_profile_counter(
        stream,
        "scalar_span_local_cells_inspected",
        stats.scalar_span_local_cells_inspected);
    append_profile_counter(
        stream,
        "row_content_generation_comparisons",
        stats.row_content_generation_comparisons);
    append_profile_counter(
        stream,
        "row_content_generation_comparison_cells",
        stats.row_content_generation_comparison_cells);
    append_profile_counter(stream, "row_content_generation_advances", stats.row_content_generation_advances);
    append_profile_counter(
        stream,
        "wide_boundary_repairs_from_text_writes",
        stats.wide_boundary_repairs_from_text_writes);
    append_profile_counter(stream, "dirty_marks_from_text_writes", stats.dirty_marks_from_text_writes);
    append_profile_counter(stream, "line_wraps_from_text_writes", stats.line_wraps_from_text_writes);
    append_profile_counter(
        stream,
        "scrollback_appends_from_text_writes",
        stats.scrollback_appends_from_text_writes);
    append_profile_counter(stream, "render_snapshot_requests", stats.render_snapshot_requests);
    append_profile_counter(stream, "render_snapshots_constructed", stats.render_snapshots_constructed);
    append_profile_counter(stream, "render_snapshot_rows_visited", stats.render_snapshot_rows_visited);
    append_profile_counter(
        stream,
        "render_snapshot_rows_materialized",
        stats.render_snapshot_rows_materialized);
    append_profile_counter(stream, "render_snapshot_rows_borrowed", stats.render_snapshot_rows_borrowed);
    append_profile_counter(stream, "render_snapshot_rows_owned", stats.render_snapshot_rows_owned);
    append_profile_counter(
        stream,
        "render_snapshot_rows_built_from_model_storage",
        stats.render_snapshot_rows_built_from_model_storage);
    append_profile_counter(
        stream,
        "render_snapshot_model_row_accessor_borrows",
        stats.render_snapshot_model_row_accessor_borrows);
    append_profile_counter(stream, "render_snapshot_cells_scanned", stats.render_snapshot_cells_scanned);
    append_profile_counter(stream, "render_snapshot_cells_emitted", stats.render_snapshot_cells_emitted);
    append_profile_counter(
        stream,
        "render_snapshot_compact_empty_text_cells",
        stats.render_snapshot_compact_empty_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_compact_ascii_text_cells",
        stats.render_snapshot_compact_ascii_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_inline_single_bmp_text_cells",
        stats.render_snapshot_inline_single_bmp_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_qstring_copies",
        stats.render_snapshot_fallback_qstring_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_text_code_units_copied",
        stats.render_snapshot_fallback_text_code_units_copied);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_printable_ascii_copies",
        stats.render_snapshot_fallback_printable_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_other_ascii_copies",
        stats.render_snapshot_fallback_other_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_single_non_ascii_copies",
        stats.render_snapshot_fallback_single_non_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_multi_text_copies",
        stats.render_snapshot_fallback_multi_text_copies);
    append_profile_counter(
        stream,
        "render_snapshot_unoccupied_cells_skipped",
        stats.render_snapshot_unoccupied_cells_skipped);
    append_profile_counter(
        stream,
        "render_snapshot_dirty_rows_requested",
        stats.render_snapshot_dirty_rows_requested);
    append_profile_counter(
        stream,
        "render_snapshot_dirty_rows_visible",
        stats.render_snapshot_dirty_rows_visible);
    append_profile_counter(
        stream,
        "render_snapshot_full_repaint_fallbacks",
        stats.render_snapshot_full_repaint_fallbacks);
    append_profile_counter(
        stream,
        "render_snapshot_viewport_fallbacks",
        stats.render_snapshot_viewport_fallbacks);
    append_profile_counter(
        stream,
        "render_snapshot_zero_dirty_publications",
        stats.render_snapshot_zero_dirty_publications);
    append_profile_counter(
        stream,
        "max_render_snapshot_rows_visited",
        stats.max_render_snapshot_rows_visited);
    append_profile_counter(
        stream,
        "max_render_snapshot_cells_emitted",
        stats.max_render_snapshot_cells_emitted);
    append_profile_counter(
        stream,
        "max_render_snapshot_fallback_text_units_per_cell",
        stats.max_render_snapshot_fallback_text_units_per_cell);
}

void append_session_profile_stats_section(
    QTextStream&                              stream,
    const term::Terminal_session_profile_stats& stats)
{
    stream << "session_profile_stats\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "render_snapshot_requests", stats.render_snapshot_requests);
    append_profile_counter(stream, "render_snapshots_constructed", stats.render_snapshots_constructed);
    append_profile_counter(stream, "render_snapshot_publications", stats.render_snapshot_publications);
    append_profile_counter(stream, "full_snapshot_publications", stats.full_snapshot_publications);
    append_profile_counter(stream, "content_snapshot_publications", stats.content_snapshot_publications);
    append_profile_counter(stream, "selection_snapshot_publications", stats.selection_snapshot_publications);
    append_profile_counter(stream, "geometry_snapshot_publications", stats.geometry_snapshot_publications);
    append_profile_counter(
        stream,
        "public_projection_scroll_requests",
        stats.public_projection_scroll_requests);
    append_profile_counter(
        stream,
        "public_projection_scroll_publications",
        stats.public_projection_scroll_publications);
    append_profile_counter(stream, "dirty_coalescing_attempts", stats.dirty_coalescing_attempts);
    append_profile_counter(stream, "dirty_coalescing_applied", stats.dirty_coalescing_applied);
    append_profile_counter(stream, "zero_dirty_snapshot_publications", stats.zero_dirty_snapshot_publications);
    append_profile_counter(
        stream,
        "snapshots_superseded_before_render",
        stats.snapshots_superseded_before_render);
    append_profile_counter(stream, "snapshots_marked_rendered", stats.snapshots_marked_rendered);
    append_profile_counter(stream, "snapshots_consumed_by_bridge", stats.snapshots_consumed_by_bridge);
    append_profile_counter(
        stream,
        "max_unrendered_snapshot_generations",
        stats.max_unrendered_snapshot_generations);
    stream << "  consumer_materialization_counters_available=true\n";
    stream << "  consumer_materialization_counters_schema_semantics="
        << "batch_3_materialization_boundaries\n";
    stream << "  consumer_materialization_counters_owner_batch=Batch 3\n";
    append_profile_counter(
        stream,
        "consumer_materialization_counters_geometry_derived_snapshot_calls",
        stats.geometry_derived_materialization_calls);
    append_profile_counter(
        stream,
        "consumer_materialization_counters_geometry_derived_snapshot_rows",
        stats.geometry_derived_materialization_rows);
    append_profile_counter(
        stream,
        "consumer_materialization_counters_geometry_derived_snapshot_cells",
        stats.geometry_derived_materialization_cells);
    append_profile_counter(
        stream,
        "retained_snapshot_payload_bytes",
        stats.retained_snapshot_payload_bytes);
    append_profile_counter(
        stream,
        "retained_snapshot_generation_count",
        stats.retained_snapshot_generation_count);
    append_profile_counter(
        stream,
        "max_retained_snapshot_payload_bytes",
        stats.max_retained_snapshot_payload_bytes);
    append_profile_counter(
        stream,
        "max_retained_snapshot_generation_count",
        stats.max_retained_snapshot_generation_count);
    stream << "  lazy_snapshot_fallback_reason_counters_available=true\n";
    stream << "  lazy_snapshot_fallback_reason_counters_schema_semantics="
        << "batch_5_lazy_eligibility\n";
    stream << "  lazy_snapshot_fallback_reason_counters_owner_batch=Batch 5\n";
    append_profile_counter(
        stream,
        "lazy_snapshot_eligibility_checks",
        stats.lazy_snapshot_eligibility_checks);
    append_profile_counter(
        stream,
        "lazy_snapshot_eligible_checks",
        stats.lazy_snapshot_eligible_checks);
    append_profile_counter(
        stream,
        "lazy_snapshot_materialization_mismatches_for_testing",
        stats.lazy_snapshot_materialization_mismatches_for_testing);
    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        append_profile_counter(
            stream,
            descriptor.profile_key,
            term::terminal_lazy_snapshot_fallback_reason_counter(
                stats.lazy_snapshot_fallback_reasons,
                descriptor));
    }
}

template<typename Renderer_stats>
void append_renderer_stats_section(
    QTextStream&                 stream,
    const Renderer_stats&        stats)
{
    stream << "last_renderer_stats\n";
    stream << "  paint_completed=" << (stats.paint_completed ? "true" : "false") << '\n';
    append_renderer_frame_stats_text(stream, stats.frame);
    append_profile_counter(
        stream,
        "text_content_rebuilds",
        static_cast<std::uint64_t>(stats.text_content_rebuilds));
    append_profile_counter(
        stream,
        "text_content_reused",
        static_cast<std::uint64_t>(stats.text_content_reused));
    append_profile_counter(
        stream,
        "text_content_removed",
        static_cast<std::uint64_t>(stats.text_content_removed));
    append_profile_counter(
        stream,
        "text_content_failures",
        static_cast<std::uint64_t>(stats.text_content_failures));
    append_profile_counter(
        stream,
        "atlas_work_created",
        static_cast<std::uint64_t>(stats.atlas_work_created));
    if constexpr (requires { stats.atlas_work_reused; }) {
        append_profile_counter(
            stream,
            "atlas_work_reused",
            static_cast<std::uint64_t>(stats.atlas_work_reused));
    }
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        static_cast<std::uint64_t>(
            stats.text_cache_entry_child_nodes_cleared_for_replacement));
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_removal",
        static_cast<std::uint64_t>(
            stats.text_cache_entry_child_nodes_cleared_for_removal));
    append_profile_counter(
        stream,
        "text_cache_entry_max_child_nodes_cleared",
        static_cast<std::uint64_t>(stats.text_cache_entry_max_child_nodes_cleared));
    detail::append_text_layout_stats_text(stream, stats);
    append_profile_counter(
        stream,
        "text_groups_considered",
        static_cast<std::uint64_t>(stats.text_groups_considered));
    append_profile_counter(
        stream,
        "text_groups_dirty",
        static_cast<std::uint64_t>(stats.text_groups_dirty));
    append_profile_counter(
        stream,
        "text_groups_clean",
        static_cast<std::uint64_t>(stats.text_groups_clean));
    append_profile_counter(
        stream,
        "text_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.text_clean_reuse_skips));
    append_profile_counter(
        stream,
        "text_resource_descriptor_reuses",
        static_cast<std::uint64_t>(stats.text_resource_descriptor_reuses));
    if constexpr (requires { stats.text_resource_descriptor_builds; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_builds));
    }
    if constexpr (requires { stats.text_resource_descriptor_builds_avoided; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds_avoided",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_builds_avoided));
    }
    if constexpr (requires { stats.text_resource_descriptor_clean_reuse_skips; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_clean_reuse_skips",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_clean_reuse_skips));
    }
    append_profile_counter(
        stream,
        "text_key_builds",
        static_cast<std::uint64_t>(stats.text_key_builds));
    append_profile_counter(
        stream,
        "text_dirty_row_ranges",
        static_cast<std::uint64_t>(stats.text_dirty_row_ranges));
    append_profile_counter(
        stream,
        "text_dirty_rows",
        static_cast<std::uint64_t>(stats.text_dirty_rows));
    append_profile_counter(
        stream,
        "text_runs_considered",
        static_cast<std::uint64_t>(stats.text_runs_considered));
    append_profile_counter(
        stream,
        "text_coalescing_candidate_groups",
        static_cast<std::uint64_t>(stats.text_coalescing_candidate_groups));
    append_profile_counter(
        stream,
        "text_coalescing_enabled_groups",
        static_cast<std::uint64_t>(stats.text_coalescing_enabled_groups));
    append_profile_counter(
        stream,
        "text_resource_rows_with_runs",
        static_cast<std::uint64_t>(stats.text_resource_rows_with_runs));
    append_profile_counter(
        stream,
        "text_resource_max_runs_after_coalescing_per_row",
        static_cast<std::uint64_t>(stats.text_resource_max_runs_after_coalescing_per_row));
    append_profile_counter(
        stream,
        "text_resource_runs_before_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_before_coalescing));
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_after_coalescing));
    append_profile_counter(
        stream,
        "text_dirty_descriptor_identical_rows",
        static_cast<std::uint64_t>(stats.text_dirty_descriptor_identical_rows));
    append_profile_counter(
        stream,
        "text_key_match_reuses",
        static_cast<std::uint64_t>(stats.text_key_match_reuses));
    append_profile_counter(
        stream,
        "text_dirty_rows_rebuilt",
        static_cast<std::uint64_t>(stats.text_dirty_rows_rebuilt));
    append_profile_counter(
        stream,
        "text_clean_rows_rebuilt",
        static_cast<std::uint64_t>(stats.text_clean_rows_rebuilt));
    if constexpr (requires { stats.text_dirty_rebuilds_without_old_slot; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_without_old_slot",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_without_old_slot));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_frame_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_frame_key_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_frame_key_mismatch));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_ineligible; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_ineligible",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_descriptor_ineligible));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_old_descriptor_missing; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_old_descriptor_missing",
            static_cast<std::uint64_t>(
                stats.text_dirty_rebuilds_with_old_descriptor_missing));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_descriptor_mismatch));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_run_count; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_run_count",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_run_count));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_text; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_text",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_text));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_foreground; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_foreground",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_foreground));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_geometry; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_geometry",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_geometry));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_baseline; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_baseline",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_baseline));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_key_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_key_mismatch));
    }
    append_profile_counter(
        stream,
        "rect_resource_rects_before_coalescing",
        static_cast<std::uint64_t>(stats.rect_resource_rects_before_coalescing));
    append_profile_counter(
        stream,
        "rect_resource_rects_after_coalescing",
        static_cast<std::uint64_t>(stats.rect_resource_rects_after_coalescing));
    append_profile_counter(
        stream,
        "text_cache_entries_created",
        static_cast<std::uint64_t>(stats.text_cache_entries_created));
    append_profile_counter(
        stream,
        "text_cache_entries_replaced",
        static_cast<std::uint64_t>(stats.text_cache_entries_replaced));
    stream
        << "  text_wrapper_order_rebuilt="
        << (stats.text_wrapper_order_rebuilt ? "true" : "false") << '\n';
    stream
        << "  background_layer_rebuilt="
        << (stats.background_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  selection_layer_rebuilt="
        << (stats.selection_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  graphic_layer_rebuilt="
        << (stats.graphic_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  decoration_layer_rebuilt="
        << (stats.decoration_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  cursor_layer_rebuilt="
        << (stats.cursor_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  cursor_text_layer_rebuilt="
        << (stats.cursor_text_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  overlay_layer_rebuilt="
        << (stats.overlay_layer_rebuilt ? "true" : "false") << '\n';
    append_profile_counter(
        stream,
        "background_rows_rebuilt",
        static_cast<std::uint64_t>(stats.background_rows_rebuilt));
    append_profile_counter(
        stream,
        "background_rows_reused",
        static_cast<std::uint64_t>(stats.background_rows_reused));
    append_profile_counter(
        stream,
        "background_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.background_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "background_rows_removed",
        static_cast<std::uint64_t>(stats.background_rows_removed));
    append_profile_counter(
        stream,
        "background_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.background_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "selection_rows_rebuilt",
        static_cast<std::uint64_t>(stats.selection_rows_rebuilt));
    append_profile_counter(
        stream,
        "selection_rows_reused",
        static_cast<std::uint64_t>(stats.selection_rows_reused));
    append_profile_counter(
        stream,
        "selection_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.selection_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "selection_rows_removed",
        static_cast<std::uint64_t>(stats.selection_rows_removed));
    append_profile_counter(
        stream,
        "selection_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.selection_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "decoration_rows_rebuilt",
        static_cast<std::uint64_t>(stats.decoration_rows_rebuilt));
    append_profile_counter(
        stream,
        "decoration_rows_reused",
        static_cast<std::uint64_t>(stats.decoration_rows_reused));
    append_profile_counter(
        stream,
        "decoration_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.decoration_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "decoration_rows_removed",
        static_cast<std::uint64_t>(stats.decoration_rows_removed));
    append_profile_counter(
        stream,
        "decoration_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.decoration_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "graphic_rect_rows_rebuilt",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_rebuilt));
    append_profile_counter(
        stream,
        "graphic_rect_rows_reused",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_reused));
    append_profile_counter(
        stream,
        "graphic_rect_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.graphic_rect_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "graphic_rect_rows_removed",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_removed));
    append_profile_counter(
        stream,
        "graphic_rect_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.graphic_rect_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "graphic_arc_rows_rebuilt",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_rebuilt));
    append_profile_counter(
        stream,
        "graphic_arc_rows_reused",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_reused));
    append_profile_counter(
        stream,
        "graphic_arc_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.graphic_arc_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "graphic_arc_rows_removed",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_removed));
    append_profile_counter(
        stream,
        "graphic_arc_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.graphic_arc_row_cache_fallbacks));
}

template<typename Cumulative_renderer_stats>
void append_cumulative_renderer_stats_section(
    QTextStream&                       stream,
    const Cumulative_renderer_stats&   stats)
{
    stream << "cumulative_renderer_stats\n";
    append_profile_counter(stream, "frames_published",       stats.frames_published);
    append_profile_counter(stream, "paint_completed_frames", stats.paint_completed_frames);
    append_profile_counter(stream, "root_reused_frames",     stats.root_reused_frames);
    append_renderer_frame_stats_text(stream, stats.frame);
    append_profile_counter(stream, "text_content_rebuilds",   stats.text_content_rebuilds);
    append_profile_counter(stream, "text_content_reused",     stats.text_content_reused);
    append_profile_counter(stream, "text_content_removed",    stats.text_content_removed);
    append_profile_counter(stream, "text_content_failures",   stats.text_content_failures);
    append_profile_counter(stream, "atlas_work_created", stats.atlas_work_created);
    if constexpr (requires { stats.atlas_work_reused; }) {
        append_profile_counter(stream, "atlas_work_reused", stats.atlas_work_reused);
    }
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        stats.text_cache_entry_child_nodes_cleared_for_replacement);
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_removal",
        stats.text_cache_entry_child_nodes_cleared_for_removal);
    append_profile_counter(
        stream,
        "text_cache_entry_max_child_nodes_cleared",
        stats.text_cache_entry_max_child_nodes_cleared);
    detail::append_text_layout_stats_text(stream, stats);
    append_profile_counter(stream, "text_groups_considered", stats.text_groups_considered);
    append_profile_counter(stream, "text_groups_dirty",      stats.text_groups_dirty);
    append_profile_counter(stream, "text_groups_clean",      stats.text_groups_clean);
    append_profile_counter(stream, "text_clean_reuse_skips", stats.text_clean_reuse_skips);
    append_profile_counter(
        stream,
        "text_resource_descriptor_reuses",
        stats.text_resource_descriptor_reuses);
    if constexpr (requires { stats.text_resource_descriptor_builds; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds",
            stats.text_resource_descriptor_builds);
    }
    if constexpr (requires { stats.text_resource_descriptor_builds_avoided; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds_avoided",
            stats.text_resource_descriptor_builds_avoided);
    }
    if constexpr (requires { stats.text_resource_descriptor_clean_reuse_skips; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_clean_reuse_skips",
            stats.text_resource_descriptor_clean_reuse_skips);
    }
    append_profile_counter(stream, "text_key_builds",       stats.text_key_builds);
    append_profile_counter(stream, "text_dirty_row_ranges", stats.text_dirty_row_ranges);
    append_profile_counter(stream, "text_dirty_rows",       stats.text_dirty_rows);
    append_profile_counter(stream, "text_runs_considered",  stats.text_runs_considered);
    append_profile_counter(
        stream,
        "text_coalescing_candidate_groups",
        stats.text_coalescing_candidate_groups);
    append_profile_counter(
        stream,
        "text_coalescing_enabled_groups",
        stats.text_coalescing_enabled_groups);
    append_profile_counter(
        stream,
        "text_resource_rows_with_runs",
        stats.text_resource_rows_with_runs);
    append_profile_counter(
        stream,
        "text_resource_max_runs_after_coalescing_per_row",
        stats.text_resource_max_runs_after_coalescing_per_row);
    append_profile_counter(
        stream,
        "text_resource_runs_before_coalescing",
        stats.text_resource_runs_before_coalescing);
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        stats.text_resource_runs_after_coalescing);
    append_profile_counter(
        stream,
        "text_dirty_descriptor_identical_rows",
        stats.text_dirty_descriptor_identical_rows);
    append_profile_counter(
        stream,
        "text_key_match_reuses",
        stats.text_key_match_reuses);
    append_profile_counter(
        stream,
        "text_dirty_rows_rebuilt",
        stats.text_dirty_rows_rebuilt);
    append_profile_counter(
        stream,
        "text_clean_rows_rebuilt",
        stats.text_clean_rows_rebuilt);
    if constexpr (requires { stats.text_dirty_rebuilds_without_old_slot; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_without_old_slot",
            stats.text_dirty_rebuilds_without_old_slot);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_frame_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_frame_key_mismatch",
            stats.text_dirty_rebuilds_with_frame_key_mismatch);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_ineligible; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_ineligible",
            stats.text_dirty_rebuilds_with_descriptor_ineligible);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_old_descriptor_missing; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_old_descriptor_missing",
            stats.text_dirty_rebuilds_with_old_descriptor_missing);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_mismatch",
            stats.text_dirty_rebuilds_with_descriptor_mismatch);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_run_count; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_run_count",
            stats.text_descriptor_mismatch_run_count);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_text; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_text",
            stats.text_descriptor_mismatch_text);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_foreground; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_foreground",
            stats.text_descriptor_mismatch_foreground);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_geometry; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_geometry",
            stats.text_descriptor_mismatch_geometry);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_baseline; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_baseline",
            stats.text_descriptor_mismatch_baseline);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_key_mismatch",
            stats.text_dirty_rebuilds_with_key_mismatch);
    }
    append_profile_counter(
        stream,
        "rect_resource_rects_before_coalescing",
        stats.rect_resource_rects_before_coalescing);
    append_profile_counter(
        stream,
        "rect_resource_rects_after_coalescing",
        stats.rect_resource_rects_after_coalescing);
    append_profile_counter(
        stream,
        "text_cache_entries_created",
        stats.text_cache_entries_created);
    append_profile_counter(
        stream,
        "text_cache_entries_replaced",
        stats.text_cache_entries_replaced);
    append_profile_counter(
        stream,
        "text_wrapper_order_rebuilds",
        stats.text_wrapper_order_rebuilds);
    append_profile_counter(stream, "background_layer_rebuilds", stats.background_layer_rebuilds);
    append_profile_counter(stream, "selection_layer_rebuilds",  stats.selection_layer_rebuilds);
    append_profile_counter(stream, "graphic_layer_rebuilds",    stats.graphic_layer_rebuilds);
    append_profile_counter(stream, "decoration_layer_rebuilds", stats.decoration_layer_rebuilds);
    append_profile_counter(stream, "cursor_layer_rebuilds",     stats.cursor_layer_rebuilds);
    append_profile_counter(stream, "cursor_text_layer_rebuilds", stats.cursor_text_layer_rebuilds);
    append_profile_counter(stream, "overlay_layer_rebuilds",     stats.overlay_layer_rebuilds);
    append_profile_counter(stream, "background_rows_rebuilt",    stats.background_rows_rebuilt);
    append_profile_counter(stream, "background_rows_reused",     stats.background_rows_reused);
    append_profile_counter(
        stream,
        "background_row_clean_reuse_skips",
        stats.background_row_clean_reuse_skips);
    append_profile_counter(stream, "background_rows_removed", stats.background_rows_removed);
    append_profile_counter(
        stream,
        "background_row_cache_fallbacks",
        stats.background_row_cache_fallbacks);
    append_profile_counter(stream, "selection_rows_rebuilt", stats.selection_rows_rebuilt);
    append_profile_counter(stream, "selection_rows_reused",  stats.selection_rows_reused);
    append_profile_counter(
        stream,
        "selection_row_clean_reuse_skips",
        stats.selection_row_clean_reuse_skips);
    append_profile_counter(stream, "selection_rows_removed", stats.selection_rows_removed);
    append_profile_counter(
        stream,
        "selection_row_cache_fallbacks",
        stats.selection_row_cache_fallbacks);
    append_profile_counter(stream, "decoration_rows_rebuilt", stats.decoration_rows_rebuilt);
    append_profile_counter(stream, "decoration_rows_reused",  stats.decoration_rows_reused);
    append_profile_counter(
        stream,
        "decoration_row_clean_reuse_skips",
        stats.decoration_row_clean_reuse_skips);
    append_profile_counter(stream, "decoration_rows_removed", stats.decoration_rows_removed);
    append_profile_counter(
        stream,
        "decoration_row_cache_fallbacks",
        stats.decoration_row_cache_fallbacks);
    append_profile_counter(stream, "graphic_rect_rows_rebuilt", stats.graphic_rect_rows_rebuilt);
    append_profile_counter(stream, "graphic_rect_rows_reused",  stats.graphic_rect_rows_reused);
    append_profile_counter(
        stream,
        "graphic_rect_row_clean_reuse_skips",
        stats.graphic_rect_row_clean_reuse_skips);
    append_profile_counter(stream, "graphic_rect_rows_removed", stats.graphic_rect_rows_removed);
    append_profile_counter(
        stream,
        "graphic_rect_row_cache_fallbacks",
        stats.graphic_rect_row_cache_fallbacks);
    append_profile_counter(stream, "graphic_arc_rows_rebuilt", stats.graphic_arc_rows_rebuilt);
    append_profile_counter(stream, "graphic_arc_rows_reused",  stats.graphic_arc_rows_reused);
    append_profile_counter(
        stream,
        "graphic_arc_row_clean_reuse_skips",
        stats.graphic_arc_row_clean_reuse_skips);
    append_profile_counter(stream, "graphic_arc_rows_removed", stats.graphic_arc_rows_removed);
    append_profile_counter(
        stream,
        "graphic_arc_row_cache_fallbacks",
        stats.graphic_arc_row_cache_fallbacks);
}

void append_slow_text_layout_diagnostics_section(
    QTextStream&           stream,
    const term::terminal_text_layout_slow_diagnostics_t&
                           diagnostics)
{
    stream
        << "slow_text_layouts"
        << " threshold_ns="   << static_cast<qulonglong>(diagnostics.threshold_ns)
        << " slow_calls="     << static_cast<qulonglong>(diagnostics.slow_call_count)
        << " stored_samples=" << static_cast<qulonglong>(diagnostics.samples.size())
        << '\n';

    int index = 0;
    for (const term::terminal_text_layout_slow_diagnostic_t& sample :
        diagnostics.samples)
    {
        stream
            << "  sample index="    << index
            << " duration_ns="      << static_cast<qulonglong>(sample.duration_ns)
            << " text_utf16_units=" << sample.text_utf16_units
            << " text_codepoints="  << sample.text_codepoints
            << " text_hash="        << static_cast<qulonglong>(sample.text_hash)
            << " row="              << sample.row
            << " logical_row="      << sample.logical_row
            << " column="           << sample.column
            << " style_id="         << sample.style_id
            << " hyperlink_id="     << static_cast<qulonglong>(sample.hyperlink_id)
            << " rect_width="       << sample.rect_width
            << " rect_height="      << sample.rect_height
            << " ascii_only="       << (sample.ascii_only ? "true" : "false")
            << " printable_ascii_only="
            << (sample.printable_ascii_only ? "true" : "false")
            << " has_control_codepoint="
            << (sample.has_control_codepoint ? "true" : "false")
            << " clipped="          << (sample.clipped ? "true" : "false")
            << " force_blended_order="
            << (sample.force_blended_order ? "true" : "false")
            << " ascii_layout_font="
            << (sample.ascii_layout_font ? "true" : "false")
            << " line_has_text="    << (sample.line_has_text ? "true" : "false")
            << " font_family="      << profile_string_literal(sample.font_family)
            << " font_style_name="
            << profile_string_literal(sample.font_style_name)
            << " resolved_font_family="
            << profile_string_literal(sample.resolved_font_family)
            << " resolved_font_style_name="
            << profile_string_literal(sample.resolved_font_style_name)
            << " font_point_size="  << sample.font_point_size
            << " font_pixel_size="  << sample.font_pixel_size
            << " font_weight="      << sample.font_weight
            << " font_italic="      << (sample.font_italic ? "true" : "false")
            << " codepoints="
            << profile_string_literal(sample.codepoint_sample)
            << " text_preview_truncated="
            << (sample.text_preview_truncated ? "true" : "false")
            << " text_preview="
            << profile_string_literal(sample.text_preview)
            << '\n';
        ++index;
    }
}

void append_qsg_atlas_profile_section(
    QTextStream&                         stream,
    const term::Qsg_atlas_frame_report&  report)
{
    const term::Glyph_coverage_counts& coverage =
        report.frame_build.glyph_coverage;

    stream << "qsg_atlas\n";
    stream << "  renderer=atlas\n";
    stream << "  text_renderer_policy="
        << QString::fromLatin1(
            term::qsg_atlas_text_renderer_policy_name(
                report.text_renderer_policy))
        << '\n';
    stream << "  effective_text_renderer="
        << QString::fromLatin1(
            term::qsg_atlas_text_renderer_kind_name(
                report.effective_text_renderer))
        << '\n';
    stream << "  msdf_lcd_subpixel_order="
        << QString::fromLatin1(
            term::qsg_atlas_lcd_subpixel_order_name(
                report.msdf_lcd_subpixel_order))
        << '\n';
    append_profile_bool(
        stream,
        "msdf_lcd_text_enabled",
        report.msdf_lcd_text_enabled);
    append_profile_bool(
        stream,
        "text_renderer_fallback_allowed",
        report.text_renderer_fallback_allowed);
    append_profile_bool(
        stream,
        "text_renderer_fallback_used",
        report.text_renderer_fallback_used);
    stream << "  sampler_mode="
        << QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(report.render.glyph_sampler_mode))
        << '\n';
    detail::emit_metrics_text(stream, report, detail::atlas_report_sequence_metrics());
    stream
        << "  command_buffer_non_null="
        << (report.command_buffer_non_null ? "true" : "false") << '\n';
    stream
        << "  render_target_non_null="
        << (report.render_target_non_null ? "true" : "false") << '\n';
    stream << "  rhi_non_null=" << (report.rhi_non_null ? "true" : "false") << '\n';
    stream << "  drew=" << (report.drew ? "true" : "false") << '\n';
    append_profile_bool(
        stream,
        "coverage_texture_created",
        report.coverage_texture_created);
    append_profile_bool(
        stream,
        "coverage_upload_recorded",
        report.coverage_upload_recorded);
    detail::emit_metrics_text(stream, report, detail::atlas_report_rasterization_metrics());
    append_profile_counter(
        stream,
        "max_glyph_instance_page",
        static_cast<std::uint64_t>(std::max(
            0,
            report.frame_build.max_glyph_instance_page)));
    const term::Qsg_atlas_producer_summary& producer = report.producer;
    stream << "  producer\n";
    detail::emit_metrics_text(stream, producer, detail::atlas_producer_metrics());
    const term::Qsg_atlas_warm_lazy_summary& warm_lazy = report.warm_lazy;
    stream << "  warm_lazy\n";
    detail::emit_metrics_text(
        stream, warm_lazy, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    stream << "  warm_elapsed_ms=" << warm_lazy.warm_elapsed_ms << '\n';
    detail::emit_metrics_text(
        stream, warm_lazy, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    stream << "  lazy_elapsed_ms=" << warm_lazy.lazy_elapsed_ms << '\n';
    detail::emit_metrics_text(
        stream, warm_lazy, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    stream << "  placement\n";
    append_profile_counter(
        stream,
        "snapped_origin_failures",
        static_cast<std::uint64_t>(report.frame_build.snapped_origin_failures));
    stream << "  misses\n";
    append_profile_counter(
        stream,
        "glyph_missed_instances",
        static_cast<std::uint64_t>(report.frame_build.glyph_missed_instances));
    append_profile_counter(
        stream,
        "glyph_coverage_failures",
        static_cast<std::uint64_t>(report.frame_build.glyph_coverage_failures));
    append_profile_counter(
        stream,
        "glyph_atlas_insert_failures",
        static_cast<std::uint64_t>(report.frame_build.glyph_atlas_insert_failures));
    if (report.frame_build.first_glyph_miss.valid) {
        const term::Qsg_atlas_glyph_miss_diagnostic& miss =
            report.frame_build.first_glyph_miss;
        stream << "  first_glyph_miss\n";
        stream << "  cause="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_miss_cause_name(miss.cause))
            << '\n';
        stream << "  coverage_kind="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_coverage_kind_name(
                    miss.image.coverage_kind))
            << '\n';
        stream << "  presentation="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_image_presentation_name(
                    miss.image.presentation))
            << '\n';
        append_profile_counter(
            stream,
            "glyph_index",
            static_cast<std::uint64_t>(miss.image.glyph_index));
        stream << "  fallback_face_id="
            << profile_string_literal(miss.image.fallback_face_id)
            << '\n';
        append_profile_counter(
            stream,
            "source_format",
            static_cast<std::uint64_t>(miss.image.source_format));
        append_profile_counter(
            stream,
            "source_string_start",
            static_cast<std::uint64_t>(miss.image.source_string_start));
        append_profile_counter(
            stream,
            "source_string_end",
            static_cast<std::uint64_t>(miss.image.source_string_end));
        append_profile_counter(
            stream,
            "atlas_page_count",
            static_cast<std::uint64_t>(miss.atlas_page_count));
        append_profile_counter(
            stream,
            "atlas_page_budget",
            static_cast<std::uint64_t>(miss.atlas_page_budget));
    }
    stream << "  coverage\n";
    detail::emit_metrics_text(stream, coverage, detail::glyph_coverage_metrics());
    stream << "  buffer_upload\n";
    append_profile_counter(
        stream,
        "atlas_page_budget",
        report.render.atlas_page_budget);
    append_profile_counter(
        stream,
        "atlas_budget_bytes",
        report.render.atlas_budget_bytes);
    append_profile_counter(stream, "atlas_used_bytes", report.render.atlas_used_bytes);
    append_profile_counter(
        stream,
        "atlas_failed_inserts",
        report.render.atlas_failed_inserts);
    append_profile_counter(
        stream,
        "shaped_text_runs",
        static_cast<std::uint64_t>(report.render.shaped_text_runs));
    append_profile_counter(
        stream,
        "shaped_glyph_records",
        static_cast<std::uint64_t>(report.render.shaped_glyph_records));
    append_profile_counter(
        stream,
        "shaped_missing_string_indexes",
        static_cast<std::uint64_t>(
            report.render.shaped_missing_string_indexes));
    append_profile_counter(
        stream,
        "shaped_invalid_string_indexes",
        static_cast<std::uint64_t>(
            report.render.shaped_invalid_string_indexes));
    append_profile_bool(
        stream,
        "atlas_page_pressure",
        report.render.atlas_page_pressure);
    stream << "  render\n";
    append_profile_counter(stream, "draw_calls", report.render.draw_calls);
    append_profile_counter(stream, "rect_draw_calls", report.render.rect_draw_calls);
    append_profile_counter(stream, "glyph_draw_calls", report.render.glyph_draw_calls);
    append_profile_counter(
        stream,
        "rect_buffer_uploaded_bytes",
        report.render.rect_buffer.uploaded_bytes);
    append_profile_counter(
        stream,
        "glyph_buffer_uploaded_bytes",
        report.render.glyph_buffer.uploaded_bytes);
    stream << "  capabilities\n";
    detail::emit_metrics_text(stream, report.render, detail::atlas_capabilities_metrics());
}

}

void append_dirty_row_stats_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_dirty_row_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::dirty_row_stats(surface));
}

void append_dirty_row_timeline_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_dirty_row_timeline_section(
        out,
        term::VNM_TerminalSurface_render_bridge::dirty_row_timeline(surface));
}

void append_model_profile_stats_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_model_profile_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::model_profile_stats(surface));
}

void append_session_profile_stats_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_session_profile_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::session_profile_stats(surface));
}

void append_renderer_stats_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_renderer_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface));
}

void append_cumulative_renderer_stats_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out)
{
    append_cumulative_renderer_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(surface));
}

void append_qsg_atlas_profile_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_qsg_atlas_profile_section(
        out,
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface));
}

void append_slow_text_layout_diagnostics_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out)
{
    const term::Render_profile_snapshot_t render_profile =
        term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(surface);
    append_slow_text_layout_diagnostics_section(out, render_profile.slow_text_layouts);
}

void append_surface_geometry_profile_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out)
{
    const term::Qt_grid_metrics_provider metrics_provider(
        term::vnm_terminal_font(surface.font_family(), surface.font_size()),
        surface.window() != nullptr ? surface.window()->devicePixelRatio() : 1.0);
    const term::terminal_cell_metrics_t cell_metrics =
        metrics_provider.cell_metrics();
    const QQuickWindow* const window = surface.window();

    out << "surface_geometry\n";
    out << "  rows=" << surface.rows() << '\n';
    out << "  columns=" << surface.columns() << '\n';
    out << "  surface_width=" << surface.width() << '\n';
    out << "  surface_height=" << surface.height() << '\n';
    out << "  cell_width=" << cell_metrics.width << '\n';
    out << "  cell_height=" << cell_metrics.height << '\n';
    out << "  font_family=" << profile_string_literal(surface.font_family()) << '\n';
    out << "  font_size=" << surface.font_size() << '\n';
    if (window != nullptr) {
        out << "  window_width=" << window->width() << '\n';
        out << "  window_height=" << window->height() << '\n';
        out << "  device_pixel_ratio=" << window->devicePixelRatio() << '\n';
    }
}

void append_render_thread_profile_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out)
{
    const term::Render_profile_snapshot_t render_profile =
        term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(surface);

    out << "render_thread sequence=" << static_cast<qulonglong>(render_profile.sequence)
        << '\n';
    append_profile_node_text(out, render_profile.root, 1);
    out << '\n';
    append_profile_timeline_text(
        out,
        QStringLiteral("render_thread"),
        render_profile.timeline);
}

}

#endif
