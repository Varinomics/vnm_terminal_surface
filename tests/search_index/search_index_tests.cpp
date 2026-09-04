#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

// The published render snapshot is the oracle for search text. This is the
// transcription of searchable_projection_row, applied to the cells a snapshot
// actually emitted for one visible row.
term::Terminal_search_row_text expected_row_text_from_snapshot(
    const term::Terminal_render_snapshot& snapshot,
    int                                   snapshot_row)
{
    term::Terminal_search_row_text expected;
    int                            next_column = 0;
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row != snapshot_row ||
            cell.wide_continuation ||
            cell.text.is_empty())
        {
            continue;
        }

        while (next_column < cell.position.column) {
            expected.append_padding_column(next_column);
            ++next_column;
        }

        const int end_column = cell.position.column + std::max(1, cell.display_width);
        const QString cell_text = cell.text.to_qstring();
        expected.append_cell_text(cell_text, cell.position.column, end_column);
        next_column = std::max(next_column, end_column);
    }

    return expected;
}

QString describe_row_text(const term::Terminal_search_row_text& text)
{
    QString description = QStringLiteral("\"%1\" spans[").arg(text.view().toString());
    for (const term::terminal_search_column_span_t& span : text.spans) {
        description += QStringLiteral("%1:%2 ").arg(span.first_column).arg(span.end_column);
    }
    return description + QLatin1Char(']');
}

bool check_row_parity(
    const term::Terminal_search_row_text& expected,
    const term::Terminal_search_row_text& actual,
    const char*                           label,
    int                                   logical_row)
{
    if (expected.units == actual.units && expected.spans == actual.spans) {
        return true;
    }

    std::cerr << "FAIL: " << label << " logical row " << logical_row
        << " expected " << describe_row_text(expected).toUtf8().constData()
        << " actual "   << describe_row_text(actual).toUtf8().constData() << '\n';
    return false;
}

bool test_identity_spans_stay_implicit_until_needed()
{
    term::Terminal_search_row_text text;
    text.append_cell_text(QStringView(u"a"), 0, 1);
    text.append_cell_text(QStringView(u"b"), 1, 2);
    text.append_cell_text(QStringView(u"c"), 2, 3);
    bool ok = true;
    ok &= check(
        text.identity_spans && text.spans.empty() && text.view() == QStringView(u"abc"),
        "ordinary single-width search text keeps identity spans implicit");

    text.append_cell_text(QStringView(u"xy"), 3, 5);
    ok &= check(
        !text.identity_spans && text.spans.size() == text.units.size() &&
            text.spans[0] == term::terminal_search_column_span_t{0, 1} &&
            text.spans[2] == term::terminal_search_column_span_t{2, 3} &&
            text.spans[3] == term::terminal_search_column_span_t{3, 5} &&
            text.spans[4] == term::terminal_search_column_span_t{3, 5},
        "the first non-identity cell backfills exact spans for preceding text");
    return ok;
}

// Walks every viewport position of the primary buffer, so every retained row is
// compared against the snapshot that publishes it.
bool check_primary_parity_over_history(
    const term::Terminal_screen_model& model,
    const char*                        label)
{
    bool ok = true;
    const term::Terminal_render_snapshot tail = model.render_snapshot(1U);
    const int scrollback_rows = tail.viewport.scrollback_rows;
    const int visible_rows    = tail.viewport.visible_rows;
    term::Terminal_search_row_text actual;

    for (int offset = 0; offset <= scrollback_rows; ++offset) {
        term::Terminal_render_snapshot_request request;
        request.sequence          = static_cast<std::uint64_t>(offset) + 2U;
        request.viewport          = tail.viewport;
        request.viewport.offset_from_tail = offset;
        request.viewport_changed  = true;
        const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);

        for (int row = 0; row < visible_rows; ++row) {
            const int logical_row = scrollback_rows - offset + row;
            ok &= check(
                model.search_row_text(term::Terminal_buffer_id::PRIMARY, logical_row, actual),
                label);
            ok &= check_row_parity(
                expected_row_text_from_snapshot(snapshot, row),
                actual,
                label,
                logical_row);
            ok &= check(
                !actual.identity_spans ||
                    std::all_of(
                        actual.spans.begin(),
                        actual.spans.end(),
                        [&](const term::terminal_search_column_span_t& span) {
                            const std::int32_t index = static_cast<std::int32_t>(
                                &span - actual.spans.data());
                            return span.first_column == index && span.end_column == index + 1;
                        }),
                "identity_spans agrees with the recorded spans");
        }
    }

    ok &= check(
        !model.search_row_text(
            term::Terminal_buffer_id::PRIMARY,
            scrollback_rows + visible_rows,
            actual),
        "search_row_text rejects a logical row past the backing range");
    return ok;
}

