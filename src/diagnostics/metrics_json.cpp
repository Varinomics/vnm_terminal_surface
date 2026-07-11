#include "vnm_terminal/diagnostics/metrics_json.h"

#include "atlas_metric_descriptors.h"
#include "metric_descriptor.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"

#include <QString>

#include <algorithm>
#include <cstdint>

namespace vnm_terminal::diagnostics {

namespace internal = vnm_terminal::internal;

namespace {

template<typename Value>
void insert_json_counter(
    QJsonObject&  object,
    const char*   name,
    Value         value)
{
    object.insert(
        QString::fromLatin1(name),
        QString::number(static_cast<qulonglong>(value)));
}

QJsonObject atlas_buffer_summary_json(
    const internal::Qsg_atlas_buffer_update_summary& summary)
{
    QJsonObject object;
    insert_json_counter(object, "rhi_frames_in_flight", summary.rhi_frames_in_flight);
    insert_json_counter(object, "rhi_frame_slot", summary.rhi_frame_slot);
    insert_json_counter(object, "instance_count", summary.instance_count);
    insert_json_counter(
        object,
        "active_instance_count",
        summary.active_instance_count);
    insert_json_counter(object, "instance_bytes", summary.instance_bytes);
    insert_json_counter(object, "buffer_bytes", summary.buffer_bytes);
    insert_json_counter(object, "dirty_rows", summary.dirty_rows);
    insert_json_counter(object, "seeded_slots", summary.seeded_slots);
    insert_json_counter(object, "full_uploads", summary.full_uploads);
    insert_json_counter(object, "partial_uploads", summary.partial_uploads);
    insert_json_counter(object, "uploaded_bytes", summary.uploaded_bytes);
    object.insert(QStringLiteral("full_upload"), summary.full_upload);
    object.insert(QStringLiteral("partial_upload"), summary.partial_upload);
    object.insert(QStringLiteral("skipped_upload"), summary.skipped_upload);
    object.insert(
        QStringLiteral("rotating_slot_seed_upload"),
        summary.rotating_slot_seed_upload);
    object.insert(
        QStringLiteral("buffer_recreated_upload"),
        summary.buffer_recreated_upload);
    object.insert(
        QStringLiteral("instance_layout_changed_upload"),
        summary.instance_layout_changed_upload);
    object.insert(QStringLiteral("full_repaint_upload"), summary.full_repaint_upload);
    object.insert(QStringLiteral("non_dirty_state_upload"), summary.non_dirty_state_upload);
    object.insert(QStringLiteral("row_stable_layout"), summary.row_stable_layout);
    return object;
}

QJsonObject glyph_coverage_counts_json(
    const internal::Qsg_atlas_frame_build_summary& summary)
{
    QJsonObject object;
    detail::emit_metrics_json(object, summary.glyph_coverage, detail::glyph_coverage_metrics());
    return object;
}

QJsonObject atlas_first_glyph_miss_json(
    const internal::Qsg_atlas_glyph_miss_diagnostic& miss)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), miss.valid);
    object.insert(
        QStringLiteral("cause"),
        QString::fromLatin1(internal::qsg_atlas_glyph_miss_cause_name(miss.cause)));
    object.insert(
        QStringLiteral("coverage_kind"),
        QString::fromLatin1(
            internal::qsg_atlas_glyph_coverage_kind_name(
                miss.image.coverage_kind)));
    object.insert(
        QStringLiteral("presentation"),
        QString::fromLatin1(
            internal::qsg_atlas_glyph_image_presentation_name(
                miss.image.presentation)));
    object.insert(
        QStringLiteral("source_format"),
        static_cast<int>(miss.image.source_format));
    object.insert(QStringLiteral("source_width"), miss.image.source_size.width());
    object.insert(QStringLiteral("source_height"), miss.image.source_size.height());
    insert_json_counter(object, "glyph_index", miss.image.glyph_index);
    object.insert(QStringLiteral("fallback_face_id"), miss.image.fallback_face_id);
    insert_json_counter(object, "text_run_index", miss.image.text_run_index);
    insert_json_counter(object, "glyph_run_index", miss.image.glyph_run_index);
    insert_json_counter(
        object,
        "glyph_index_in_run",
        miss.image.glyph_index_in_run);
    insert_json_counter(
        object,
        "source_string_start",
        miss.image.source_string_start);
    insert_json_counter(object, "source_string_end", miss.image.source_string_end);
    object.insert(QStringLiteral("tile_width"), miss.tile_size.width());
    object.insert(QStringLiteral("tile_height"), miss.tile_size.height());
    insert_json_counter(object, "tile_bytes_per_line", miss.tile_bytes_per_line);
    insert_json_counter(object, "atlas_page_count", miss.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", miss.atlas_page_budget);
    object.insert(QStringLiteral("atlas_page_width"), miss.atlas_page_size.width());
    object.insert(
        QStringLiteral("atlas_page_height"),
        miss.atlas_page_size.height());
    return object;
}

