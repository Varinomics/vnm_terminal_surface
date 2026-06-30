// Guards docs/diagnostics_schema.md against drifting from the descriptor
// tables in src/diagnostics/: every descriptor-backed field (and the few
// hand-written fields embedded inside the same documented blocks) must appear
// in the doc. The doc path arrives as argv[1] from the CTest definition.
#include "diagnostics/atlas_metric_descriptors.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>

#include <iostream>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;
namespace detail = vnm_terminal::diagnostics::detail;

namespace {

using vnm_terminal::test_helpers::check;

template<typename Stats>
void append_table_keys(
    std::vector<const char*>&                         keys,
    std::span<const detail::Metric_descriptor<Stats>> table)
{
    for (const detail::Metric_descriptor<Stats>& metric : table) {
        keys.push_back(metric.json_key);
    }
}

std::vector<const char*> descriptor_backed_keys()
{
    std::vector<const char*> keys;

    // The text-layout tables are templates; instantiate them with the same
    // stats type the production JSON serializer feeds them
    // (append_renderer_metrics_json passes the cumulative renderer stats).
    using Renderer_stats = term::terminal_renderer_cumulative_stats_t;
    append_table_keys<Renderer_stats>(
        keys, detail::text_layout_metrics_before_optional<Renderer_stats>());
    append_table_keys<Renderer_stats>(
        keys, detail::text_layout_metrics_after_optional<Renderer_stats>());

    append_table_keys(keys, detail::atlas_producer_metrics());
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    append_table_keys(keys, detail::glyph_coverage_metrics());
    append_table_keys(keys, detail::atlas_capabilities_metrics());
    append_table_keys(keys, detail::atlas_report_sequence_metrics());
    append_table_keys(keys, detail::atlas_report_rasterization_metrics());

    // Hand-written fields embedded inside the descriptor-backed blocks: the
    // optional text-layout counter and the one-off non-counter formats the
    // table comments call out. They are documented like their neighbors.
    keys.push_back("text_ascii_replacement_add_glyphs_calls");
    keys.push_back("warm_elapsed_ms");
    keys.push_back("lazy_elapsed_ms");
    keys.push_back("max_glyph_instance_page");

    return keys;
}

std::vector<const char*> hand_written_runtime_keys()
{
    return {
        "last_rendered_publication_generation",
    };
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: schema_sync_tests <path-to-diagnostics_schema.md>\n";
        return 1;
    }

    QFile doc_file(QString::fromLocal8Bit(argv[1]));
    bool ok = check(
        doc_file.open(QIODevice::ReadOnly),
        "diagnostics_schema.md opens for the schema sync check");
    if (!ok) {
        return 1;
    }

    const QByteArray doc = doc_file.readAll();
    ok &= check(!doc.isEmpty(), "diagnostics_schema.md is not empty");

    const std::vector<const char*> keys = descriptor_backed_keys();
    ok &= check(keys.size() > 100U,
        "descriptor tables enumerate the expected field volume");
    for (const char* key : keys) {
        ok &= check(
            doc.contains(key),
            std::string("diagnostics_schema.md documents descriptor field ") + key);
    }
    for (const char* key : hand_written_runtime_keys()) {
        ok &= check(
            doc.contains(key),
            std::string("diagnostics_schema.md documents runtime field ") + key);
    }

    return ok ? 0 : 1;
}
