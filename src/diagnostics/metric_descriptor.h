#pragma once

#include "vnm_terminal/internal/qsg_terminal_renderer.h"

#include <QJsonObject>
#include <QString>
#include <QTextStream>

#include <cstdint>
#include <span>

// Internal serializer support shared by metrics_json.cpp (JSON) and
// profile_text.cpp (TEXT). Both serializers describe the same metric knowledge
// for the retained-history and text-layout blocks; the descriptor tables below
// are the one place those field lists live, so the two outputs cannot drift
// apart (Rule 6: cross-cutting serialization is shared, not forked).
//
// This header is deliberately NOT under include/: it is an implementation detail
// of the two diagnostics serializers and must not become public API.
//
// docs/diagnostics_schema.md is kept in sync with the descriptor tables below.

namespace vnm_terminal::diagnostics::detail {

enum class Metric_kind
{
    COUNTER,
    BOOL,
    DOUBLE,
};

// One row of metric knowledge for a given Stats type. Readers not selected by
// the row's kind are null. Readers are plain function pointers: a stateless
// lambda converts to one, which lets each row absorb its own numeric conversion
// without std::function.
template<typename Stats>
struct Metric_descriptor
{
    const char*          json_key;
    Metric_kind          kind;
    std::uint64_t      (*read_u64)(const Stats&);
    bool               (*read_bool)(const Stats&);
    double             (*read_double)(const Stats&);
};

// Build a COUNTER row. The reader casts the field to uint64 so int-typed and
// uint64-typed stats structs share one table.
template<typename Stats>
constexpr Metric_descriptor<Stats> counter_metric(
    const char*        key,
    std::uint64_t    (*reader)(const Stats&))
{
    return Metric_descriptor<Stats>{key, Metric_kind::COUNTER, reader, nullptr, nullptr};
}

// Build a BOOL row. The reader returns the field as a native bool; JSON emits a
// native JSON bool and TEXT emits the literal `true`/`false`.
template<typename Stats>
constexpr Metric_descriptor<Stats> bool_metric(
    const char*        key,
    bool             (*reader)(const Stats&))
{
    return Metric_descriptor<Stats>{key, Metric_kind::BOOL, nullptr, reader, nullptr};
}

template<typename Stats>
constexpr Metric_descriptor<Stats> double_metric(
    const char*        key,
    double           (*reader)(const Stats&))
{
    return Metric_descriptor<Stats>{key, Metric_kind::DOUBLE, nullptr, nullptr, reader};
}

// JSON: COUNTER -> decimal string; BOOL -> native bool; DOUBLE -> native number.
template<typename Stats>
void emit_metrics_json(
    QJsonObject&                                out,
    const Stats&                                stats,
    std::span<const Metric_descriptor<Stats>>   table)
{
    for (const Metric_descriptor<Stats>& metric : table) {
        const QString key = QString::fromLatin1(metric.json_key);
        switch (metric.kind) {
            case Metric_kind::COUNTER:
                out.insert(
                    key,
                    QString::number(static_cast<qulonglong>(metric.read_u64(stats))));
                break;
            case Metric_kind::BOOL:
                out.insert(key, metric.read_bool(stats));
                break;
            case Metric_kind::DOUBLE:
                out.insert(key, metric.read_double(stats));
                break;
        }
    }
}

// TEXT: reproduces the existing metric spelling exactly
//   "  " << label << '=' << value << '\n'
// with the same true/false spelling.
template<typename Stats>
void emit_metrics_text(
    QTextStream&                                out,
    const Stats&                                stats,
    std::span<const Metric_descriptor<Stats>>   table,
    const char*                                 indent = "  ")
{
    for (const Metric_descriptor<Stats>& metric : table) {
        out << indent << metric.json_key << '=';
        switch (metric.kind) {
            case Metric_kind::COUNTER:
                out << static_cast<qulonglong>(metric.read_u64(stats));
                break;
            case Metric_kind::BOOL:
                out << (metric.read_bool(stats) ? "true" : "false");
                break;
            case Metric_kind::DOUBLE:
                out << metric.read_double(stats);
                break;
        }
        out << '\n';
    }
}

#define VNM_RETAINED_COUNTER(field)                                    \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

#define VNM_RETAINED_DOUBLE(field) \
    double_metric<Stats>(#field, [](const Stats& s) -> double { return s.field; })

template<typename Stats>
inline constexpr Metric_descriptor<Stats> k_retained_history_metrics[] = {
    VNM_RETAINED_COUNTER(byte_budget),
    VNM_RETAINED_COUNTER(retained_rows),
    VNM_RETAINED_COUNTER(retained_record_bytes),
    VNM_RETAINED_DOUBLE(average_retained_row_bytes),
    VNM_RETAINED_COUNTER(payload_kind_generic_compact_rows),
    VNM_RETAINED_COUNTER(payload_kind_prefix_plain_ascii_rows),
    VNM_RETAINED_COUNTER(current_style_count),
    VNM_RETAINED_COUNTER(peak_style_count),
    VNM_RETAINED_COUNTER(style_compaction_count),
    VNM_RETAINED_COUNTER(reclaimed_styles),
    VNM_RETAINED_COUNTER(hyperlink_compaction_count),
    VNM_RETAINED_COUNTER(reclaimed_hyperlink_ids),
};

template<typename Stats>
inline constexpr Metric_descriptor<Stats> k_retained_history_estimate_metrics[] = {
    VNM_RETAINED_COUNTER(contract_version),
    VNM_RETAINED_COUNTER(source_width_columns),
    VNM_RETAINED_COUNTER(record_bytes),
    VNM_RETAINED_COUNTER(retained_rows),
    VNM_RETAINED_COUNTER(target_rows),
    VNM_RETAINED_COUNTER(max_columns_at_target_rows),
};

#undef VNM_RETAINED_DOUBLE
#undef VNM_RETAINED_COUNTER

template<typename Stats>
constexpr std::span<const Metric_descriptor<Stats>> retained_history_metrics()
{
    return std::span<const Metric_descriptor<Stats>>(k_retained_history_metrics<Stats>);
}

template<typename Stats>
constexpr std::span<const Metric_descriptor<Stats>> retained_history_estimate_metrics()
{
    return std::span<const Metric_descriptor<Stats>>(k_retained_history_estimate_metrics<Stats>);
}

// The text-layout block field list: ONE source of truth for both serializers.
//
// The block is emitted in two contiguous spans. The single optional field
// `text_ascii_replacement_add_glyphs_calls` sits between them and stays an
// explicit `if constexpr (requires {...})` guard in the emitters below, because
// a Stats type that lacks the field must still serialize the rest of the block.
// Splitting at that point keeps the field list a single ordered array while
// preserving the original per-field optionality and emit order exactly.
//
// The two table segments are namespace-scope `inline constexpr` variable
// templates (static storage duration), so the spans returned by the accessors
// below stay valid. A `static` local in a `constexpr` function is C++23-only;
// hoisting the storage keeps the field list valid under C++20.
//
// `VNM_TL_COUNTER` builds a COUNTER row whose reader casts the named field to
// uint64.
#define VNM_TL_COUNTER(field) \
    counter_metric<Stats>(#field, [](const Stats& s) -> std::uint64_t { \
        return static_cast<std::uint64_t>(s.field); })

template<typename Stats>
inline constexpr Metric_descriptor<Stats> k_text_layout_before_optional[] = {
    VNM_TL_COUNTER(qt_text_layout_calls),
    VNM_TL_COUNTER(text_layout_runs_single_code_unit),
    VNM_TL_COUNTER(text_layout_runs_multi_code_unit),
    VNM_TL_COUNTER(text_layout_runs_all_space),
    VNM_TL_COUNTER(text_layout_runs_printable_ascii),
    VNM_TL_COUNTER(text_layout_runs_printable_ascii_with_space),
    VNM_TL_COUNTER(text_layout_runs_other_ascii),
    VNM_TL_COUNTER(text_layout_runs_non_ascii),
    VNM_TL_COUNTER(text_layout_runs_clipped),
    VNM_TL_COUNTER(text_layout_runs_ascii_layout_font),
    VNM_TL_COUNTER(text_layout_runs_force_blended_order),
    VNM_TL_COUNTER(text_layout_runs_with_hyperlink),
    VNM_TL_COUNTER(text_layout_runs_with_decoration),
    VNM_TL_COUNTER(text_layout_runs_mixed_ascii_non_ascii),
    VNM_TL_COUNTER(text_layout_runs_pure_non_ascii),
    VNM_TL_COUNTER(text_layout_runs_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_plain_unclipped_ascii_font),
    VNM_TL_COUNTER(text_layout_runs_all_space_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_printable_ascii_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_non_ascii_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_mixed_ascii_non_ascii_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_pure_non_ascii_plain_unclipped),
    VNM_TL_COUNTER(text_layout_runs_fast_space_candidate),
    VNM_TL_COUNTER(text_layout_runs_fast_ascii_candidate),
    VNM_TL_COUNTER(text_layout_runs_fast_ascii_no_space_candidate),
    VNM_TL_COUNTER(text_layout_runs_fast_ascii_single_candidate),
    VNM_TL_COUNTER(text_layout_runs_fast_ascii_multi_candidate),
    VNM_TL_COUNTER(text_ascii_replacement_runs_screened),
    VNM_TL_COUNTER(text_ascii_replacement_runs_eligible),
    VNM_TL_COUNTER(text_ascii_replacement_runs_attempted),
    VNM_TL_COUNTER(text_ascii_replacement_runs_trusted_fast_path),
    VNM_TL_COUNTER(text_ascii_replacement_runs_succeeded),
    VNM_TL_COUNTER(text_ascii_replacement_runs_all_space_succeeded),
    // -- optional field text_ascii_replacement_add_glyphs_calls here --
};

template<typename Stats>
inline constexpr Metric_descriptor<Stats> k_text_layout_after_optional[] = {
    VNM_TL_COUNTER(text_ascii_replacement_runs_fallback),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_clipped),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_force_blended_order),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_decoration),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_hyperlink),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_non_printable_ascii),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_non_ascii),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_geometry),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_unsupported_font),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_internal_node),
    VNM_TL_COUNTER(text_ascii_replacement_runs_rejected_glyph_mapping),
    VNM_TL_COUNTER(text_layout_code_units),
    VNM_TL_COUNTER(text_layout_space_code_units),
    VNM_TL_COUNTER(text_layout_printable_ascii_code_units),
    VNM_TL_COUNTER(text_layout_other_ascii_code_units),
    VNM_TL_COUNTER(text_layout_non_ascii_code_units),
    VNM_TL_COUNTER(text_layout_plain_unclipped_code_units),
    VNM_TL_COUNTER(text_layout_all_space_plain_unclipped_code_units),
    VNM_TL_COUNTER(text_layout_printable_ascii_plain_unclipped_code_units),
    VNM_TL_COUNTER(text_layout_non_ascii_plain_unclipped_code_units),
    VNM_TL_COUNTER(text_layout_fast_space_candidate_code_units),
    VNM_TL_COUNTER(text_layout_fast_ascii_candidate_code_units),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_screened),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_eligible),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_attempted),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_trusted_fast_path),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_succeeded),
    VNM_TL_COUNTER(text_ascii_replacement_code_units_fallback),
};