QJsonObject atlas_capabilities_json(const internal::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    detail::emit_metrics_json(object, summary, detail::atlas_capabilities_metrics());
    return object;
}

QJsonObject atlas_render_summary_json(
    const internal::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("rect_buffer"),
        atlas_buffer_summary_json(summary.rect_buffer));
    object.insert(
        QStringLiteral("glyph_buffer"),
        atlas_buffer_summary_json(summary.glyph_buffer));
    insert_json_counter(
        object,
        "shaped_text_runs",
        summary.shaped_text_runs);
    insert_json_counter(
        object,
        "shaped_glyph_records",
        summary.shaped_glyph_records);
    insert_json_counter(
        object,
        "shaped_missing_string_indexes",
        summary.shaped_missing_string_indexes);
    insert_json_counter(
        object,
        "shaped_invalid_string_indexes",
        summary.shaped_invalid_string_indexes);
    insert_json_counter(
        object,
        "glyph_buffer_instances",
        summary.glyph_buffer_instances);
    insert_json_counter(
        object,
        "rect_row_capacity",
        summary.rect_row_capacity);
    insert_json_counter(
        object,
        "glyph_text_row_capacity",
        summary.glyph_text_row_capacity);
    insert_json_counter(
        object,
        "glyph_cursor_text_row_capacity",
        summary.glyph_cursor_text_row_capacity);
    insert_json_counter(
        object,
        "background_rects_before_coalescing",
        summary.background_rects_before_coalescing);
    insert_json_counter(
        object,
        "background_rects_after_coalescing",
        summary.background_rects_after_coalescing);
    insert_json_counter(
        object,
        "background_rects_coalesced",
        summary.background_rects_coalesced);
    insert_json_counter(object, "rect_draw_calls", summary.rect_draw_calls);
    insert_json_counter(object, "glyph_draw_calls", summary.glyph_draw_calls);
    insert_json_counter(
        object,
        "msdf_text_draw_calls",
        summary.msdf_text_draw_calls);
    insert_json_counter(object, "draw_calls", summary.draw_calls);
    object.insert(
        QStringLiteral("text_renderer_policy"),
        QString::fromLatin1(
            internal::qsg_atlas_text_renderer_policy_name(
                summary.text_renderer_policy)));
    object.insert(
        QStringLiteral("effective_text_renderer"),
        QString::fromLatin1(
            internal::qsg_atlas_text_renderer_kind_name(
                summary.effective_text_renderer)));
    object.insert(
        QStringLiteral("text_renderer_fallback_allowed"),
        summary.text_renderer_fallback_allowed);
    object.insert(
        QStringLiteral("text_renderer_fallback_used"),
        summary.text_renderer_fallback_used);
    object.insert(
        QStringLiteral("msdf_lcd_subpixel_order"),
        QString::fromLatin1(
            internal::qsg_atlas_lcd_subpixel_order_name(
                summary.msdf_lcd_subpixel_order)));
    object.insert(
        QStringLiteral("msdf_lcd_text_enabled"),
        summary.msdf_lcd_text_enabled);
    insert_json_counter(object, "atlas_page_count", summary.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", summary.atlas_page_budget);
    insert_json_counter(object, "atlas_page_bytes", summary.atlas_page_bytes);
    insert_json_counter(object, "atlas_allocated_bytes", summary.atlas_allocated_bytes);
    insert_json_counter(object, "atlas_budget_bytes", summary.atlas_budget_bytes);
    insert_json_counter(object, "atlas_used_bytes", summary.atlas_used_bytes);
    insert_json_counter(object, "atlas_failed_inserts", summary.atlas_failed_inserts);
    object.insert(QStringLiteral("atlas_page_pressure"), summary.atlas_page_pressure);
    object.insert(
        QStringLiteral("coverage_texture_uploaded"),
        summary.coverage_texture_uploaded);
    object.insert(
        QStringLiteral("coverage_texture_skipped"),
        summary.coverage_texture_skipped);
    object.insert(
        QStringLiteral("full_dirty_range_reupload"),
        summary.full_dirty_range_reupload);
    object.insert(
        QStringLiteral("public_projection_full_reupload"),
        summary.public_projection_full_reupload);
    object.insert(QStringLiteral("scroll_full_reupload"), summary.scroll_full_reupload);
    object.insert(
        QStringLiteral("non_dirty_selection_invalidation"),
        summary.non_dirty_selection_invalidation);
    object.insert(
        QStringLiteral("non_dirty_cursor_invalidation"),
        summary.non_dirty_cursor_invalidation);
    object.insert(
        QStringLiteral("non_dirty_preedit_invalidation"),
        summary.non_dirty_preedit_invalidation);
    object.insert(
        QStringLiteral("non_dirty_options_invalidation"),
        summary.non_dirty_options_invalidation);
    object.insert(
        QStringLiteral("non_dirty_visual_bell_invalidation"),
        summary.non_dirty_visual_bell_invalidation);
    object.insert(QStringLiteral("font_epoch_invalidation"), summary.font_epoch_invalidation);
    return object;
}

