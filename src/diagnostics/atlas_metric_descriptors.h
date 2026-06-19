#pragma once

#include "metric_descriptor.h"

#include "vnm_terminal/internal/qsg_atlas_renderer.h"

#include <QJsonObject>
#include <QTextStream>

#include <cstdint>
#include <span>

// Shared descriptor tables for the atlas diagnostics blocks. Both the JSON
// serializer (metrics_json.cpp) and the TEXT serializer (profile_text.cpp)
// describe the same field knowledge for these blocks; the tables below are the
// one place each field list lives, so the two outputs cannot drift apart
// (Rule 6: cross-cutting serialization is shared, not forked).
//
// Each block keeps its surrounding structure hand-written in the serializers:
// JSON nests a block under its key (e.g. "producer"); TEXT emits the field run
// under a hand-written header line (e.g. "  producer\n"). Only the flat field
// run inside each block is table-driven here. The one-off non-counter fields
// (warm/lazy elapsed-ms doubles, the max_glyph_instance_page std::max
// expression) stay hand-written in the serializers, outside these tables.
//
// This header, like metric_descriptor.h, is deliberately NOT under include/: it
// is an implementation detail of the two diagnostics serializers.
//
// docs/diagnostics_schema.md is kept in sync with the tables below.

namespace vnm_terminal::diagnostics::detail {

namespace internal = vnm_terminal::internal;

// Each block's descriptor table is a namespace-scope `inline constexpr` variable
// (static storage duration), so the spans returned by the accessors below stay
// valid. A `static` local in a `constexpr`/`inline` accessor would be C++23-only;
// hoisting the storage keeps these tables valid under C++20.

// Atlas producer block: 21 plain counters, identical JSON keys and TEXT labels
// in the same order in both serializers.
namespace producer_table {
using Stats = internal::Qsg_atlas_producer_summary;
#define VNM_ATLAS_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

inline constexpr Metric_descriptor<Stats> k_table[] = {
    VNM_ATLAS_COUNTER(text_runs_considered),
    VNM_ATLAS_COUNTER(text_runs_empty),
    VNM_ATLAS_COUNTER(shape_cache_lookups),
    VNM_ATLAS_COUNTER(shape_cache_hits),
    VNM_ATLAS_COUNTER(shape_cache_misses),
    VNM_ATLAS_COUNTER(shape_cache_inserts),
    VNM_ATLAS_COUNTER(shape_cache_pruned),
    VNM_ATLAS_COUNTER(shape_cache_entries),
    VNM_ATLAS_COUNTER(shaped_runs_built),
    VNM_ATLAS_COUNTER(shaped_runs_reused),
    VNM_ATLAS_COUNTER(shaped_glyph_records_built),
    VNM_ATLAS_COUNTER(shaped_glyph_records_reused),
    VNM_ATLAS_COUNTER(presentation_run_scans),
    VNM_ATLAS_COUNTER(presentation_source_scans),
    VNM_ATLAS_COUNTER(presentation_fast_text_runs),
    VNM_ATLAS_COUNTER(presentation_emoji_runs),
    VNM_ATLAS_COUNTER(slot_resolutions_built),
    VNM_ATLAS_COUNTER(slot_resolutions_reused),
    VNM_ATLAS_COUNTER(simple_path_attempts),
    VNM_ATLAS_COUNTER(simple_path_used),
    VNM_ATLAS_COUNTER(simple_path_fallbacks),
};

#undef VNM_ATLAS_COUNTER
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_producer_summary>>
atlas_producer_metrics()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_producer_summary>>(
        producer_table::k_table);
}

// Atlas warm-lazy block. The two elapsed-ms doubles are one-off non-counter
// formats and stay hand-written in the serializers, splitting the field run
// into three table segments. `warm_completed` and `warm_page_pressure` are
// BOOLs (consuming read_bool); every other field is a plain counter.
namespace warm_lazy_table {
using Stats = internal::Qsg_atlas_warm_lazy_summary;
#define VNM_ATLAS_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })
#define VNM_ATLAS_BOOL(field) \
    bool_metric<Stats>(#field, [](const Stats& s) -> bool { return s.field; })

inline constexpr Metric_descriptor<Stats> k_before_warm_elapsed[] = {
    VNM_ATLAS_BOOL(warm_completed),
    VNM_ATLAS_COUNTER(warm_epoch),
    VNM_ATLAS_COUNTER(warm_seed_strings),
    VNM_ATLAS_COUNTER(warm_shaped_glyph_records),
    VNM_ATLAS_COUNTER(warm_covered_glyph_records),
    VNM_ATLAS_COUNTER(warm_skipped_glyph_records),
    VNM_ATLAS_COUNTER(warm_environment_skipped_glyph_records),
    VNM_ATLAS_COUNTER(warm_failed_glyph_records),
    VNM_ATLAS_COUNTER(warm_missing_string_indexes),
    VNM_ATLAS_COUNTER(warm_invalid_string_indexes),
    VNM_ATLAS_COUNTER(warm_unsupported_images),
    VNM_ATLAS_COUNTER(warm_cache_hits),
    VNM_ATLAS_COUNTER(warm_insert_attempts),
    VNM_ATLAS_COUNTER(warm_inserts),
    VNM_ATLAS_COUNTER(warm_failed_inserts),
    // -- one-off field warm_elapsed_ms (double) emitted here --
};

inline constexpr Metric_descriptor<Stats> k_before_lazy_elapsed[] = {
    VNM_ATLAS_BOOL(warm_page_pressure),
    VNM_ATLAS_COUNTER(lazy_insert_attempts),
    VNM_ATLAS_COUNTER(lazy_inserts),
    VNM_ATLAS_COUNTER(lazy_failed_inserts),
    // -- one-off field lazy_elapsed_ms (double) emitted here --
};

inline constexpr Metric_descriptor<Stats> k_after_lazy_elapsed[] = {
    VNM_ATLAS_COUNTER(lazy_max_insert_us),
    VNM_ATLAS_COUNTER(lazy_frames),
    VNM_ATLAS_COUNTER(incomplete_frames),
};

#undef VNM_ATLAS_BOOL
#undef VNM_ATLAS_COUNTER
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>
atlas_warm_lazy_metrics_before_warm_elapsed()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>(
        warm_lazy_table::k_before_warm_elapsed);
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>
atlas_warm_lazy_metrics_before_lazy_elapsed()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>(
        warm_lazy_table::k_before_lazy_elapsed);
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>
atlas_warm_lazy_metrics_after_lazy_elapsed()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_warm_lazy_summary>>(
        warm_lazy_table::k_after_lazy_elapsed);
}

// Glyph coverage block: 7 plain counters.
namespace glyph_coverage_table {
using Stats = internal::Glyph_coverage_counts;
#define VNM_ATLAS_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

inline constexpr Metric_descriptor<Stats> k_table[] = {
    VNM_ATLAS_COUNTER(grayscale_masks),
    VNM_ATLAS_COUNTER(lcd_rgb_masks),
    VNM_ATLAS_COUNTER(lcd_bgr_masks),
    VNM_ATLAS_COUNTER(color_images),
    VNM_ATLAS_COUNTER(ambiguous_images),
    VNM_ATLAS_COUNTER(unsupported_images),
    VNM_ATLAS_COUNTER(missed_images),
};

#undef VNM_ATLAS_COUNTER
}

inline std::span<const Metric_descriptor<internal::Glyph_coverage_counts>>
glyph_coverage_metrics()
{
    return std::span<const Metric_descriptor<internal::Glyph_coverage_counts>>(
        glyph_coverage_table::k_table);
}

// Atlas capabilities block: 4 BOOL flags (consuming read_bool).
namespace capabilities_table {
using Stats = internal::Qsg_atlas_render_summary;
#define VNM_ATLAS_BOOL(field) \
    bool_metric<Stats>(#field, [](const Stats& s) -> bool { return s.field; })

inline constexpr Metric_descriptor<Stats> k_table[] = {
    VNM_ATLAS_BOOL(glyph_shader_package_available),
    VNM_ATLAS_BOOL(dual_source_probe_shader_package_available),
    VNM_ATLAS_BOOL(dual_source_blend_factors_available),
    VNM_ATLAS_BOOL(dual_source_blend_factors_runtime_probe),
};

#undef VNM_ATLAS_BOOL
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_render_summary>>
atlas_capabilities_metrics()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_render_summary>>(
        capabilities_table::k_table);
}

// Atlas top-level frame-report counters that overlap between the JSON top-level
// object and the TEXT top-level section with the same name and a plain field
// read. These two runs are contiguous in both serializers but separated there
// by bespoke fields (enum-string and boolean flags) that are NOT shared; the
// max_glyph_instance_page between them stays hand-written because it is a
// std::max(0, ...) transform rather than a plain field read.
namespace report_table {
using Stats = internal::Qsg_atlas_frame_report;
#define VNM_ATLAS_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

inline constexpr Metric_descriptor<Stats> k_sequence[] = {
    VNM_ATLAS_COUNTER(capture_count),
    VNM_ATLAS_COUNTER(prepare_count),
    VNM_ATLAS_COUNTER(render_count),
    VNM_ATLAS_COUNTER(capture_sequence),
    VNM_ATLAS_COUNTER(captured_snapshot_sequence),
    VNM_ATLAS_COUNTER(captured_font_epoch),
};

inline constexpr Metric_descriptor<Stats> k_rasterization[] = {
    VNM_ATLAS_COUNTER(rasterized_glyphs),
    VNM_ATLAS_COUNTER(atlas_page_count),
};

#undef VNM_ATLAS_COUNTER
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_frame_report>>
atlas_report_sequence_metrics()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_frame_report>>(
        report_table::k_sequence);
}

inline std::span<const Metric_descriptor<internal::Qsg_atlas_frame_report>>
atlas_report_rasterization_metrics()
{
    return std::span<const Metric_descriptor<internal::Qsg_atlas_frame_report>>(
        report_table::k_rasterization);
}

}
