// Guards docs/diagnostics_schema.md against drifting from the descriptor
// tables and runtime JSON helpers in src/diagnostics/: each documented table
// section must match the expected field sequence exactly. The doc path arrives
// as argv[1] from the CTest definition.
#include "diagnostics/atlas_metric_descriptors.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>

#include <algorithm>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace term = vnm_terminal::internal;
namespace detail = vnm_terminal::diagnostics::detail;

namespace {

using vnm_terminal::test_helpers::check;

template<typename Stats>
void append_table_keys(
    std::vector<std::string>&                         keys,
    std::span<const detail::Metric_descriptor<Stats>> table)
{
    for (const detail::Metric_descriptor<Stats>& metric : table) {
        keys.push_back(metric.json_key);
    }
}

void append_key(std::vector<std::string>& keys, const char* key)
{
    keys.push_back(key);
}

std::vector<std::string> text_layout_keys()
{
    std::vector<std::string> keys;

    // The legacy text-layout descriptor tables are templates; instantiate them
    // with the cumulative renderer stats type they were originally serialized
    // from so schema coverage continues to validate the compatibility block.
    using Renderer_stats = term::terminal_renderer_cumulative_stats_t;
    append_table_keys<Renderer_stats>(
        keys, detail::text_layout_metrics_before_optional<Renderer_stats>());
    append_key(keys, "text_ascii_replacement_add_glyphs_calls");
    append_table_keys<Renderer_stats>(
        keys, detail::text_layout_metrics_after_optional<Renderer_stats>());

    return keys;
}

std::vector<std::string> atlas_producer_keys()
{
    std::vector<std::string> keys;
    append_table_keys(keys, detail::atlas_producer_metrics());
    return keys;
}

std::vector<std::string> atlas_warm_lazy_keys()
{
    std::vector<std::string> keys;
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    append_key(keys, "warm_elapsed_ms");
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    append_key(keys, "lazy_elapsed_ms");
    append_table_keys(keys, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    return keys;
}

std::vector<std::string> glyph_coverage_keys()
{
    std::vector<std::string> keys;
    append_table_keys(keys, detail::glyph_coverage_metrics());
    return keys;
}

std::vector<std::string> atlas_capabilities_keys()
{
    std::vector<std::string> keys;
    append_table_keys(keys, detail::atlas_capabilities_metrics());
    return keys;
}

std::vector<std::string> atlas_top_level_overlap_keys()
{
    std::vector<std::string> keys;
    append_table_keys(keys, detail::atlas_report_sequence_metrics());
    append_table_keys(keys, detail::atlas_report_rasterization_metrics());
    return keys;
}

std::vector<std::string> renderer_compatibility_keys()
{
    return {
        "compatibility_scope",
        "canonical_renderer_metrics",
        "frames_published",
        "paint_completed_frames",
        "qsg_atlas_render_count",
    };
}

std::vector<std::string> render_invalidation_keys()
{
    return {
        "update_requests",
        "scheduled_updates",
        "coalesced_requests",
        "consumed_updates",
        "render_snapshot_callback_epoch",
        "last_rendered_snapshot_sequence",
        "last_rendered_publication_generation",
        "pending_update",
    };
}

std::vector<std::string> backend_drain_keys()
{
    return {
        "total_drain_calls",
        "budgeted_drain_calls",
        "unbudgeted_drain_calls",
        "posted_drain_calls",
        "posted_full_budget_calls",
        "posted_frame_pending_small_budget_calls",
        "budget_exhausted_incomplete",
        "cursor_stable_incomplete",
        "total_elapsed_ns",
        "max_elapsed_ns",
        "session_processing_calls",
        "session_processing_elapsed_ns",
        "session_processing_max_elapsed_ns",
        "sync_from_session_calls",
        "sync_from_session_elapsed_ns",
        "sync_from_session_max_elapsed_ns",
        "frame_work_pending_drain_calls",
        "frame_work_pending_elapsed_ns",
        "render_update_pending_drain_calls",
        "atlas_completion_pending_drain_calls",
        "requeue_count",
        "pending_callback_after_drain",
        "output_backpressure_after_drain",
    };
}

int markdown_heading_level(std::string_view line)
{
    int level = 0;
    while (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == '#') {
        ++level;
    }

    return (level > 0 && level < static_cast<int>(line.size()) &&
        line[static_cast<std::size_t>(level)] == ' ')
        ? level
        : 0;
}

std::string_view without_trailing_cr(std::string_view line)
{
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

bool extract_markdown_section(
    std::string_view  document,
    int               heading_level,
    std::string_view  heading_title,
    std::string&      section)
{
    std::size_t section_start = std::string::npos;
    std::size_t section_end   = document.size();
    std::size_t pos           = 0;

    while (pos <= document.size()) {
        std::size_t line_end = document.find('\n', pos);
        if (line_end == std::string_view::npos) {
            line_end = document.size();
        }

        const std::string_view line = without_trailing_cr(
            document.substr(pos, line_end - pos));
        const int level = markdown_heading_level(line);
        const std::size_t title_start =
            level > 0 ? static_cast<std::size_t>(level + 1) : std::string::npos;

        if (section_start == std::string::npos) {
            if (level == heading_level &&
                title_start <= line.size() &&
                line.substr(title_start) == heading_title) {
                section_start = (line_end == document.size()) ? document.size() : line_end + 1U;
            }
        }
        else if (level > 0 && level <= heading_level) {
            section_end = pos;
            break;
        }

        if (line_end == document.size()) {
            break;
        }
        pos = line_end + 1U;
    }

    if (section_start == std::string::npos) {
        return false;
    }

    section.assign(
        document.substr(section_start, section_end - section_start));
    return true;
}

std::vector<std::string> documented_field_keys(std::string_view section)
{
    std::vector<std::string> keys;
    std::size_t pos = 0;

    while (pos <= section.size()) {
        std::size_t line_end = section.find('\n', pos);
        if (line_end == std::string_view::npos) {
            line_end = section.size();
        }

        const std::string_view line = without_trailing_cr(
            section.substr(pos, line_end - pos));
        if (!line.empty() && line.front() == '|') {
            const std::size_t first_tick = line.find('`');
            if (first_tick != std::string_view::npos) {
                const std::size_t second_tick = line.find('`', first_tick + 1U);
                if (second_tick != std::string_view::npos) {
                    keys.emplace_back(line.substr(
                        first_tick + 1U,
                        second_tick - first_tick - 1U));
                }
            }
        }

        if (line_end == section.size()) {
            break;
        }
        pos = line_end + 1U;
    }

    return keys;
}

void print_field_sequence_mismatch(
    std::string_view                  section_name,
    const std::vector<std::string>&   expected,
    const std::vector<std::string>&   actual)
{
    std::cerr
        << "diagnostics_schema.md section \"" << section_name
        << "\" expected " << expected.size()
        << " fields but documented " << actual.size() << " fields\n";

    const std::set<std::string> expected_set(expected.begin(), expected.end());
    const std::set<std::string> actual_set(actual.begin(), actual.end());

    for (const std::string& key : expected) {
        if (!actual_set.contains(key)) {
            std::cerr << "  missing documented field: " << key << '\n';
        }
    }
    for (const std::string& key : actual) {
        if (!expected_set.contains(key)) {
            std::cerr << "  stale documented field: " << key << '\n';
        }
    }

    const std::size_t common_size = std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < common_size; ++index) {
        if (expected[index] != actual[index]) {
            std::cerr
                << "  first order mismatch at field " << index
                << ": expected " << expected[index]
                << ", documented " << actual[index] << '\n';
            return;
        }
    }
}

bool check_documented_section(
    std::string_view                  document,
    int                               heading_level,
    std::string_view                  heading_title,
    const std::vector<std::string>&   expected_keys)
{
    bool ok = true;

    std::string section;
    ok &= check(
        extract_markdown_section(document, heading_level, heading_title, section),
        std::string("diagnostics_schema.md contains section ") +
            std::string(heading_title));
    if (section.empty()) {
        return false;
    }

    const std::vector<std::string> actual_keys = documented_field_keys(section);
    if (actual_keys != expected_keys) {
        print_field_sequence_mismatch(heading_title, expected_keys, actual_keys);
        ok &= check(
            false,
            std::string("diagnostics_schema.md field list matches section ") +
                std::string(heading_title));
    }

    return ok;
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

    const QByteArray doc_bytes = doc_file.readAll();
    ok &= check(!doc_bytes.isEmpty(), "diagnostics_schema.md is not empty");

    const std::string document(doc_bytes.constData(), doc_bytes.size());

    const std::vector<std::string> text_keys = text_layout_keys();
    ok &= check(text_keys.size() > 50U,
        "text-layout tables enumerate the expected field volume");

    ok &= check_documented_section(
        document,
        3,
        "Renderer compatibility block (JSON key `renderer`)",
        renderer_compatibility_keys());
    ok &= check_documented_section(
        document,
        3,
        "Render invalidation block (JSON key `render_invalidation`)",
        render_invalidation_keys());
    ok &= check_documented_section(
        document,
        3,
        "Backend drain block (JSON key `backend_drain`)",
        backend_drain_keys());
    ok &= check_documented_section(
        document,
        2,
        "Text-layout block",
        text_keys);
    ok &= check_documented_section(
        document,
        3,
        "Atlas producer block (JSON key `producer`, TEXT header `producer`)",
        atlas_producer_keys());
    ok &= check_documented_section(
        document,
        3,
        "Atlas warm-lazy block (JSON key `warm_lazy`, TEXT header `warm_lazy`)",
        atlas_warm_lazy_keys());
    ok &= check_documented_section(
        document,
        3,
        "Glyph coverage block (JSON key `coverage`, TEXT header `coverage`)",
        glyph_coverage_keys());
    ok &= check_documented_section(
        document,
        3,
        "Atlas capabilities block (JSON key `capabilities`, TEXT header `capabilities`)",
        atlas_capabilities_keys());
    ok &= check_documented_section(
        document,
        3,
        "Atlas top-level overlap (`qsg_atlas` TEXT section / top-level JSON object)",
        atlas_top_level_overlap_keys());

    return ok ? 0 : 1;
}