QJsonObject atlas_producer_summary_json(
    const internal::Qsg_atlas_producer_summary& summary)
{
    QJsonObject object;
    detail::emit_metrics_json(object, summary, detail::atlas_producer_metrics());
    return object;
}

QJsonObject atlas_warm_lazy_summary_json(
    const internal::Qsg_atlas_warm_lazy_summary& summary)
{
    QJsonObject object;
    detail::emit_metrics_json(
        object, summary, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    object.insert(QStringLiteral("warm_elapsed_ms"), summary.warm_elapsed_ms);
    detail::emit_metrics_json(
        object, summary, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    object.insert(QStringLiteral("lazy_elapsed_ms"), summary.lazy_elapsed_ms);
    detail::emit_metrics_json(
        object, summary, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    return object;
}

QJsonObject atlas_msdf_text_metrics_json(
    const internal::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("renderer_enabled"),
        summary.msdf_text_renderer_enabled);
    object.insert(
        QStringLiteral("renderer_active"),
        summary.msdf_text_renderer_active);
    object.insert(
        QStringLiteral("atlas_built"), summary.msdf_text_atlas_built);
    object.insert(
        QStringLiteral("atlas_ready"), summary.msdf_text_atlas_ready);
    insert_json_counter(
        object, "draw_pixel_height", summary.msdf_text_pixel_height);
    insert_json_counter(
        object, "baked_pixel_height", summary.msdf_text_baked_pixel_height);
    insert_json_counter(object, "atlas_size", summary.msdf_text_atlas_size);
    object.insert(
        QStringLiteral("px_range"),
        static_cast<double>(summary.msdf_text_px_range));
    insert_json_counter(
        object, "atlas_generation", summary.msdf_text_atlas_generation);
    // Per-frame event flags.
    object.insert(QStringLiteral("cache_hit"), summary.msdf_text_cache_hit);
    object.insert(QStringLiteral("cache_miss"), summary.msdf_text_cache_miss);
    object.insert(
        QStringLiteral("baked_atlas_reused"),
        summary.msdf_text_baked_atlas_reused);
    object.insert(
        QStringLiteral("atlas_build_attempted"),
        summary.msdf_text_atlas_build_attempted);
    object.insert(
        QStringLiteral("atlas_build_succeeded"),
        summary.msdf_text_atlas_build_succeeded);
    object.insert(
        QStringLiteral("texture_uploaded"),
        summary.msdf_text_texture_uploaded);
    // Lifetime-cumulative counters: a zoom gesture needs these so a single
    // build/upload cannot be hidden by a later clean frame before capture.
    insert_json_counter(
        object,
        "atlas_build_attempts_total",
        summary.msdf_text_atlas_build_attempts_total);
    insert_json_counter(
        object,
        "atlas_build_successes_total",
        summary.msdf_text_atlas_build_successes_total);
    insert_json_counter(
        object,
        "atlas_texture_uploads_total",
        summary.msdf_text_atlas_texture_uploads_total);
    insert_json_counter(
        object,
        "baked_cache_hits_total",
        summary.msdf_text_baked_cache_hits_total);
    insert_json_counter(
        object,
        "baked_cache_misses_total",
        summary.msdf_text_baked_cache_misses_total);
    return object;
}

QJsonObject atlas_cursor_report_json(const internal::Qsg_atlas_cursor_report& report)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), report.valid);
    object.insert(QStringLiteral("visible"), report.visible);
    object.insert(QStringLiteral("shape"), static_cast<int>(report.shape));
    object.insert(QStringLiteral("row"), report.row);
    object.insert(QStringLiteral("column"), report.column);
    return object;
}

