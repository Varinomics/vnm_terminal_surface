#pragma once

#include "metric_descriptor.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <cstdint>
#include <span>

namespace vnm_terminal::diagnostics::detail {

namespace model_profile_counter_table {
using Stats = vnm_terminal::internal::Terminal_screen_model_profile_stats;
#define VNM_MODEL_PROFILE_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

inline constexpr Metric_descriptor<Stats> k_counters[] = {
    VNM_MODEL_PROFILE_COUNTER(print_text_calls),
    VNM_MODEL_PROFILE_COUNTER(printable_ascii_span_calls),
    VNM_MODEL_PROFILE_COUNTER(printable_ascii_span_characters),
    VNM_MODEL_PROFILE_COUNTER(printable_ascii_cells_written),
    VNM_MODEL_PROFILE_COUNTER(max_printable_ascii_span_characters),
    VNM_MODEL_PROFILE_COUNTER(printable_ascii_local_cells_inspected),
    VNM_MODEL_PROFILE_COUNTER(scalar_span_local_cells_inspected),
    VNM_MODEL_PROFILE_COUNTER(row_content_generation_comparisons),
    VNM_MODEL_PROFILE_COUNTER(row_content_generation_comparison_cells),
    VNM_MODEL_PROFILE_COUNTER(row_content_generation_advances),
    VNM_MODEL_PROFILE_COUNTER(wide_boundary_repairs_from_text_writes),
    VNM_MODEL_PROFILE_COUNTER(dirty_marks_from_text_writes),
    VNM_MODEL_PROFILE_COUNTER(line_wraps_from_text_writes),
    VNM_MODEL_PROFILE_COUNTER(scrollback_appends_from_text_writes),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_requests),
    VNM_MODEL_PROFILE_COUNTER(render_snapshots_constructed),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_rows_visited),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_rows_materialized),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_rows_borrowed),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_rows_owned),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_rows_built_from_model_storage),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_model_row_accessor_borrows),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_cells_scanned),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_cells_emitted),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_compact_empty_text_cells),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_compact_ascii_text_cells),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_inline_single_bmp_text_cells),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_qstring_copies),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_text_code_units_copied),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_printable_ascii_copies),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_other_ascii_copies),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_single_non_ascii_copies),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_fallback_multi_text_copies),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_unoccupied_cells_skipped),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_dirty_rows_requested),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_dirty_rows_visible),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_full_repaint_fallbacks),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_viewport_fallbacks),
    VNM_MODEL_PROFILE_COUNTER(render_snapshot_zero_dirty_publications),
    VNM_MODEL_PROFILE_COUNTER(max_render_snapshot_rows_visited),
    VNM_MODEL_PROFILE_COUNTER(max_render_snapshot_cells_emitted),
    VNM_MODEL_PROFILE_COUNTER(max_render_snapshot_fallback_text_units_per_cell),
};

#undef VNM_MODEL_PROFILE_COUNTER
}

inline std::span<const Metric_descriptor<model_profile_counter_table::Stats>>
model_profile_counter_metrics()
{
    return model_profile_counter_table::k_counters;
}

}