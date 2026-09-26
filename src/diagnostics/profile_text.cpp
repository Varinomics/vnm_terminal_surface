#include "vnm_terminal/diagnostics/profile_text.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "atlas_metric_descriptors.h"
#include "metric_descriptor.h"
#include "model_profile_metric_descriptors.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/profile_text_writers.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QQuickWindow>
#include <QScreen>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <utility>

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

using profile_counter_t = std::pair<const char*, std::uint64_t>;

void append_profile_counters(
    QTextStream&                            stream,
    std::initializer_list<profile_counter_t> counters)
{
    for (const auto& [name, value] : counters) {
        append_profile_counter(stream, name, value);
    }
}

void append_profile_bool(
    QTextStream&               stream,
    const char*                name,
    bool                       value)
{
    stream << "  " << name << '=' << (value ? "true" : "false") << '\n';
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

void append_dirty_row_stats_section(
    QTextStream&           stream,
    const term::Terminal_screen_model_dirty_row_stats&
                           stats)
{
    stream << "dirty_rows\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counters(
        stream,
        {
            {"mark_requests", stats.mark_requests},
            {"duplicate_mark_requests", stats.duplicate_mark_requests},
            {"out_of_bounds_mark_requests", stats.out_of_bounds_mark_requests},
            {"unique_pending_row_marks", stats.unique_pending_row_marks},
            {"mark_all_dirty_calls", stats.mark_all_dirty_calls},
            {"dirty_rows_snapshot_calls", stats.dirty_rows_snapshot_calls},
            {"dirty_rows_snapshot_rows", stats.dirty_rows_snapshot_rows},
            {"collect_synchronized_calls", stats.collect_synchronized_calls},
            {"collect_synchronized_rows", stats.collect_synchronized_rows},
            {"publish_pending_calls", stats.publish_pending_calls},
            {"published_unique_rows", stats.published_unique_rows},
            {"release_synchronized_calls", stats.release_synchronized_calls},
            {"released_synchronized_rows", stats.released_synchronized_rows},
            {"max_pending_dirty_rows", stats.max_pending_dirty_rows},
            {"max_synchronized_dirty_rows", stats.max_synchronized_dirty_rows},
        });
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
    // The text-only erase counters sit between the first seven common counters and the rest.
    const auto metrics = detail::model_profile_counter_metrics();
    detail::emit_metrics_text(stream, stats, metrics.first(7));
    append_profile_counters(
        stream,
        {
            {"erase_row_range_calls", stats.erase_row_range_calls},
            {"erase_row_cells_visited", stats.erase_row_cells_visited},
            {"erase_row_cells_replaced", stats.erase_row_cells_replaced},
            {"erase_row_cells_already_erased", stats.erase_row_cells_already_erased},
            {"erase_row_wide_span_cells", stats.erase_row_wide_span_cells},
        });
    detail::emit_metrics_text(stream, stats, metrics.subspan(7));
}

void append_retained_history_profile_section(
    QTextStream&                                         stream,
    const term::terminal_retained_history_diagnostics_t& diagnostics)
{
    stream << "retained_history\n";
    detail::emit_metrics_text(
        stream,
        diagnostics,
        detail::retained_history_metrics<
            term::terminal_retained_history_diagnostics_t>());
    stream << "  prefix_plain_ascii_estimate\n";
    detail::emit_metrics_text(
        stream,
        diagnostics.prefix_plain_ascii_estimate,
        detail::retained_history_estimate_metrics<
            term::terminal_history_prefix_plain_ascii_retention_estimate_t>(),
        "    ");
}

