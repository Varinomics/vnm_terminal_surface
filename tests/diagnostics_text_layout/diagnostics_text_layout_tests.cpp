#include "helpers/test_check.h"

#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "diagnostics/metric_descriptor.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdint>
#include <utility>
#include <vector>

// Golden characterization test for the text-layout diagnostics block.
//
// The block is serialized twice in production: once as JSON
// (metrics_json.cpp) and once as TEXT (profile_text.cpp). Both call the shared
// detail emitters. This test feeds a fabricated stats struct -- distinct,
// non-zero value per field -- through those emitters and asserts the output
// byte-for-byte (text) and key/value-string-exact (JSON), against an
// independently-spelled-out oracle. A dropped, renamed, or reordered field, or
// any change to the number formatting, is caught here.
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

}

int main()
{
    bool ok = true;
    ok &= test_text_golden();
    ok &= test_json_golden();
    return ok ? 0 : 1;
}
