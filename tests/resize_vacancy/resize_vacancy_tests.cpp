#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QString>
#include <iostream>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

// The oracle for every expectation in this suite is the resize-vacancy
// lifecycle of the batch-1 design (DESIGN.md 5.2): resize-appended rows are
// vacant, provenance follows native reflow slot by slot, cursor entry and
// application placement retire the affected row and every earlier row,
// erasure and attributes preserve it, whole-screen erasure retires it, rows
// carry it when they move, and V is the trailing run of wholly vacant rows.

term::Terminal_screen_model make_model(int rows, int columns, int scrollback_limit = 100)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = {rows, columns};
    config.scrollback_limit = scrollback_limit;
    config.cell_pixel_size  = term::terminal_cell_pixel_size_t{10, 20};
    return term::Terminal_screen_model(config);
}

bool expect_vacancy(
    const term::Terminal_screen_model& model,
    int                                expected,
    const std::string&                 step)
{
    const int actual = model.resize_vacancy_rows();
    if (actual != expected) {
        std::cerr << step << ": expected V=" << expected << " actual V=" << actual
                  << " grid=" << model.grid_size().rows << 'x' << model.grid_size().columns
                  << " cursor_row=" << model.cursor_position().row << '\n';
    }
    return check(actual == expected, "the trailing wholly vacant row count follows the resize-vacancy lifecycle");
}

bool expect_rows(
    const term::Terminal_screen_model& model,
    const std::vector<QString>&        expected,
    const std::string&                 step)
{
    bool ok = true;
    for (int row = 0; row < static_cast<int>(expected.size()); ++row) {
        const QString actual = model.row_text(row);
        if (actual.trimmed() != expected[static_cast<std::size_t>(row)]) {
            std::cerr << step << ": row " << row << " expected \""
                      << expected[static_cast<std::size_t>(row)].toStdString()
                      << "\" actual \"" << actual.trimmed().toStdString() << "\"\n";
            ok = false;
        }
    }
    return check(ok, "native rows keep the written text through the sequence");
}

// "a" and "b" written at 2x8 with the cursor after "b"; the grid then grows to
// six rows, appending four vacant rows.
term::Terminal_screen_model grown_prompt_model()
{
    auto model = make_model(2, 8);
    model.ingest(QByteArrayLiteral("a\r\nb"));
    model.resize({6, 8});
    return model;
}

// One operation applied to the grown prompt: the four vacant rows must exist
// before it, so a retirement to zero is evidence of retirement.
bool expect_after_operation(
    const QByteArray&  bytes,
    int                expected,
    const std::string& step)
{
    auto model = grown_prompt_model();
    bool ok = expect_vacancy(model, 4, step + " (before)");
    model.ingest(bytes);
    ok &= expect_vacancy(model, expected, step);
    return ok;
}

bool run_mixed_source_join_split(const QByteArray& style_vacancy, const std::string& variant)
{
    bool ok = true;
    auto model = grown_prompt_model();
    ok &= expect_vacancy(model, 4, variant + " growth to 6x8");

    model.ingest(style_vacancy);
    ok &= expect_vacancy(model, 4, variant + " styling the vacant rows");

    model.resize({6, 4});
    ok &= expect_vacancy(model, 4, variant + " first narrowing to 6x4");
    ok &= expect_rows(model, {QStringLiteral("a"), QStringLiteral("b")}, variant + " first narrowing");

    model.ingest(QByteArrayLiteral("\x1b[3;1HX\x1b[2;1H"));
    ok &= expect_vacancy(model, 3, variant + " placement in row 2 and return to row 1");

    model.resize({6, 8});
    ok &= expect_vacancy(model, 3, variant + " widening joins the mixed row");
    ok &= expect_rows(model, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("X")},
        variant + " widening");

    model.resize({6, 4});
    ok &= expect_vacancy(model, 3, variant + " second narrowing splits the mixed row");
    ok &= expect_rows(model, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("X")},
        variant + " second narrowing");
    return ok;
}

bool test_mixed_source_join_split()
{
    bool ok = run_mixed_source_join_split(
        QByteArrayLiteral("\x1b[3;1;6;8;7$r"), "DECCARA");
    // Styled ED 0 from the cursor, then a default EL that leaves row 1 as it was:
    // only the vacant rows keep occupied inverse cells.
    ok &= run_mixed_source_join_split(
        QByteArrayLiteral("\x1b[7m\x1b[J\x1b[0m\x1b[K"), "styled ED 0");
    return ok;
}