term::Terminal_screen_model make_model(int rows, int columns, int scrollback_limit)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = term::terminal_grid_size_t{rows, columns};
    config.scrollback_limit = scrollback_limit;
    return term::Terminal_screen_model(config);
}

// One row per emitted line, chosen so the retained history covers every text
// shape the snapshot builder can emit.
QByteArray parity_fixture_bytes()
{
    QByteArray bytes;
    bytes += QByteArrayLiteral("plain ascii\r\n");
    bytes += QByteArrayLiteral("0123456789abcdef\r\n");
    bytes += QByteArrayLiteral("ab\x1b[10Gxy\r\n");
    bytes += QByteArrayLiteral("tail\r\n");
    bytes += QString::fromUtf8("\xE6\xBC\xA2\xE5\xAD\x97 ok").toUtf8() + QByteArrayLiteral("\r\n");
    bytes += QByteArrayLiteral("\x1b[15G") +
        QString::fromUtf8("\xE6\xBC\xA2").toUtf8() + QByteArrayLiteral("\r\n");
    bytes += QString::fromUtf8("e\xCC\x81 combined").toUtf8() + QByteArrayLiteral("\r\n");
    bytes += QString::fromUtf8("\xF0\x9D\x84\x9E clef").toUtf8() + QByteArrayLiteral("\r\n");
    bytes += QByteArrayLiteral("\x1b[38;2;12;200;90mstyled\x1b[0m rest\r\n");
    bytes += QByteArrayLiteral(
        "\x1b]8;;https://vnm-terminal.example.invalid/doc\x1b\\link\x1b]8;;\x1b\\ after\r\n");
    bytes += QByteArrayLiteral("last visible\r\n");
    return bytes;
}

// The parity comparison is only worth what the fixture covers, so the shapes
// that make it interesting are asserted rather than assumed. A run of two or
// more spaces before further text can only come from the gap padding, because
// no fixture line writes consecutive spaces.
bool check_fixture_covers_every_text_shape(const term::Terminal_screen_model& model)
{
    bool wide_cell          = false;
    bool padded_gap         = false;
    bool combining_sequence = false;
    bool surrogate_pair     = false;
    term::Terminal_search_row_text text;
    for (int logical_row = 0; logical_row < model.scrollback_size(); ++logical_row) {
        if (!model.search_row_text(term::Terminal_buffer_id::PRIMARY, logical_row, text)) {
            continue;
        }

        padded_gap = padded_gap || text.view().contains(QStringView(u"  "));
        for (std::size_t unit = 1U; unit < text.spans.size(); ++unit) {
            const term::terminal_search_column_span_t span = text.spans[unit];
            wide_cell = wide_cell || span.end_column - span.first_column > 1;
            if (text.spans[unit - 1U] != span) {
                continue;
            }

            if (QChar::isHighSurrogate(text.units[unit - 1U])) {
                surrogate_pair = true;
            }
            else {
                combining_sequence = true;
            }
        }
    }

    return check(wide_cell, "the parity fixture retains a wide cell") &&
        check(padded_gap, "the parity fixture retains a padded interior gap") &&
        check(combining_sequence, "the parity fixture retains a combining sequence") &&
        check(surrogate_pair, "the parity fixture retains a non-BMP scalar");
}

