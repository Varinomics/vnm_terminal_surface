#pragma once

#include <QJsonObject>
#include <QString>
#include <QTextStream>

#include <cstdint>
#include <span>

// Internal serializer support shared by metrics_json.cpp (JSON) and
// profile_text.cpp (TEXT). Both serializers describe the same metric knowledge
// for the retained-history block; the descriptor tables below are the one
// place its field lists live, so the two outputs cannot drift apart (Rule 6:
// cross-cutting serialization is shared, not forked).
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

}