bool test_height_resizes()
{
    bool ok = true;
    auto model = grown_prompt_model();
    ok &= expect_vacancy(model, 4, "growth 2 -> 6");
    model.resize({3, 8});
    ok &= expect_vacancy(model, 1, "partial shrink 6 -> 3");
    model.resize({1, 8});
    ok &= expect_vacancy(model, 0, "shrink below the content");
    ok &= check(model.scrollback_size() == 1 && model.row_text(0) == QStringLiteral("b"),
        "shrinking below the content archives the row above the cursor");
    model.resize({8, 8});
    ok &= expect_vacancy(model, 7, "growth past the starting height");
    model.resize({5, 8});
    ok &= expect_vacancy(model, 4, "shrink within the vacancy");
    return ok;
}

bool test_width_round_trips()
{
    bool ok = true;
    auto model = grown_prompt_model();
    model.ingest(QByteArrayLiteral("\x1b[3;1;6;8;7$r"));
    for (int trip = 0; trip < 3; ++trip) {
        const std::string step = "round trip " + std::to_string(trip + 1);
        model.resize({6, 4});
        ok &= expect_vacancy(model, 4, step + " narrowing");
        model.resize({6, 8});
        ok &= expect_vacancy(model, 4, step + " widening");
        ok &= expect_rows(model, {QStringLiteral("a"), QStringLiteral("b")}, step);
    }

    auto blank = grown_prompt_model();
    for (int trip = 0; trip < 2; ++trip) {
        blank.resize({6, 3});
        ok &= expect_vacancy(blank, 4, "empty vacant rows keep their empty-row origin when narrowed");
        blank.resize({6, 8});
        ok &= expect_vacancy(blank, 4, "empty vacant rows keep their empty-row origin when widened");
    }
    return ok;
}

bool test_width_only_vacancy()
{
    bool ok = true;
    auto model = make_model(4, 4);
    model.ingest(QByteArrayLiteral("abcdefgh\r\np"));
    ok &= expect_vacancy(model, 0, "a fresh grid holds no resize vacancy");
    model.resize({4, 8});
    ok &= expect_vacancy(model, 1, "rows joined by widening leave one appended vacant row");
    ok &= expect_rows(model, {QStringLiteral("abcdefgh"), QStringLiteral("p")}, "width-only growth");

    auto staged = make_model(4, 8);
    staged.ingest(QByteArrayLiteral("line0\r\nline1\r\n$ "));
    staged.resize({3, 4});
    ok &= expect_vacancy(staged, 0, "4x8 -> 3x4 archives and truncates without vacancy");
    staged.resize({4, 8});
    ok &= expect_vacancy(staged, 2, "3x4 -> 4x8 joins rows and appends vacancy");
    ok &= expect_rows(staged, {QStringLiteral("line1"), QStringLiteral("$")}, "4x8 -> 3x4 -> 4x8");
    ok &= check(staged.scrollback_size() == 2,
        "the rows archived at width four stay archived");
    return ok;
}

bool test_wide_glyph_reflow()
{
    bool ok = true;

    // One column: the wide glyph keeps its own row and the cursor slot after it
    // pushes the vacant rows down; widening rejoins them and appends vacancy.
    auto narrow = make_model(2, 4);
    narrow.ingest(QByteArrayLiteral("a\r\n\xe4\xb8\xad"));
    narrow.resize({6, 4});
    narrow.ingest(QByteArrayLiteral("\x1b[3;1;6;4;7$r"));
    ok &= expect_vacancy(narrow, 4, "styled vacancy below a wide glyph");
    narrow.resize({6, 1});
    ok &= expect_vacancy(narrow, 3, "one-column reflow keeps the vacant fragments after the cursor row");
    narrow.resize({6, 4});
    ok &= expect_vacancy(narrow, 4, "widening from one column rejoins the vacant fragments");

    // Wrap margin: a wide glyph placed in a split vacant row makes it
    // application space; narrowing to three columns leaves a wrap margin that
    // inherits the glyph row's provenance, then a mixed row, then vacancy.
    auto margin = grown_prompt_model();
    margin.ingest(QByteArrayLiteral("\x1b[3;1;6;8;7$r"));
    margin.resize({6, 4});
    margin.ingest(QByteArrayLiteral("\x1b[3;3H\xe4\xb8\xad\x1b[2;1H"));
    ok &= expect_vacancy(margin, 3, "a wide glyph placed in the first split vacant row");
    margin.resize({6, 3});
    // Rows: a, b, [inv inv margin], [glyph inv], [inv inv inv], [inv inv inv];
    // the cursor stays on row 1 and the remaining rows are truncated.
    ok &= expect_vacancy(margin, 2, "the wrap margin and the glyph's mixed row stay visible");
    margin.resize({6, 4});
    ok &= expect_vacancy(margin, 3, "rejoining at four columns restores the retired row and vacancy");
    return ok;
}

