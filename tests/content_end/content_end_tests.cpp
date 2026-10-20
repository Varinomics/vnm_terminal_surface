#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_content_tail_projection.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QString>
#include <memory>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

// The oracle is the adopted transcript contract: current occupied content and
// the logical cursor define the end; only explicit page clears establish a
// clear boundary. A resize or a later erase cannot leave scrollable tail space.
term::Terminal_screen_model make_model(int rows, int columns, int history = 6000)
{
    term::Terminal_screen_model_config config;
    config.grid_size = {rows, columns};
    config.scrollback_limit = history;
    config.cell_pixel_size = term::terminal_cell_pixel_size_t{10, 20};
    return term::Terminal_screen_model(config);
}

std::shared_ptr<const term::Terminal_render_snapshot> presented(term::Terminal_screen_model& model)
{
    return term::make_presented_snapshot(
        std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(1U)));
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

bool expect_cursor_at_tail(term::Terminal_screen_model& model)
{
    const auto before = model.render_snapshot(1U);
    const auto display = presented(model);
    const auto after = model.render_snapshot(1U);
    if (!check(display != nullptr, "the current content window is fully acquired")) {
        return false;
    }
    return check(display->cursor.position.row == display->grid_size.rows - 1,
               "the current prompt occupies the final displayed row") &&
        check(term::render_snapshot_visible_line_provenance_is_valid(*display),
            "presented rows preserve native identities and padding has no identity") &&
        check(before.visible_line_provenance == after.visible_line_provenance &&
                before.cursor.position == after.cursor.position,
            "presentation leaves native identities, generations and cursor untouched");
}

QByteArray shell_prompt(const QByteArray& prompt)
{
    // Exact zsh -f startup/prompt bytes captured from a Linux PTY at 24x80.
    return QByteArrayLiteral("\x1b[1m\x1b[7m#\x1b[27m\x1b[1m\x1b[0m") +
        QByteArray(79, ' ') + QByteArrayLiteral("\r \r\r\x1b[0m\x1b[27m\x1b[24m\x1b[J") +
        prompt + QByteArrayLiteral("\x1b[K\x1b[?2004h");
}

void captured_seq(term::Terminal_screen_model& model, const QByteArray& prompt)
{
    model.ingest(shell_prompt(prompt));
    QByteArray output = QByteArrayLiteral("s\bseq 1 5000\x1b[?2004l\r\r\n");
    for (int line = 1; line <= 5000; ++line) {
        output += QByteArray::number(line) + QByteArrayLiteral("\r\n");
    }
    output += shell_prompt(prompt);
    model.ingest(output);
}

QByteArray redraw(const QByteArray& prompt, bool move_up = false)
{
    return QByteArrayLiteral("\r\r") + (move_up ? QByteArrayLiteral("\x1b[A") : QByteArray()) +
        QByteArrayLiteral("\x1b[0m\x1b[27m\x1b[24m\x1b[J") + prompt;
}

bool test_captured_wrapped_prompt_redraw()
{
    auto model = make_model(24, 80);
    const QByteArray prompt = QByteArrayLiteral("investigator@computer project> ");
    captured_seq(model, prompt);
    const int initial_max = term::presentation_max_offset_from_tail(presented(model)->viewport);
    bool ok = expect_cursor_at_tail(model);

    // Captured TIOCSWINSZ sequence and zsh responses. CUU on the first growth
    // erases the former wrapped prompt row; that row must stop extending E.
    const std::vector<term::terminal_grid_size_t> sizes = {{12, 40}, {6, 20}, {12, 40}, {24, 80}};
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        model.resize(sizes[index]);
        model.ingest(redraw(prompt, index == 2U));
        ok &= expect_cursor_at_tail(model);
    }
    const auto display = presented(model);
    ok &= check(row_text(*display, 21) == QStringLiteral("4999") &&
            row_text(*display, 22) == QStringLiteral("5000") &&
            row_text(*display, 23) == QStringLiteral("investigator@computer project>"),
        "the numbered output and redrawn prompt remain adjacent after the zoom roundtrip");
    ok &= check(term::presentation_max_offset_from_tail(display->viewport) == initial_max,
        "the roundtrip does not add blank scroll depth");
    return ok;
}

bool test_captured_one_row_redraw()
{
    auto model = make_model(24, 80);
    const QByteArray prompt = QByteArrayLiteral("prompt> ");
    captured_seq(model, prompt);
    const int initial_max = term::presentation_max_offset_from_tail(presented(model)->viewport);
    model.resize({1, 80});
    model.ingest(redraw(prompt));
    model.resize({24, 80});
    bool ok = expect_cursor_at_tail(model);
    model.ingest(redraw(prompt));
    ok &= expect_cursor_at_tail(model);
    const auto display = presented(model);
    ok &= check(row_text(*display, 22) == QStringLiteral("5000") &&
            term::presentation_max_offset_from_tail(display->viewport) == initial_max,
        "ED0 at native home neither creates a page break nor restores blank tail depth");
    return ok;
}