QJsonObject qsg_atlas_metrics_json(const internal::Qsg_atlas_frame_report& report)
{
    QJsonObject object;
    object.insert(QStringLiteral("renderer"), QStringLiteral("atlas"));
    object.insert(
        QStringLiteral("text_renderer_policy"),
        QString::fromLatin1(
            internal::qsg_atlas_text_renderer_policy_name(
                report.text_renderer_policy)));
    object.insert(
        QStringLiteral("effective_text_renderer"),
        QString::fromLatin1(
            internal::qsg_atlas_text_renderer_kind_name(
                report.effective_text_renderer)));
    object.insert(
        QStringLiteral("text_renderer_fallback_allowed"),
        report.text_renderer_fallback_allowed);
    object.insert(
        QStringLiteral("text_renderer_fallback_used"),
        report.text_renderer_fallback_used);
    object.insert(
        QStringLiteral("msdf_lcd_subpixel_order"),
        QString::fromLatin1(
            internal::qsg_atlas_lcd_subpixel_order_name(
                report.msdf_lcd_subpixel_order)));
    object.insert(
        QStringLiteral("msdf_lcd_text_enabled"),
        report.msdf_lcd_text_enabled);

    detail::emit_metrics_json(object, report, detail::atlas_report_sequence_metrics());
    object.insert(QStringLiteral("command_buffer_non_null"), report.command_buffer_non_null);
    object.insert(QStringLiteral("render_target_non_null"), report.render_target_non_null);
    object.insert(QStringLiteral("rhi_non_null"), report.rhi_non_null);
    object.insert(QStringLiteral("drew"), report.drew);
    object.insert(QStringLiteral("coverage_texture_created"), report.coverage_texture_created);
    object.insert(QStringLiteral("coverage_upload_recorded"), report.coverage_upload_recorded);
    object.insert(QStringLiteral("raw_font_rasterized"), report.raw_font_rasterized);
    object.insert(
        QStringLiteral("captured_snapshot_cursor"),
        atlas_cursor_report_json(report.captured_snapshot_cursor));
    object.insert(
        QStringLiteral("captured_render_cursor"),
        atlas_cursor_report_json(report.captured_render_cursor));
    object.insert(
        QStringLiteral("emitted_cursor"),
        atlas_cursor_report_json(report.frame_build.emitted_cursor));
    detail::emit_metrics_json(object, report, detail::atlas_report_rasterization_metrics());
    insert_json_counter(
        object,
        "max_glyph_instance_page",
        std::max(0, report.frame_build.max_glyph_instance_page));
    insert_json_counter(
        object,
        "snapped_origin_failures",
        report.frame_build.snapped_origin_failures);
    insert_json_counter(
        object,
        "frame_row_descriptors",
        report.frame_build.frame_row_descriptors);
    insert_json_counter(
        object,
        "frame_layer_descriptors",
        report.frame_build.frame_layer_descriptors);
    insert_json_counter(
        object,
        "qsg_layer_descriptors",
        report.frame_build.qsg_layer_descriptors);
    insert_json_counter(
        object,
        "glyph_missed_instances",
        report.frame_build.glyph_missed_instances);
    insert_json_counter(
        object,
        "glyph_coverage_failures",
        report.frame_build.glyph_coverage_failures);
    insert_json_counter(
        object,
        "glyph_atlas_insert_failures",
        report.frame_build.glyph_atlas_insert_failures);
    object.insert(
        QStringLiteral("coverage"),
        glyph_coverage_counts_json(report.frame_build));
    object.insert(
        QStringLiteral("first_glyph_miss"),
        atlas_first_glyph_miss_json(report.frame_build.first_glyph_miss));
    object.insert(
        QStringLiteral("sampler_mode"),
        QString::fromLatin1(
            internal::qsg_atlas_sampler_mode_name(report.render.glyph_sampler_mode)));
    object.insert(
        QStringLiteral("capabilities"),
        atlas_capabilities_json(report.render));
    object.insert(
        QStringLiteral("producer"),
        atlas_producer_summary_json(report.producer));
    object.insert(
        QStringLiteral("warm_lazy"),
        atlas_warm_lazy_summary_json(report.warm_lazy));
    object.insert(QStringLiteral("buffer_upload"), atlas_render_summary_json(report.render));
    object.insert(
        QStringLiteral("msdf_text"),
        atlas_msdf_text_metrics_json(report.render));
    return object;
}

}

void append_atlas_metrics_json(const VNM_TerminalSurface& surface, QJsonObject& out)
{
    const internal::Qsg_atlas_frame_report report =
        internal::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    out = qsg_atlas_metrics_json(report);
}