#undef VNM_TL_COUNTER

template<typename Stats>
constexpr std::span<const Metric_descriptor<Stats>> text_layout_metrics_before_optional()
{
    return std::span<const Metric_descriptor<Stats>>(k_text_layout_before_optional<Stats>);
}

template<typename Stats>
constexpr std::span<const Metric_descriptor<Stats>> text_layout_metrics_after_optional()
{
    return std::span<const Metric_descriptor<Stats>>(k_text_layout_after_optional<Stats>);
}

// Emit the JSON text-layout block: the shared table, then the one optional
// field by hand (exactly as before), then the rest of the shared table.
template<typename Stats>
void insert_text_layout_stats_json(
    QJsonObject&  object,
    const Stats&  stats)
{
    emit_metrics_json<Stats>(object, stats, text_layout_metrics_before_optional<Stats>());
    if constexpr (requires { stats.text_ascii_replacement_add_glyphs_calls; }) {
        object.insert(
            QStringLiteral("text_ascii_replacement_add_glyphs_calls"),
            QString::number(
                static_cast<qulonglong>(stats.text_ascii_replacement_add_glyphs_calls)));
    }
    emit_metrics_json<Stats>(object, stats, text_layout_metrics_after_optional<Stats>());
}

// Emit the TEXT text-layout block, mirroring the JSON ordering.
template<typename Stats>
void append_text_layout_stats_text(
    QTextStream&  stream,
    const Stats&  stats)
{
    emit_metrics_text<Stats>(stream, stats, text_layout_metrics_before_optional<Stats>());
    if constexpr (requires { stats.text_ascii_replacement_add_glyphs_calls; }) {
        stream
            << "  text_ascii_replacement_add_glyphs_calls="
            << static_cast<qulonglong>(stats.text_ascii_replacement_add_glyphs_calls)
            << '\n';
    }
    emit_metrics_text<Stats>(stream, stats, text_layout_metrics_after_optional<Stats>());
}

}