bool test_current_semantic_content()
{
    auto model = make_model(6, 20);
    model.ingest(QByteArrayLiteral("prompt> "));
    model.ingest(QByteArrayLiteral("\x1b[3B\x1b[3A"));
    bool ok = check(model.render_snapshot(1U).native_content_bottom_row_exclusive == 1,
        "a transient cursor visit does not retain abandoned blank rows");
    model.ingest(QByteArrayLiteral("\x1b[?25l\x1b[5;1H"));
    ok &= check(model.render_snapshot(1U).native_content_bottom_row_exclusive == 5,
        "the logical cursor extends the end even while it is hidden");
    model.ingest(QByteArrayLiteral(" \x1b[1;1H"));
    ok &= check(model.render_snapshot(1U).native_content_bottom_row_exclusive == 5,
        "an explicitly written space is semantic content after the cursor leaves");
    model.ingest(QByteArrayLiteral("\x1b[5;1H\x1b[2K\x1b[1;1H"));
    ok &= check(model.render_snapshot(1U).native_content_bottom_row_exclusive == 1,
        "default erasure removes a formerly occupied tail from the current end");
    model.ingest(QByteArrayLiteral("\x1b[6;1H\x1b[41m\x1b[2K\x1b[0m\x1b[1;1H"));
    ok &= check(model.render_snapshot(1U).native_content_bottom_row_exclusive == 6,
        "a styled erasure is occupied content even without visible glyphs");

    auto allocation = make_model(2, 20);
    allocation.ingest(QByteArrayLiteral("a\r\nprompt> \x1b[41m"));
    allocation.resize({8, 20});
    ok &= check(allocation.render_snapshot(1U).native_content_bottom_row_exclusive == 2,
        "resize allocation creates unwritten default cells despite the active SGR style");
    allocation.ingest(QByteArrayLiteral("\x1b[J"));
    ok &= check(allocation.render_snapshot(1U).native_content_bottom_row_exclusive == 8,
        "an application styled erase after growth intentionally extends the end");

    auto image = make_model(6, 20);
    image.ingest(QByteArrayLiteral("\x1b[4;1H\x1bP9;1q#1;2;100;0;0#1!10~\x1b\\\x1b[1;1H"));
    ok &= check(image.render_snapshot(1U).native_content_bottom_row_exclusive == 4,
        "an image slice extends the semantic end independently of the cursor");
    return ok;
}

bool test_explicit_clear_boundary()
{
    auto model = make_model(4, 20, 100);
    for (int line = 0; line < 12; ++line) {
        model.ingest(QByteArray::number(line) + QByteArrayLiteral("\r\n"));
    }
    const int history = model.scrollback_size();
    model.ingest(QByteArrayLiteral("\x1b[H\x1b[2Jprompt> "));
    const auto cleared = presented(model);
    bool ok = check(cleared && model.presentation_mapping().clear_page_start_row == history,
        "ED2 establishes the latest page boundary at the native live-screen start");
    ok &= check(row_text(*cleared, 0).isEmpty() && row_text(*cleared, 2).isEmpty() &&
            row_text(*cleared, 3) == QStringLiteral("prompt>"),
        "a short cleared page has padding before the bottom-aligned prompt");
    ok &= check(term::presentation_max_offset_from_tail(cleared->viewport) == history,
        "old history remains reachable before the cleared page");

    model.resize({1, 20});
    model.resize({8, 20});
    model.ingest(QByteArrayLiteral("\r\x1b[Jprompt> "));
    const auto grown = presented(model);
    ok &= check(model.presentation_mapping().clear_page_start_row == history &&
            row_text(*grown, 6).isEmpty() && row_text(*grown, 7) == QStringLiteral("prompt>"),
        "the explicit boundary survives a full page and later growth and ED0 redraw");

    const auto before_eviction = model.presentation_mapping();
    model.set_scrollback_limit(3);
    ok &= check(model.presentation_mapping().clear_page_start_row == 3 &&
            before_eviction.clear_page_start_row > 3,
        "history eviction rebases the page boundary in retained logical coordinates");
    model.ingest(QByteArrayLiteral("\x1b[3J"));
    ok &= check(model.presentation_mapping().clear_page_start_row == 0,
        "clearing retained history rebases the page boundary to zero");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_captured_wrapped_prompt_redraw();
    ok &= test_captured_one_row_redraw();
    ok &= test_current_semantic_content();
    ok &= test_explicit_clear_boundary();
    return ok ? 0 : 1;
}
