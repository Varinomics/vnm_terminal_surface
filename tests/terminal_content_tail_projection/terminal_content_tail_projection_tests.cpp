#include "vnm_terminal/internal/terminal_content_tail_projection.h"

#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <memory>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Terminal_screen_model make_model(int rows, int history = 2000)
{
    term::Terminal_screen_model_config config;
    config.grid_size = {rows, 20};
    config.scrollback_limit = history;
    config.cell_pixel_size = term::terminal_cell_pixel_size_t{10, 20};
    return term::Terminal_screen_model(config);
}

term::Terminal_render_snapshot_request request_for(int source_offset = 0, int rows = 0, int offset = -1)
{
    term::Terminal_render_snapshot_request request;
    request.sequence = 157U;
    request.publication_generation = 29U;
    request.row_origin_generation = 11U;
    request.viewport.offset_from_tail = source_offset;
    request.presentation_rows = rows;
    request.presentation_offset_from_tail = offset;
    return request;
}

QByteArray numbered_lines(int count)
{
    QByteArray output;
    for (int line = 0; line < count; ++line) {
        output += QByteArray::number(line) + QByteArrayLiteral("\r\n");
    }
    return output;
}

QString row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    QString text;
    for (const auto& cell : snapshot.cells) {
        if (cell.position.row == row && !cell.wide_continuation) {
            cell.text.append_to(text);
        }
    }
    return text.trimmed();
}

// Construct the agreed stream independently: every native row through E-1,
// the history before an explicit clear included, then the unused slots that
// fill the rest of the view below a page shorter than it.
std::vector<int> expected_stream(int capacity, int end, int page)
{
    std::vector<int> stream;
    for (int row = 0; row < end; ++row) {
        stream.push_back(row);
    }
    stream.insert(stream.end(), static_cast<std::size_t>(std::max(0, capacity - (end - page))), -1);
    return stream;
}

bool test_stream_mapping_and_inverse()
{
    struct mapping_case_t { int capacity; int history; int content; int page; };
    constexpr mapping_case_t cases[] = {
        {6, 0, 2, 0}, {6, 3, 2, 0}, {6, 9, 2, 9}, {6, 9, 5, 4},
        {2, 4, 6, 0}, {10, 20, 1, 20}, {4, 20, 1, 20},
    };
    bool ok = true;
    for (const auto sample : cases) {
        const int end = sample.history + sample.content;
        const auto stream = expected_stream(sample.capacity, end, sample.page);
        term::Terminal_viewport_state viewport;
        viewport.visible_rows = sample.capacity;
        viewport.scrollback_rows = sample.history;
        viewport.presentation = term::make_presentation_mapping(
            term::Terminal_buffer_id::PRIMARY, sample.history, sample.content, sample.page);
        const int maximum = static_cast<int>(stream.size()) - sample.capacity;
        ok &= check(term::presentation_max_offset_from_tail(viewport) == maximum,
            "scroll bounds end at the current page's final native row");
        for (int offset = 0; offset <= maximum; ++offset) {
            viewport.offset_from_tail = offset;
            const int first = maximum - offset;
            for (int row = 0; row < sample.capacity; ++row) {
                ok &= check(term::presentation_requested_logical_row(viewport, row) ==
                        stream[static_cast<std::size_t>(first + row)],
                    "every displayed row follows the declared stream and padding has no row");
            }
            for (int logical = 0; logical < end; ++logical) {
                const auto found = std::find(stream.begin(), stream.end(), logical);
                ok &= check(term::presentation_display_row_for_logical_row(viewport, logical) ==
                        static_cast<int>(found - stream.begin()) - first,
                    "inverse mapping locates the same native row, including outside the window");
            }
            ok &= check(!term::presentation_display_row_for_logical_row(viewport, end).has_value(),
                "a native row after the semantic end is never presented");
        }
        viewport.offset_from_tail = 0;
        const int page_rows = end - sample.page;
        ok &= check(page_rows >= sample.capacity
                ? term::presentation_requested_logical_row(viewport, sample.capacity - 1) == end - 1
                : term::presentation_requested_logical_row(viewport, 0) == sample.page &&
                    term::presentation_requested_logical_row(viewport, page_rows - 1) == end - 1 &&
                    term::presentation_requested_logical_row(viewport, page_rows) == -1,
            "at tail a page that fits starts on the top row and a longer one ends on the final row");
    }
    return ok;
}