bool test_retirement_semantics()
{
    bool ok = true;

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[5B\x1b[5A"), 0,
        "cursor down then up within one ingest retires what it entered");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[?25l\x1b[5;1H\x1b[2;1H"), 1,
        "a hidden cursor still retires the rows it enters");

    auto identical = grown_prompt_model();
    ok &= expect_vacancy(identical, 4, "identical-value copy (before)");
    const QString before_copy = identical.row_text(4);
    identical.ingest(QByteArrayLiteral("\x1b[6;1;6;8;1;5;1;1$v"));
    ok &= expect_vacancy(identical, 1, "copying identical blank cells is placement and retires");
    ok &= check(identical.row_text(4) == before_copy,
        "the identical-value copy leaves the destination cells unchanged");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[4;1HX\x1b[2K\x1b[2;2H"), 2,
        "write then erase in one ingest stays retired");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[J"), 4,
        "ED 0 below the cursor preserves vacancy");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[1J"), 4,
        "ED 1 above the cursor preserves vacancy");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[H\x1b[J\x1b[2;1H"), 0,
        "ED 0 from home erases the whole screen and retires");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[2J"), 0,
        "ED 2 retires every row");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[3J"), 4,
        "ED 3 changes only history");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[41m\x1b[J\x1b[0m"), 4,
        "styled erasure makes vacant cells occupied but keeps them vacant");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[1;1;6;8;1$r"), 4,
        "attribute changes preserve vacancy");

    auto line_feeds = make_model(2, 8);
    line_feeds.ingest(QByteArrayLiteral("a\r\n$ "));
    line_feeds.resize({6, 8});
    line_feeds.ingest(QByteArrayLiteral("\n\n\x1b[H"));
    ok &= expect_vacancy(line_feeds, 2, "rows reached by line feeds stay application space after a move home");
    line_feeds.resize({5, 8});
    ok &= expect_vacancy(line_feeds, 1, "a shrink truncates vacancy before the rows the line feeds reached");
    line_feeds.resize({6, 8});
    ok &= expect_vacancy(line_feeds, 2, "regrowth appends vacancy below the retired rows");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[5;1H\x1b" "7\x1b[1;1H\x1b" "8\x1b[1;1H"), 1,
        "cursor save and restore retire the restored row");
    return ok;
}

bool test_row_operations()
{
    bool ok = true;

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[S"), 0,
        "SU installs an application row below the vacancy and retires it");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[T"), 3,
        "SD moves the vacant rows with their provenance");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[L"), 3,
        "IL pushes vacant rows down and drops the last one");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[M"), 0,
        "DL installs application blank rows below the former vacancy");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[H\x1bM\x1b[2;1H"), 3,
        "RI at the top moves the rows down");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[1;3r\x1b[S\x1b[r\x1b[2;1H"), 3,
        "SU in a top region retires only rows above its new application row");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[3;6r\x1b[T\x1b[r\x1b[2;1H"), 3,
        "SD in a vacant region installs an application row at its top");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[?69h\x1b[1;4s\x1b[S\x1b[?69l\x1b[2;1H"), 0,
        "segment copies inside horizontal margins are placement");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[88;5;1;5;8$x"), 1,
        "rectangular fill is placement");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[2;1;2;8;1;4;1;1$v"), 2,
        "rectangular copy is placement");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[1'}"), 0,
        "column insertion shifts every row of the region");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[1'~"), 0,
        "column deletion shifts every row of the region");

    ok &= expect_after_operation(QByteArrayLiteral("\x1b[@"), 4,
        "insertion at the cursor changes only the cursor row");
    return ok;
}

bool test_alternate_screen_scope()
{
    bool ok = true;
    auto model = grown_prompt_model();
    model.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[6;1Halternate"));
    ok &= expect_vacancy(model, 4, "alternate-screen output leaves the primary provenance alone");
    model.ingest(QByteArrayLiteral("\x1b[?1049l"));
    ok &= expect_vacancy(model, 4, "the primary provenance persists across the alternate screen");

    auto resized = make_model(2, 8);
    resized.ingest(QByteArrayLiteral("a\r\nb\x1b[?1049h"));
    resized.resize({6, 8});
    resized.ingest(QByteArrayLiteral("\x1b[?1049l"));
    ok &= expect_vacancy(resized, 4, "a resize while the alternate screen is active appends primary vacancy");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_mixed_source_join_split();
    ok &= test_height_resizes();
    ok &= test_width_round_trips();
    ok &= test_width_only_vacancy();
    ok &= test_wide_glyph_reflow();
    ok &= test_retirement_semantics();
    ok &= test_row_operations();
    ok &= test_alternate_screen_scope();
    return ok ? 0 : 1;
}