void append_render_invalidation_metrics_json(
    const VNM_TerminalSurface&  surface,
    QJsonObject&                out)
{
    const internal::Terminal_surface_render_invalidation_stats_t stats =
        internal::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);

    insert_json_counter(out, "update_requests", stats.update_requests);
    insert_json_counter(out, "scheduled_updates", stats.scheduled_updates);
    insert_json_counter(out, "coalesced_requests", stats.coalesced_requests);
    insert_json_counter(out, "consumed_updates", stats.consumed_updates);
    insert_json_counter(
        out,
        "render_snapshot_callback_epoch",
        stats.render_snapshot_callback_epoch);
    insert_json_counter(
        out,
        "last_rendered_snapshot_sequence",
        stats.last_rendered_snapshot_sequence);
    insert_json_counter(
        out,
        "last_rendered_publication_generation",
        stats.last_rendered_publication_generation);
    out.insert(QStringLiteral("pending_update"), stats.pending_update);
}

void append_backend_drain_metrics_json(
    const VNM_TerminalSurface&  surface,
    QJsonObject&                out)
{
    const internal::Terminal_surface_backend_drain_stats_t stats =
        internal::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);

    insert_json_counter(out, "total_drain_calls", stats.total_drain_calls);
    insert_json_counter(out, "budgeted_drain_calls", stats.budgeted_drain_calls);
    insert_json_counter(out, "unbudgeted_drain_calls", stats.unbudgeted_drain_calls);
    insert_json_counter(out, "posted_drain_calls", stats.posted_drain_calls);
    insert_json_counter(out, "posted_full_budget_calls", stats.posted_full_budget_calls);
    insert_json_counter(
        out,
        "posted_frame_pending_small_budget_calls",
        stats.posted_frame_pending_small_budget_calls);
    insert_json_counter(
        out,
        "budget_exhausted_incomplete",
        stats.budget_exhausted_incomplete);
    insert_json_counter(
        out,
        "cursor_stable_incomplete",
        stats.cursor_stable_incomplete);
    insert_json_counter(out, "total_elapsed_ns", stats.total_elapsed_ns);
    insert_json_counter(out, "max_elapsed_ns", stats.max_elapsed_ns);
    insert_json_counter(
        out,
        "session_processing_calls",
        stats.session_processing_calls);
    insert_json_counter(
        out,
        "session_processing_elapsed_ns",
        stats.session_processing_elapsed_ns);
    insert_json_counter(
        out,
        "session_processing_max_elapsed_ns",
        stats.session_processing_max_elapsed_ns);
    insert_json_counter(
        out,
        "sync_from_session_calls",
        stats.sync_from_session_calls);
    insert_json_counter(
        out,
        "sync_from_session_elapsed_ns",
        stats.sync_from_session_elapsed_ns);
    insert_json_counter(
        out,
        "sync_from_session_max_elapsed_ns",
        stats.sync_from_session_max_elapsed_ns);
    insert_json_counter(
        out,
        "frame_work_pending_drain_calls",
        stats.frame_work_pending_drain_calls);
    insert_json_counter(
        out,
        "frame_work_pending_elapsed_ns",
        stats.frame_work_pending_elapsed_ns);
    insert_json_counter(
        out,
        "render_update_pending_drain_calls",
        stats.render_update_pending_drain_calls);
    insert_json_counter(
        out,
        "atlas_completion_pending_drain_calls",
        stats.atlas_completion_pending_drain_calls);
    insert_json_counter(out, "requeue_count", stats.requeue_count);
    insert_json_counter(
        out,
        "pending_callback_after_drain",
        stats.pending_callback_after_drain);
    insert_json_counter(
        out,
        "output_backpressure_after_drain",
        stats.output_backpressure_after_drain);
}

void append_retained_history_metrics_json(
    const VNM_TerminalSurface& surface,
    QJsonObject&               out)
{
    const internal::terminal_retained_history_diagnostics_t diagnostics =
        internal::VNM_TerminalSurface_render_bridge::retained_history_diagnostics(surface);
    detail::emit_metrics_json(
        out,
        diagnostics,
        detail::retained_history_metrics<
            internal::terminal_retained_history_diagnostics_t>());

    QJsonObject estimate;
    detail::emit_metrics_json(
        estimate,
        diagnostics.prefix_plain_ascii_estimate,
        detail::retained_history_estimate_metrics<
            internal::terminal_history_prefix_plain_ascii_retention_estimate_t>());
    out.insert(QStringLiteral("prefix_plain_ascii_estimate"), estimate);
}

}