bool test_larger_consumer_crossing_clear_padding()
{
    auto model = make_model(24);
    model.ingest(numbered_lines(1023));
    model.ingest(QByteArrayLiteral("\x1b[H\x1b[2Jprompt> "));
    bool ok = check(model.scrollback_size() == 1000, "the source has H=1000 and L=24");
    const auto request = request_for(120, 100, 120);
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(request));
    const auto before = native->visible_line_provenance;
    ok &= check(native->grid_size.rows == 24 && native->viewport.offset_from_tail == 120 &&
            native->native_content_bottom_row_exclusive == 1 && native->clear_page_start_row == 1000,
        "consumer acquisition preserves native geometry and the cleared page basis");
    ok &= check(term::render_snapshot_row_coverage_is_coherent(*native) &&
            term::render_snapshot_first_visible_logical_row(*native->row_coverage_snapshot) == 880 &&
            native->row_coverage_snapshot->grid_size.rows == 100,
        "the larger window acquires native880..979, including rows after the native window");
    const auto display = term::make_presented_snapshot(native, 100, 120);
    if (!check(display != nullptr, "the larger consumer resolves its complete requested interval")) {
        return false;
    }
    ok &= check(display->grid_size.rows == 100 && display->display_source->grid_size.rows == 24 &&
            display->display_row_logical_rows.front() == 880 && display->display_row_logical_rows.back() == 979 &&
            row_text(*display, 0) == QStringLiteral("880") && row_text(*display, 99) == QStringLiteral("979"),
        "a large phone viewport uses actual requested content after a clear boundary");
    ok &= check(term::render_snapshot_visible_line_provenance_is_valid(*display) &&
            before == native->visible_line_provenance,
        "arbitrary coverage preserves row identity and leaves the native source untouched");

    auto missing = *native;
    missing.row_coverage_snapshot.reset();
    ok &= check(!term::render_snapshot_presentable(missing, 100, 120),
        "missing rows after the native source cannot silently become blank space");
    missing.frozen_basis = true;
    ok &= check(!term::render_snapshot_presentable(missing, 100, 120),
        "a frozen frame also rejects unavailable rows after its coverage");
    auto mismatched = *native;
    auto stale = std::make_shared<term::Terminal_render_snapshot>(*native->row_coverage_snapshot);
    ++stale->metadata.publication_generation;
    mismatched.row_coverage_snapshot = stale;
    ok &= check(!term::render_snapshot_presentable(mismatched, 100, 120),
        "coverage from another publication cannot complete the frame");
    return ok;
}

bool test_smaller_consumer_offset_exceeds_history()
{
    auto model = make_model(24);
    model.ingest(numbered_lines(31) + QByteArrayLiteral("prompt> "));
    const int history = model.scrollback_size();
    const int consumer_max = history + 24 - 6;
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(history, 6, consumer_max)));
    const auto display = term::make_presented_snapshot(native, 6, consumer_max);
    if (!check(display != nullptr, "a small consumer can scroll through its full content range")) {
        return false;
    }
    return check(display->viewport.offset_from_tail == consumer_max && consumer_max > history &&
            native->viewport.offset_from_tail == history,
               "consumer offset is independent of the native source offset") &&
        check(display->display_row_logical_rows.front() == 0 &&
                display->display_row_logical_rows.back() == 5,
            "the consumer reaches the first retained rows") &&
        check(term::validate_render_snapshot_metadata(*display).status == term::Terminal_render_snapshot_status::OK,
            "display metadata validates in the consumer offset domain");
}

bool test_coverage_includes_live_rows_from_same_source()
{
    auto model = make_model(24);
    model.ingest(numbered_lines(1023) + QByteArrayLiteral("prompt> "));
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(20, 100, 0)));
    const auto display = term::make_presented_snapshot(native, 100, 0);
    if (!check(display != nullptr, "the consumer acquires live rows beyond the native source window")) {
        return false;
    }
    bool ok = check(native->viewport.scrollback_rows == 1000 && native->grid_size.rows == 24 &&
            !native->cursor.visible && native->cursor.position.row == 43,
        "the native window covers980..1003 and clips the logical cursor");
    ok &= check(term::render_snapshot_row_coverage_is_coherent(*native) &&
            term::render_snapshot_first_visible_logical_row(*native->row_coverage_snapshot) == 924 &&
            native->row_coverage_snapshot->grid_size.rows == 100 &&
            display->display_row_logical_rows.front() == 924 &&
            display->display_row_logical_rows.back() == 1023,
        "same-publication coverage includes retained and live rows924..1023");
    ok &= check(row_text(*display, 99) == QStringLiteral("prompt>") &&
            display->cursor.visible && display->cursor.position.row == 99 &&
            term::render_snapshot_visible_line_provenance_is_valid(*display),
        "the consumer presents the logical cursor using its own window and preserved row identities");
    model.ingest(QByteArrayLiteral("\x1b[?25l"));
    const auto hidden = term::make_presented_snapshot(
        std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(request_for(20, 100, 0))),
        100, 0);
    ok &= check(hidden && !hidden->cursor.visible && hidden->viewport.presentation == display->viewport.presentation,
        "DECTCEM hides painting without moving the semantic endpoint");
    return ok;
}