bool test_search_row_text_matches_published_snapshot_rows()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 16, 64);
    (void)model.ingest(parity_fixture_bytes());
    const term::terminal_retained_history_diagnostics_t diagnostics =
        model.retained_history_diagnostics();
    ok &= check(
        diagnostics.payload_kind_prefix_plain_ascii_rows > 0U &&
        diagnostics.payload_kind_generic_compact_rows    > 0U,
        "the parity fixture retains both retained-history payload kinds");
    ok &= check_fixture_covers_every_text_shape(model);
    ok &= check_primary_parity_over_history(model, "search row text parity at the seeded width");

    term::Terminal_screen_model widened = make_model(4, 16, 64);
    (void)widened.ingest(parity_fixture_bytes());
    (void)widened.resize(term::terminal_grid_size_t{4, 24});
    ok &= check_primary_parity_over_history(
        widened,
        "search row text parity for retained rows narrower than the live grid");

    term::Terminal_screen_model narrowed = make_model(4, 16, 64);
    (void)narrowed.ingest(parity_fixture_bytes());
    (void)narrowed.resize(term::terminal_grid_size_t{4, 10});
    ok &= check_primary_parity_over_history(
        narrowed,
        "search row text parity for retained rows wider than the live grid");

    term::Terminal_screen_model alternate = make_model(4, 16, 64);
    (void)alternate.ingest(parity_fixture_bytes());
    (void)alternate.ingest(QByteArrayLiteral("\x1b[?1049halternate ") +
        QString::fromUtf8("\xE6\xBC\xA2").toUtf8() + QByteArrayLiteral("\r\nsecond"));
    const term::Terminal_render_snapshot alternate_snapshot = alternate.render_snapshot(1U);
    ok &= check(
        alternate_snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "the alternate fixture publishes from the alternate buffer");
    term::Terminal_search_row_text actual;
    for (int row = 0; row < alternate_snapshot.viewport.visible_rows; ++row) {
        ok &= check(
            alternate.search_row_text(term::Terminal_buffer_id::ALTERNATE, row, actual),
            "search_row_text serves every alternate-buffer row");
        ok &= check_row_parity(
            expected_row_text_from_snapshot(alternate_snapshot, row),
            actual,
            "search row text parity on the alternate buffer",
            row);
    }

    return ok;
}

bool test_retained_history_ordinal_range_tracks_appends_and_evictions()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model(3, 8, 6);

    const auto range_is_contiguous = [&](const char* label) {
        const term::terminal_retained_history_ordinal_range_t range =
            model.retained_history_ordinal_range();
        return check(
            range.end_ordinal - range.first_ordinal ==
                static_cast<std::uint64_t>(model.scrollback_size()),
            label);
    };

    ok &= range_is_contiguous("an empty retained history reports an empty ordinal range");

    (void)model.ingest(QByteArrayLiteral("row-a\r\nrow-b\r\nrow-c\r\n"));
    ok &= range_is_contiguous("appends keep the ordinal range contiguous");
    const term::terminal_retained_history_ordinal_range_t after_appends =
        model.retained_history_ordinal_range();
    ok &= check(after_appends.first_ordinal == 0U,
        "the first retained ordinal starts at zero");

    (void)model.ingest(QByteArrayLiteral(
        "row-d\r\nrow-e\r\nrow-f\r\nrow-g\r\nrow-h\r\nrow-i\r\n"));
    ok &= range_is_contiguous("evictions keep the ordinal range contiguous");
    const term::terminal_retained_history_ordinal_range_t after_evictions =
        model.retained_history_ordinal_range();
    ok &= check(
        after_evictions.first_ordinal > after_appends.first_ordinal &&
        after_evictions.end_ordinal   > after_appends.end_ordinal,
        "eviction advances the live ordinal window forward");

    (void)model.set_scrollback_limit(2);
    ok &= range_is_contiguous("a scrollback-limit reduction keeps the range contiguous");
    const term::terminal_retained_history_ordinal_range_t after_limit =
        model.retained_history_ordinal_range();
    ok &= check(
        after_limit.first_ordinal >= after_evictions.first_ordinal &&
        after_limit.end_ordinal   == after_evictions.end_ordinal,
        "a scrollback-limit reduction drops only the oldest ordinals");

    (void)model.ingest(QByteArrayLiteral("\x1b[3J"));
    const term::terminal_retained_history_ordinal_range_t after_clear =
        model.retained_history_ordinal_range();
    ok &= check(model.scrollback_size() == 0, "CSI 3 J clears retained history");
    ok &= check(
        after_clear.first_ordinal == after_clear.end_ordinal &&
        after_clear.end_ordinal   <  after_limit.end_ordinal,
        "a retained-history clear resets the ordinal spine rather than advancing it");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_identity_spans_stay_implicit_until_needed();
    ok &= test_search_row_text_matches_published_snapshot_rows();
    ok &= test_retained_history_ordinal_range_tracks_appends_and_evictions();

    if (!ok) {
        std::cerr << "search index tests failed\n";
        return 1;
    }

    std::cout << "search index tests passed\n";
    return 0;
}
