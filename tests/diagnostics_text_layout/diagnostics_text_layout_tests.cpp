#include "helpers/test_check.h"

#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "diagnostics/atlas_metric_descriptors.h"
#include "diagnostics/metric_descriptor.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdint>
#include <utility>
#include <vector>

// Golden characterization test for the text-layout diagnostics block.
//
// The block is retained as legacy renderer compatibility schema. This test
// feeds a fabricated stats struct -- distinct, non-zero value per field --
// through the shared detail emitters and asserts the output byte-for-byte
// (text) and key/value-string-exact (JSON), against an independently-spelled-out
// oracle. A dropped, renamed, or reordered field, or any change to the number
// formatting, is caught here.
//
// The oracle below is the EXACT field list and order the block emits. It is
// duplicated on purpose: if it drifts from the production table the test fails,
// which is what makes this a characterization test rather than a tautology.

namespace {

using vnm_terminal::test_helpers::check;
namespace detail = vnm_terminal::diagnostics::detail;
namespace term   = vnm_terminal::internal;

// The text-layout block field list, in emit order. The fabricated fixture
// assigns each field a unique value so a reorder/drop is observable.
const std::vector<const char*>& text_layout_field_names()
{
    static const std::vector<const char*> names = {
        "qt_text_layout_calls",
        "text_layout_runs_single_code_unit",
        "text_layout_runs_multi_code_unit",
        "text_layout_runs_all_space",
        "text_layout_runs_printable_ascii",
        "text_layout_runs_printable_ascii_with_space",
        "text_layout_runs_other_ascii",
        "text_layout_runs_non_ascii",
        "text_layout_runs_clipped",
        "text_layout_runs_ascii_layout_font",
        "text_layout_runs_force_blended_order",
        "text_layout_runs_with_hyperlink",
        "text_layout_runs_with_decoration",
        "text_layout_runs_mixed_ascii_non_ascii",
        "text_layout_runs_pure_non_ascii",
        "text_layout_runs_plain_unclipped",
        "text_layout_runs_plain_unclipped_ascii_font",
        "text_layout_runs_all_space_plain_unclipped",
        "text_layout_runs_printable_ascii_plain_unclipped",
        "text_layout_runs_non_ascii_plain_unclipped",
        "text_layout_runs_mixed_ascii_non_ascii_plain_unclipped",
        "text_layout_runs_pure_non_ascii_plain_unclipped",
        "text_layout_runs_fast_space_candidate",
        "text_layout_runs_fast_ascii_candidate",
        "text_layout_runs_fast_ascii_no_space_candidate",
        "text_layout_runs_fast_ascii_single_candidate",
        "text_layout_runs_fast_ascii_multi_candidate",
        "text_ascii_replacement_runs_screened",
        "text_ascii_replacement_runs_eligible",
        "text_ascii_replacement_runs_attempted",
        "text_ascii_replacement_runs_trusted_fast_path",
        "text_ascii_replacement_runs_succeeded",
        "text_ascii_replacement_runs_all_space_succeeded",
        "text_ascii_replacement_add_glyphs_calls",
        "text_ascii_replacement_runs_fallback",
        "text_ascii_replacement_runs_rejected_clipped",
        "text_ascii_replacement_runs_rejected_force_blended_order",
        "text_ascii_replacement_runs_rejected_decoration",
        "text_ascii_replacement_runs_rejected_hyperlink",
        "text_ascii_replacement_runs_rejected_non_printable_ascii",
        "text_ascii_replacement_runs_rejected_non_ascii",
        "text_ascii_replacement_runs_rejected_geometry",
        "text_ascii_replacement_runs_rejected_unsupported_font",
        "text_ascii_replacement_runs_rejected_internal_node",
        "text_ascii_replacement_runs_rejected_glyph_mapping",
        "text_layout_code_units",
        "text_layout_space_code_units",
        "text_layout_printable_ascii_code_units",
        "text_layout_other_ascii_code_units",
        "text_layout_non_ascii_code_units",
        "text_layout_plain_unclipped_code_units",
        "text_layout_all_space_plain_unclipped_code_units",
        "text_layout_printable_ascii_plain_unclipped_code_units",
        "text_layout_non_ascii_plain_unclipped_code_units",
        "text_layout_fast_space_candidate_code_units",
        "text_layout_fast_ascii_candidate_code_units",
        "text_ascii_replacement_code_units_screened",
        "text_ascii_replacement_code_units_eligible",
        "text_ascii_replacement_code_units_attempted",
        "text_ascii_replacement_code_units_trusted_fast_path",
        "text_ascii_replacement_code_units_succeeded",
        "text_ascii_replacement_code_units_fallback",
    };
    return names;
}

// Fabricate distinct non-zero values: field at emit index i gets value
// (base + i). The starting base is large enough that no field shares a value
// with a position index used elsewhere, and small enough to stay in range for
// the int-typed cumulative_stats fields.
constexpr std::uint64_t k_value_base = 1000U;

// Populate exactly the text-layout fields of a cumulative-stats fixture with
// the per-index values. The order here mirrors the struct member order, which
// is identical to the emit order.
term::terminal_renderer_cumulative_stats_t make_fixture()
{
    term::terminal_renderer_cumulative_stats_t stats;
    std::uint64_t v = k_value_base;
    auto next = [&v]() { return v++; };

    stats.qt_text_layout_calls                                       = next();
    stats.text_layout_runs_single_code_unit                          = next();
    stats.text_layout_runs_multi_code_unit                           = next();
    stats.text_layout_runs_all_space                                 = next();
    stats.text_layout_runs_printable_ascii                           = next();
    stats.text_layout_runs_printable_ascii_with_space                = next();
    stats.text_layout_runs_other_ascii                               = next();
    stats.text_layout_runs_non_ascii                                 = next();
    stats.text_layout_runs_clipped                                   = next();
    stats.text_layout_runs_ascii_layout_font                         = next();
    stats.text_layout_runs_force_blended_order                       = next();
    stats.text_layout_runs_with_hyperlink                            = next();
    stats.text_layout_runs_with_decoration                           = next();
    stats.text_layout_runs_mixed_ascii_non_ascii                     = next();
    stats.text_layout_runs_pure_non_ascii                            = next();
    stats.text_layout_runs_plain_unclipped                           = next();
    stats.text_layout_runs_plain_unclipped_ascii_font               = next();
    stats.text_layout_runs_all_space_plain_unclipped                 = next();
    stats.text_layout_runs_printable_ascii_plain_unclipped           = next();
    stats.text_layout_runs_non_ascii_plain_unclipped                 = next();
    stats.text_layout_runs_mixed_ascii_non_ascii_plain_unclipped     = next();
    stats.text_layout_runs_pure_non_ascii_plain_unclipped            = next();
    stats.text_layout_runs_fast_space_candidate                      = next();
    stats.text_layout_runs_fast_ascii_candidate                      = next();
    stats.text_layout_runs_fast_ascii_no_space_candidate             = next();
    stats.text_layout_runs_fast_ascii_single_candidate               = next();
    stats.text_layout_runs_fast_ascii_multi_candidate                = next();
    stats.text_ascii_replacement_runs_screened                       = next();
    stats.text_ascii_replacement_runs_eligible                       = next();
    stats.text_ascii_replacement_runs_attempted                      = next();
    stats.text_ascii_replacement_runs_trusted_fast_path              = next();
    stats.text_ascii_replacement_runs_succeeded                      = next();
    stats.text_ascii_replacement_runs_all_space_succeeded            = next();
    stats.text_ascii_replacement_add_glyphs_calls                    = next();
    stats.text_ascii_replacement_runs_fallback                       = next();
    stats.text_ascii_replacement_runs_rejected_clipped               = next();
    stats.text_ascii_replacement_runs_rejected_force_blended_order   = next();
    stats.text_ascii_replacement_runs_rejected_decoration            = next();
    stats.text_ascii_replacement_runs_rejected_hyperlink             = next();
    stats.text_ascii_replacement_runs_rejected_non_printable_ascii   = next();
    stats.text_ascii_replacement_runs_rejected_non_ascii             = next();
    stats.text_ascii_replacement_runs_rejected_geometry              = next();
    stats.text_ascii_replacement_runs_rejected_unsupported_font      = next();
    stats.text_ascii_replacement_runs_rejected_internal_node         = next();
    stats.text_ascii_replacement_runs_rejected_glyph_mapping         = next();
    stats.text_layout_code_units                                     = next();
    stats.text_layout_space_code_units                               = next();
    stats.text_layout_printable_ascii_code_units                     = next();
    stats.text_layout_other_ascii_code_units                         = next();
    stats.text_layout_non_ascii_code_units                           = next();
    stats.text_layout_plain_unclipped_code_units                     = next();
    stats.text_layout_all_space_plain_unclipped_code_units           = next();
    stats.text_layout_printable_ascii_plain_unclipped_code_units     = next();
    stats.text_layout_non_ascii_plain_unclipped_code_units           = next();
    stats.text_layout_fast_space_candidate_code_units                = next();
    stats.text_layout_fast_ascii_candidate_code_units                = next();
    stats.text_ascii_replacement_code_units_screened                 = next();
    stats.text_ascii_replacement_code_units_eligible                 = next();
    stats.text_ascii_replacement_code_units_attempted                = next();
    stats.text_ascii_replacement_code_units_trusted_fast_path        = next();
    stats.text_ascii_replacement_code_units_succeeded                = next();
    stats.text_ascii_replacement_code_units_fallback                 = next();
    return stats;
}

// Build the expected TEXT exactly as append_profile_counter spells it:
//   "  " << name << '=' << value << '\n'
QString expected_text()
{
    QString out;
    QTextStream stream(&out);
    std::uint64_t v = k_value_base;
    for (const char* name : text_layout_field_names()) {
        stream << "  " << name << '=' << static_cast<qulonglong>(v) << '\n';
        ++v;
    }
    stream.flush();
    return out;
}

bool test_text_golden()
{
    const term::terminal_renderer_cumulative_stats_t stats = make_fixture();

    QString     actual;
    QTextStream stream(&actual);
    detail::append_text_layout_stats_text(stream, stats);
    stream.flush();

    const QString expected = expected_text();
    const bool match = (actual == expected);
    if (!match) {
        const QStringList actual_lines   = actual.split(QLatin1Char('\n'));
        const QStringList expected_lines = expected.split(QLatin1Char('\n'));
        const int count = std::max(actual_lines.size(), expected_lines.size());
        for (int i = 0; i < count; ++i) {
            const QString a = i < actual_lines.size()   ? actual_lines.at(i)   : QString();
            const QString e = i < expected_lines.size() ? expected_lines.at(i) : QString();
            if (a != e) {
                QTextStream(stderr)
                    << "text mismatch at line " << i
                    << ": expected [" << e << "] got [" << a << "]\n";
            }
        }
    }
    return check(match, "text-layout TEXT golden is byte-identical");
}

bool test_json_golden()
{
    const term::terminal_renderer_cumulative_stats_t stats = make_fixture();

    QJsonObject object;
    detail::insert_text_layout_stats_json(object, stats);

    bool ok = true;
    // Exactly the expected keys, no more.
    ok &= check(
        object.size() == static_cast<int>(text_layout_field_names().size()),
        "text-layout JSON emits exactly the expected key count");

    std::uint64_t v = k_value_base;
    for (const char* name : text_layout_field_names()) {
        const QString key = QString::fromLatin1(name);
        const QString want = QString::number(static_cast<qulonglong>(v));
        const bool present = object.contains(key);
        ok &= check(present, "text-layout JSON contains expected key");
        if (present) {
            // JSON counters are emitted as decimal STRINGS.
            const QString got = object.value(key).toString();
            const bool eq = (got == want);
            if (!eq) {
                QTextStream(stderr)
                    << "json mismatch for " << key
                    << ": expected [" << want << "] got [" << got << "]\n";
            }
            ok &= check(eq, "text-layout JSON value matches");
        }
        ++v;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Atlas diagnostics blocks (R2.3 Slice 2).
//
// Each converted atlas block is emitted by the same shared detail emitters as
// the text-layout block, but over a dedicated descriptor table per block. The
// oracle below is independently spelled out: for each block we list the exact
// labels, kinds, and the fabricated per-field value, then assert the TEXT bytes
// and the JSON key/value exactly. A dropped/renamed/reordered field or a
// bool/counter-kind mix-up is caught here.

enum class Oracle_kind
{
    COUNTER,
    BOOL,
};

struct Oracle_field
{
    const char*    label;
    Oracle_kind    kind;
    std::uint64_t  counter_value;  // used when kind == COUNTER
    bool           bool_value;     // used when kind == BOOL
};

Oracle_field oracle_counter(const char* label, std::uint64_t value)
{
    return Oracle_field{label, Oracle_kind::COUNTER, value, false};
}

Oracle_field oracle_bool(const char* label, bool value)
{
    return Oracle_field{label, Oracle_kind::BOOL, 0U, value};
}

// Expected TEXT for a flat field run, exactly as emit_metrics_text spells it:
//   "  " << label << '=' << value << '\n'   (bool -> true/false)
QString expected_block_text(const std::vector<Oracle_field>& fields)
{
    QString out;
    QTextStream stream(&out);
    for (const Oracle_field& field : fields) {
        stream << "  " << field.label << '=';
        if (field.kind == Oracle_kind::COUNTER) {
            stream << static_cast<qulonglong>(field.counter_value);
        }
        else {
            stream << (field.bool_value ? "true" : "false");
        }
        stream << '\n';
    }
    stream.flush();
    return out;
}

bool check_block_text(
    const char*                       block_name,
    const QString&                    actual,
    const std::vector<Oracle_field>&  fields)
{
    const QString expected = expected_block_text(fields);
    const bool match = (actual == expected);
    if (!match) {
        QTextStream(stderr)
            << "atlas TEXT mismatch for block " << block_name
            << ": expected [" << expected << "] got [" << actual << "]\n";
    }
    return check(match, "atlas TEXT block byte-identical");
}

bool check_block_json(
    const char*                       block_name,
    const QJsonObject&                object,
    const std::vector<Oracle_field>&  fields)
{
    bool ok = true;
    ok &= check(
        object.size() == static_cast<int>(fields.size()),
        "atlas JSON emits exactly the expected key count");

    for (const Oracle_field& field : fields) {
        const QString key = QString::fromLatin1(field.label);
        const bool present = object.contains(key);
        ok &= check(present, "atlas JSON contains expected key");
        if (!present) {
            continue;
        }
        const QJsonValue value = object.value(key);
        bool eq = false;
        if (field.kind == Oracle_kind::COUNTER) {
            // JSON counters are decimal STRINGS.
            const QString want = QString::number(static_cast<qulonglong>(field.counter_value));
            eq = value.isString() && value.toString() == want;
        }
        else {
            // JSON bools are native true/false.
            eq = value.isBool() && value.toBool() == field.bool_value;
        }
        if (!eq) {
            QTextStream(stderr)
                << "atlas JSON mismatch for " << block_name << '/' << key << "\n";
        }
        ok &= check(eq, "atlas JSON value matches");
    }
    return ok;
}

// Producer: 21 counters with distinct values.
term::Qsg_atlas_producer_summary make_producer_fixture()
{
    term::Qsg_atlas_producer_summary s;
    int v = 5100;
    s.text_runs_considered        = v++;
    s.text_runs_empty             = v++;
    s.shape_cache_lookups         = v++;
    s.shape_cache_hits            = v++;
    s.shape_cache_misses          = v++;
    s.shape_cache_inserts         = v++;
    s.shape_cache_pruned          = v++;
    s.shape_cache_entries         = v++;
    s.shaped_runs_built           = v++;
    s.shaped_runs_reused          = v++;
    s.shaped_glyph_records_built  = v++;
    s.shaped_glyph_records_reused = v++;
    s.presentation_run_scans      = v++;
    s.presentation_source_scans   = v++;
    s.presentation_fast_text_runs = v++;
    s.presentation_emoji_runs     = v++;
    s.slot_resolutions_built      = v++;
    s.slot_resolutions_reused     = v++;
    s.simple_path_attempts        = v++;
    s.simple_path_used            = v++;
    s.simple_path_fallbacks       = v++;
    return s;
}

std::vector<Oracle_field> producer_oracle()
{
    std::uint64_t v = 5100U;
    return {
        oracle_counter("text_runs_considered",        v++),
        oracle_counter("text_runs_empty",             v++),
        oracle_counter("shape_cache_lookups",         v++),
        oracle_counter("shape_cache_hits",            v++),
        oracle_counter("shape_cache_misses",          v++),
        oracle_counter("shape_cache_inserts",         v++),
        oracle_counter("shape_cache_pruned",          v++),
        oracle_counter("shape_cache_entries",         v++),
        oracle_counter("shaped_runs_built",           v++),
        oracle_counter("shaped_runs_reused",          v++),
        oracle_counter("shaped_glyph_records_built",  v++),
        oracle_counter("shaped_glyph_records_reused", v++),
        oracle_counter("presentation_run_scans",      v++),
        oracle_counter("presentation_source_scans",   v++),
        oracle_counter("presentation_fast_text_runs", v++),
        oracle_counter("presentation_emoji_runs",     v++),
        oracle_counter("slot_resolutions_built",      v++),
        oracle_counter("slot_resolutions_reused",     v++),
        oracle_counter("simple_path_attempts",        v++),
        oracle_counter("simple_path_used",            v++),
        oracle_counter("simple_path_fallbacks",       v++),
    };
}

bool test_producer_golden()
{
    const term::Qsg_atlas_producer_summary fixture = make_producer_fixture();
    const std::vector<Oracle_field> oracle = producer_oracle();

    QString     text;
    QTextStream stream(&text);
    detail::emit_metrics_text(stream, fixture, detail::atlas_producer_metrics());
    stream.flush();

    QJsonObject object;
    detail::emit_metrics_json(object, fixture, detail::atlas_producer_metrics());

    bool ok = true;
    ok &= check_block_text("producer", text, oracle);
    ok &= check_block_json("producer", object, oracle);
    return ok;
}

// Glyph coverage: 7 counters.
term::Glyph_coverage_counts make_coverage_fixture()
{
    term::Glyph_coverage_counts s;
    int v = 5200;
    s.grayscale_masks    = v++;
    s.lcd_rgb_masks      = v++;
    s.lcd_bgr_masks      = v++;
    s.color_images       = v++;
    s.ambiguous_images   = v++;
    s.unsupported_images = v++;
    s.missed_images      = v++;
    return s;
}

std::vector<Oracle_field> coverage_oracle()
{
    std::uint64_t v = 5200U;
    return {
        oracle_counter("grayscale_masks",    v++),
        oracle_counter("lcd_rgb_masks",      v++),
        oracle_counter("lcd_bgr_masks",      v++),
        oracle_counter("color_images",       v++),
        oracle_counter("ambiguous_images",   v++),
        oracle_counter("unsupported_images", v++),
        oracle_counter("missed_images",      v++),
    };
}

bool test_coverage_golden()
{
    const term::Glyph_coverage_counts fixture = make_coverage_fixture();
    const std::vector<Oracle_field> oracle = coverage_oracle();

    QString     text;
    QTextStream stream(&text);
    detail::emit_metrics_text(stream, fixture, detail::glyph_coverage_metrics());
    stream.flush();

    QJsonObject object;
    detail::emit_metrics_json(object, fixture, detail::glyph_coverage_metrics());

    bool ok = true;
    ok &= check_block_text("coverage", text, oracle);
    ok &= check_block_json("coverage", object, oracle);
    return ok;
}

// Capabilities: 4 bools. Distinct true/false pattern so the spelling is pinned.
term::Qsg_atlas_render_summary make_capabilities_fixture()
{
    term::Qsg_atlas_render_summary s;
    s.glyph_shader_package_available             = true;
    s.dual_source_probe_shader_package_available = false;
    s.dual_source_blend_factors_available        = true;
    s.dual_source_blend_factors_runtime_probe    = false;
    return s;
}

std::vector<Oracle_field> capabilities_oracle()
{
    return {
        oracle_bool("glyph_shader_package_available",             true),
        oracle_bool("dual_source_probe_shader_package_available", false),
        oracle_bool("dual_source_blend_factors_available",        true),
        oracle_bool("dual_source_blend_factors_runtime_probe",    false),
    };
}

bool test_capabilities_golden()
{
    const term::Qsg_atlas_render_summary fixture = make_capabilities_fixture();
    const std::vector<Oracle_field> oracle = capabilities_oracle();

    QString     text;
    QTextStream stream(&text);
    detail::emit_metrics_text(stream, fixture, detail::atlas_capabilities_metrics());
    stream.flush();

    QJsonObject object;
    detail::emit_metrics_json(object, fixture, detail::atlas_capabilities_metrics());

    bool ok = true;
    ok &= check_block_text("capabilities", text, oracle);
    ok &= check_block_json("capabilities", object, oracle);
    return ok;
}

// Warm-lazy: three table segments split by the two one-off elapsed-ms doubles.
// Two bools (warm_completed, warm_page_pressure) exercise the BOOL kind.
term::Qsg_atlas_warm_lazy_summary make_warm_lazy_fixture()
{
    term::Qsg_atlas_warm_lazy_summary s;
    int v = 5301;  // warm_completed is a bool and consumes no counter value
    s.warm_completed                          = true;
    s.warm_epoch                              = static_cast<std::uint64_t>(v++);
    s.warm_seed_strings                       = v++;
    s.warm_shaped_glyph_records               = v++;
    s.warm_covered_glyph_records              = v++;
    s.warm_skipped_glyph_records              = v++;
    s.warm_environment_skipped_glyph_records  = v++;
    s.warm_failed_glyph_records               = v++;
    s.warm_missing_string_indexes             = v++;
    s.warm_invalid_string_indexes             = v++;
    s.warm_unsupported_images                 = v++;
    s.warm_cache_hits                         = v++;
    s.warm_insert_attempts                    = v++;
    s.warm_inserts                            = v++;
    s.warm_failed_inserts                     = v++;
    s.warm_page_pressure                      = false;
    s.lazy_insert_attempts                    = v++;
    s.lazy_inserts                            = v++;
    s.lazy_failed_inserts                     = v++;
    s.lazy_max_insert_us                      = v++;
    s.lazy_frames                             = v++;
    s.incomplete_frames                       = v++;
    return s;
}

bool test_warm_lazy_golden()
{
    const term::Qsg_atlas_warm_lazy_summary f = make_warm_lazy_fixture();

    // Each segment is spelled out independently with literal values, so the
    // test does not mirror production's own iteration over the table.
    const std::vector<Oracle_field> seg_before_warm = {
        oracle_bool("warm_completed", true),
        oracle_counter("warm_epoch",                             5301U),
        oracle_counter("warm_seed_strings",                      5302U),
        oracle_counter("warm_shaped_glyph_records",              5303U),
        oracle_counter("warm_covered_glyph_records",             5304U),
        oracle_counter("warm_skipped_glyph_records",             5305U),
        oracle_counter("warm_environment_skipped_glyph_records", 5306U),
        oracle_counter("warm_failed_glyph_records",              5307U),
        oracle_counter("warm_missing_string_indexes",            5308U),
        oracle_counter("warm_invalid_string_indexes",            5309U),
        oracle_counter("warm_unsupported_images",                5310U),
        oracle_counter("warm_cache_hits",                        5311U),
        oracle_counter("warm_insert_attempts",                   5312U),
        oracle_counter("warm_inserts",                           5313U),
        oracle_counter("warm_failed_inserts",                    5314U),
    };
    const std::vector<Oracle_field> seg_before_lazy = {
        oracle_bool("warm_page_pressure", false),
        oracle_counter("lazy_insert_attempts", 5315U),
        oracle_counter("lazy_inserts",         5316U),
        oracle_counter("lazy_failed_inserts",  5317U),
    };
    const std::vector<Oracle_field> seg_after_lazy = {
        oracle_counter("lazy_max_insert_us", 5318U),
        oracle_counter("lazy_frames",        5319U),
        oracle_counter("incomplete_frames",  5320U),
    };

    bool ok = true;

    QString     text;
    QTextStream stream(&text);
    detail::emit_metrics_text(stream, f, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    stream.flush();
    ok &= check_block_text("warm_lazy/before_warm_elapsed", text, seg_before_warm);

    QString     text2;
    QTextStream stream2(&text2);
    detail::emit_metrics_text(stream2, f, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    stream2.flush();
    ok &= check_block_text("warm_lazy/before_lazy_elapsed", text2, seg_before_lazy);

    QString     text3;
    QTextStream stream3(&text3);
    detail::emit_metrics_text(stream3, f, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    stream3.flush();
    ok &= check_block_text("warm_lazy/after_lazy_elapsed", text3, seg_after_lazy);

    QJsonObject object;
    detail::emit_metrics_json(object, f, detail::atlas_warm_lazy_metrics_before_warm_elapsed());
    detail::emit_metrics_json(object, f, detail::atlas_warm_lazy_metrics_before_lazy_elapsed());
    detail::emit_metrics_json(object, f, detail::atlas_warm_lazy_metrics_after_lazy_elapsed());
    std::vector<Oracle_field> all = seg_before_warm;
    all.insert(all.end(), seg_before_lazy.begin(), seg_before_lazy.end());
    all.insert(all.end(), seg_after_lazy.begin(), seg_after_lazy.end());
    ok &= check_block_json("warm_lazy", object, all);
    return ok;
}

// Top-level frame-report overlap: the two shared counter runs.
term::Qsg_atlas_frame_report make_report_fixture()
{
    term::Qsg_atlas_frame_report r;
    r.capture_count              = 5400U;
    r.prepare_count              = 5401U;
    r.prepare_elapsed_ns         = 5402U;
    r.render_count               = 5403U;
    r.render_elapsed_ns          = 5404U;
    r.capture_sequence           = 5405U;
    r.captured_snapshot_sequence = 5406U;
    r.captured_font_epoch        = 5407U;
    r.rasterized_glyphs          = 5408;
    r.atlas_page_count           = 5409;
    return r;
}

bool test_report_overlap_golden()
{
    const term::Qsg_atlas_frame_report f = make_report_fixture();

    const std::vector<Oracle_field> sequence = {
        oracle_counter("capture_count",              5400U),
        oracle_counter("prepare_count",              5401U),
        oracle_counter("prepare_elapsed_ns",         5402U),
        oracle_counter("render_count",               5403U),
        oracle_counter("render_elapsed_ns",          5404U),
        oracle_counter("capture_sequence",           5405U),
        oracle_counter("captured_snapshot_sequence", 5406U),
        oracle_counter("captured_font_epoch",        5407U),
    };
    const std::vector<Oracle_field> rasterization = {
        oracle_counter("rasterized_glyphs", 5408U),
        oracle_counter("atlas_page_count",  5409U),
    };

    bool ok = true;

    QString     seq_text;
    QTextStream seq_stream(&seq_text);
    detail::emit_metrics_text(seq_stream, f, detail::atlas_report_sequence_metrics());
    seq_stream.flush();
    ok &= check_block_text("report/sequence", seq_text, sequence);

    QString     ras_text;
    QTextStream ras_stream(&ras_text);
    detail::emit_metrics_text(ras_stream, f, detail::atlas_report_rasterization_metrics());
    ras_stream.flush();
    ok &= check_block_text("report/rasterization", ras_text, rasterization);

    QJsonObject seq_object;
    detail::emit_metrics_json(seq_object, f, detail::atlas_report_sequence_metrics());
    ok &= check_block_json("report/sequence", seq_object, sequence);

    QJsonObject ras_object;
    detail::emit_metrics_json(ras_object, f, detail::atlas_report_rasterization_metrics());
    ok &= check_block_json("report/rasterization", ras_object, rasterization);
    return ok;
}

bool test_retained_history_golden()
{
    term::terminal_retained_history_diagnostics_t diagnostics;
    diagnostics.byte_budget                          = 1001U;
    diagnostics.retained_rows                        = 1002U;
    diagnostics.retained_record_bytes                = 1003U;
    diagnostics.average_retained_row_bytes           = 12.5;
    diagnostics.payload_kind_generic_compact_rows    = 1004U;
    diagnostics.payload_kind_prefix_plain_ascii_rows = 1005U;
    diagnostics.current_style_count                  = 1006U;
    diagnostics.peak_style_count                     = 1007U;
    diagnostics.style_compaction_count               = 1008U;
    diagnostics.reclaimed_styles                     = 1009U;
    diagnostics.hyperlink_compaction_count           = 1010U;
    diagnostics.reclaimed_hyperlink_ids              = 1011U;
    diagnostics.prefix_plain_ascii_estimate = {
        1U,
        171U,
        327U,
        205225U,
        205000U,
        171U,
    };

    QString     text;
    QTextStream stream(&text);
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
    stream.flush();

    const QString expected = QStringLiteral(
        "  byte_budget=1001\n"
        "  retained_rows=1002\n"
        "  retained_record_bytes=1003\n"
        "  average_retained_row_bytes=12.5\n"
        "  payload_kind_generic_compact_rows=1004\n"
        "  payload_kind_prefix_plain_ascii_rows=1005\n"
        "  current_style_count=1006\n"
        "  peak_style_count=1007\n"
        "  style_compaction_count=1008\n"
        "  reclaimed_styles=1009\n"
        "  hyperlink_compaction_count=1010\n"
        "  reclaimed_hyperlink_ids=1011\n"
        "  prefix_plain_ascii_estimate\n"
        "    contract_version=1\n"
        "    source_width_columns=171\n"
        "    record_bytes=327\n"
        "    retained_rows=205225\n"
        "    target_rows=205000\n"
        "    max_columns_at_target_rows=171\n");

    QJsonObject actual;
    detail::emit_metrics_json(
        actual,
        diagnostics,
        detail::retained_history_metrics<
            term::terminal_retained_history_diagnostics_t>());
    QJsonObject estimate;
    detail::emit_metrics_json(
        estimate,
        diagnostics.prefix_plain_ascii_estimate,
        detail::retained_history_estimate_metrics<
            term::terminal_history_prefix_plain_ascii_retention_estimate_t>());

    bool ok = true;
    ok &= check(text == expected,
        "retained-history descriptor emits exact nested profile-text order");
    ok &= check(
        actual.size() == 12 &&
        actual.value(QStringLiteral("byte_budget")).toString() == QStringLiteral("1001") &&
        actual.value(QStringLiteral("average_retained_row_bytes")).toDouble() == 12.5,
        "retained-history descriptor emits exact JSON values and types");
    ok &= check(
        estimate.size() == 6 &&
        estimate.value(QStringLiteral("contract_version")).toString() == QStringLiteral("1") &&
        estimate.value(QStringLiteral("max_columns_at_target_rows")).toString() ==
            QStringLiteral("171"),
        "retained-history estimate descriptor emits versioned JSON values");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_text_golden();
    ok &= test_json_golden();
    ok &= test_producer_golden();
    ok &= test_coverage_golden();
    ok &= test_capabilities_golden();
    ok &= test_warm_lazy_golden();
    ok &= test_report_overlap_golden();
    ok &= test_retained_history_golden();
    return ok ? 0 : 1;
}