bool test_shared_cells_images_selection_and_search()
{
    auto model = make_model(2);
    model.ingest(QByteArrayLiteral(
        "\x1b]8;;https://example.test\x07\x1b[31mold\x1b[0m\x1b]8;;\x07"
        "\x1b[1;10H\x1bP9;1q#1;2;100;0;0#1!10~\x1b\\\x1b[1;4H\r\nmid\r\nprompt> "));
    model.resize({6, 20});
    auto request = request_for();
    const term::Terminal_selection_range selection{{0, 0}, {2, 8}, term::Terminal_selection_mode::NORMAL};
    request.selections.push_back({selection,
        model.selection_line_leases(term::Terminal_buffer_id::PRIMARY, selection)});
    auto native = std::make_shared<term::Terminal_render_snapshot>(model.render_snapshot(request));
    if (!check(native->row_coverage_snapshot != nullptr, "history has a coherent auxiliary source")) {
        return false;
    }
    auto coverage = std::make_shared<term::Terminal_render_snapshot>(*native->row_coverage_snapshot);
    coverage->search_match_spans.push_back({0, 0, 3, true});
    native->row_coverage_snapshot = coverage;
    const auto display = term::make_presented_snapshot(native);
    if (!check(display != nullptr, "all row-derived content resolves through the common mapping")) {
        return false;
    }
    bool ok = check(display->display_row_logical_rows == std::vector<int>{0, 1, 2, -1, -1, -1},
        "short content starts on the top row and identity-free slots follow it");
    ok &= check(row_text(*display, 0) == QStringLiteral("old") &&
            row_text(*display, 1) == QStringLiteral("mid") && row_text(*display, 2) == QStringLiteral("prompt>"),
        "history and live text follow the same mapping");
    ok &= check(term::render_snapshot_row_image(*display, 0) != nullptr &&
            display->search_match_spans.size() == 1U && display->search_match_spans.front().row == 0 &&
            display->selection_spans.size() == 3U && display->selection_spans.front().row == 0,
        "images, search and selection share the displayed row coordinates");
    const auto found = std::find_if(display->cells.begin(), display->cells.end(), [](const auto& cell) {
        return cell.position.row == 0 && cell.position.column == 0;
    });
    ok &= check(found != display->cells.end() && found->hyperlink_id != term::k_no_terminal_hyperlink_id &&
            display->styles[found->style_id] != term::make_default_terminal_text_style(),
        "style and hyperlink identities survive auxiliary coverage composition");
    ok &= check(term::render_snapshot_visible_line_provenance_is_valid(*display),
        "the common mapping preserves retained-row provenance");
    return ok;
}

bool test_frozen_coverage_and_alternate_screen()
{
    auto model = make_model(2);
    model.ingest(QByteArrayLiteral("old\r\nnew\r\nprompt> "));
    model.resize({6, 20});
    auto held = std::make_shared<term::Terminal_render_snapshot>(model.render_snapshot(request_for()));
    held->frozen_basis = true;
    const auto held_end = held->viewport.presentation;
    const auto first = term::make_presented_snapshot(held);
    model.ingest(QByteArrayLiteral("\x1b[H\x1b[2Jdifferent"));
    const auto second = term::make_presented_snapshot(held);
    bool ok = check(first && second && second->viewport.presentation == held_end &&
            row_text(*second, 2) == QStringLiteral("prompt>"),
        "a held frame retains its own end, boundary and rows after live output changes");
    held->row_coverage_snapshot.reset();
    const auto without_prefix = term::make_presented_snapshot(held);
    ok &= check(!without_prefix && !term::render_snapshot_presentable(*held),
        "missing frozen history is unavailable; only the page map may create unused slots");

    model.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[6;1Hstatus"));
    const auto alternate = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(request_for()));
    ok &= check(alternate->viewport.presentation.policy == term::Terminal_presentation_policy::NATIVE &&
            term::make_presented_snapshot(alternate) == alternate,
        "the alternate screen keeps native row layout");
    const auto larger = term::make_presented_snapshot(alternate, 8);
    ok &= check(larger && row_text(*larger, 5) == QStringLiteral("status") &&
            larger->display_row_logical_rows[6] == -1 &&
            term::render_snapshot_visible_line_provenance_is_valid(*larger),
        "a larger consumer preserves native alternate rows and adds identity-free external space");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_stream_mapping_and_inverse();
    ok &= test_larger_consumer_crossing_clear_padding();
    ok &= test_smaller_consumer_offset_exceeds_history();
    ok &= test_coverage_includes_live_rows_from_same_source();
    ok &= test_shared_cells_images_selection_and_search();
    ok &= test_frozen_coverage_and_alternate_screen();
    return ok ? 0 : 1;
}