void append_session_profile_stats_section(
    QTextStream&                              stream,
    const term::Terminal_session_profile_stats& stats)
{
    stream << "session_profile_stats\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counters(
        stream,
        {
            {"render_snapshot_requests", stats.render_snapshot_requests},
            {"render_snapshots_constructed", stats.render_snapshots_constructed},
            {"render_snapshot_publications", stats.render_snapshot_publications},
            {"full_snapshot_publications", stats.full_snapshot_publications},
            {"content_snapshot_publications", stats.content_snapshot_publications},
            {"selection_snapshot_publications", stats.selection_snapshot_publications},
            {"geometry_snapshot_publications", stats.geometry_snapshot_publications},
            {"public_projection_scroll_requests", stats.public_projection_scroll_requests},
            {"public_projection_scroll_publications", stats.public_projection_scroll_publications},
            {"dirty_coalescing_attempts", stats.dirty_coalescing_attempts},
            {"dirty_coalescing_applied", stats.dirty_coalescing_applied},
            {"zero_dirty_snapshot_publications", stats.zero_dirty_snapshot_publications},
            {"snapshots_superseded_before_render", stats.snapshots_superseded_before_render},
            {"snapshots_marked_rendered", stats.snapshots_marked_rendered},
            {"snapshots_consumed_by_bridge", stats.snapshots_consumed_by_bridge},
            {"max_unrendered_snapshot_generations", stats.max_unrendered_snapshot_generations},
        });
    stream << "  consumer_materialization_counters_available=true\n";
    stream << "  consumer_materialization_counters_schema_semantics="
        << "geometry_derived_snapshot_materialization_counters\n";
    stream << "  consumer_materialization_counters_owner_semantics="
        << "terminal_session_profile_stats\n";
    append_profile_counters(
        stream,
        {
            {"consumer_materialization_counters_geometry_derived_snapshot_calls",
             stats.geometry_derived_materialization_calls},
            {"consumer_materialization_counters_geometry_derived_snapshot_rows",
             stats.geometry_derived_materialization_rows},
            {"consumer_materialization_counters_geometry_derived_snapshot_cells",
             stats.geometry_derived_materialization_cells},
            {"retained_snapshot_payload_bytes", stats.retained_snapshot_payload_bytes},
            {"retained_snapshot_generation_count", stats.retained_snapshot_generation_count},
            {"max_retained_snapshot_payload_bytes", stats.max_retained_snapshot_payload_bytes},
            {"max_retained_snapshot_generation_count", stats.max_retained_snapshot_generation_count},
        });
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
    append_profile_counters(
        stream,
        {
            {"snapped_origin_failures",
             static_cast<std::uint64_t>(report.frame_build.snapped_origin_failures)},
            {"frame_row_descriptors", static_cast<std::uint64_t>(report.frame_build.frame_row_descriptors)},
            {"frame_layer_descriptors",
             static_cast<std::uint64_t>(report.frame_build.frame_layer_descriptors)},
            {"qsg_layer_descriptors", static_cast<std::uint64_t>(report.frame_build.qsg_layer_descriptors)},
        });
    stream << "  misses\n";
    append_profile_counters(
        stream,
        {
            {"glyph_missed_instances",
             static_cast<std::uint64_t>(report.frame_build.glyph_missed_instances)},
            {"glyph_coverage_failures",
             static_cast<std::uint64_t>(report.frame_build.glyph_coverage_failures)},
            {"glyph_atlas_insert_failures",
             static_cast<std::uint64_t>(report.frame_build.glyph_atlas_insert_failures)},
        });
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
        append_profile_counters(
            stream,
            {
                {"source_format", static_cast<std::uint64_t>(miss.image.source_format)},
                {"source_string_start", static_cast<std::uint64_t>(miss.image.source_string_start)},
                {"source_string_end", static_cast<std::uint64_t>(miss.image.source_string_end)},
                {"atlas_page_count", static_cast<std::uint64_t>(miss.atlas_page_count)},
                {"atlas_page_budget", static_cast<std::uint64_t>(miss.atlas_page_budget)},
            });
    }
    stream << "  coverage\n";
    detail::emit_metrics_text(stream, coverage, detail::glyph_coverage_metrics());
    stream << "  buffer_upload\n";
    append_profile_counters(
        stream,
        {
            {"atlas_page_budget", report.render.atlas_page_budget},
            {"atlas_budget_bytes", report.render.atlas_budget_bytes},
            {"atlas_used_bytes", report.render.atlas_used_bytes},
            {"atlas_failed_inserts", report.render.atlas_failed_inserts},
            {"shaped_text_runs", static_cast<std::uint64_t>(report.render.shaped_text_runs)},
            {"shaped_glyph_records", static_cast<std::uint64_t>(report.render.shaped_glyph_records)},
            {"shaped_missing_string_indexes", static_cast<std::uint64_t>(
                report.render.shaped_missing_string_indexes)},
            {"shaped_invalid_string_indexes", static_cast<std::uint64_t>(
                report.render.shaped_invalid_string_indexes)},
        });
    append_profile_bool(
        stream,
        "atlas_page_pressure",
        report.render.atlas_page_pressure);
    stream << "  render\n";
    append_profile_counters(
        stream,
        {
            {"draw_calls", report.render.draw_calls},
            {"rect_draw_calls", report.render.rect_draw_calls},
            {"glyph_draw_calls", report.render.glyph_draw_calls},
            {"rect_row_capacity", static_cast<std::uint64_t>(report.render.rect_row_capacity)},
            {"rect_buffer_uploaded_bytes", report.render.rect_buffer.uploaded_bytes},
            {"glyph_buffer_uploaded_bytes", report.render.glyph_buffer.uploaded_bytes},
        });
    stream << "  capabilities\n";
    detail::emit_metrics_text(stream, report.render, detail::atlas_capabilities_metrics());
    stream << "  images\n";
    detail::emit_metrics_text(stream, report.render.images, detail::atlas_image_metrics());
}

}

void append_profile_node_text(
    QTextStream&                            stream,
    const internal::Profile_node_snapshot&  node,
    int                                     depth)
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
    QTextStream&                                stream,
    const QString&                              label,
    const internal::Profile_timeline_snapshot&  timeline)
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

void append_retained_history_profile_text(
    const VNM_TerminalSurface& surface,
    QTextStream&               out)
{
    append_retained_history_profile_section(
        out,
        term::VNM_TerminalSurface_render_bridge::retained_history_diagnostics(surface));
}

void append_session_profile_stats_text(const VNM_TerminalSurface& surface, QTextStream& out)
{
    append_session_profile_stats_section(
        out,
        term::VNM_TerminalSurface_render_bridge::session_profile_stats(surface));
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
    const QQuickWindow* const window = surface.window();
    const QScreen* const screen = window != nullptr ? window->screen() : nullptr;
    const qreal logical_dpi = term::normalized_logical_dpi(
        screen != nullptr
            ? screen->logicalDotsPerInch()
            : term::k_vnm_terminal_default_logical_dpi);
    const qreal effective_font_size = vnm_terminal::effective_font_size_for_font(
        surface.font_family(),
        surface.font_size(),
        window != nullptr ? window->effectiveDevicePixelRatio() : 1.0,
        surface.font_advance_policy(),
        logical_dpi);
    const term::Qt_grid_metrics_provider metrics_provider(
        term::vnm_terminal_font(
            surface.font_family(),
            effective_font_size,
            logical_dpi),
        window != nullptr ? window->devicePixelRatio() : 1.0,
        surface.font_advance_policy(),
        logical_dpi);
    const term::terminal_cell_metrics_t cell_metrics =
        metrics_provider.cell_metrics();

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
