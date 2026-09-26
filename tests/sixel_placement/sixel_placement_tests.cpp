#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/sixel_decoder.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

// Oracles, per test: the VT330/VT340 Programmer Reference Manual vol. 2 ch. 14
// (an image starts at the upper-left corner of the text active position and
// scrolls the text); the adopted cursor rule V1 (the text cursor ends on the
// row the top of the final sixel row falls in, at the image's first column),
// whose Windows side is checked by the ConPTY cursor-sync gate; the owner
// decisions D1 (an image erases the text it covers), D2 (image rows break
// incoming and outgoing soft wraps), D5 (a sixel row is at most one scroll
// region tall, as in OpenConsole) and S4 (images on one row composite);
// the anchors A1 (one slice per row, owned by the row and then its
// history record), A2/I5 (the decoded-size cap keeps geometry) and A7/I9 (a
// row record over the record limit keeps its text and drops its image).
// Behavior no oracle settles yet is marked provisional where it is asserted.

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr term::terminal_cell_pixel_size_t k_cell{10, 20};

term::Terminal_screen_model make_model(
    int                                             rows,
    int                                             columns,
    std::optional<term::terminal_cell_pixel_size_t> cell            = k_cell,
    int                                             scrollback_rows = 100,
    std::size_t                                     capacity_bytes  =
        term::k_terminal_default_retained_history_capacity_bytes)
{
    term::Terminal_screen_model_config config;
    config.grid_size                       = {rows, columns};
    config.scrollback_limit                = scrollback_rows;
    config.cell_pixel_size                 = cell;
    config.retained_history_capacity_bytes = capacity_bytes;
    return term::Terminal_screen_model(config);
}

QByteArray sixel(const QByteArray& parameters, const QByteArray& data)
{
    return QByteArray("\x1bP") + parameters + 'q' + data + QByteArray("\x1b\\");
}

// Register 1 as full red, then `rows` sixel rows of `width` fully set sixels,
// each but the last followed by a graphics new line.
QByteArray solid_rows(int width, int rows)
{
    QByteArray data("#1;2;100;0;0");
    for (int row = 0; row < rows; ++row) {
        data += "#1!" + QByteArray::number(width) + '~';
        if (row + 1 < rows) {
            data += '-';
        }
    }
    return data;
}

QByteArray cursor_to(int row, int column)
{
    return "\x1b[" + QByteArray::number(row + 1) + ';' + QByteArray::number(column + 1) + 'H';
}

// The decoder's own raster for an image, the reference its bands are cut from.
term::Screen_sixel_image_mutation decoded_image(
    const QByteArray& parameters,
    const QByteArray& data)
{
    term::Sixel_decoder decoder;
    std::vector<term::Parser_action> actions;
    decoder.begin(parameters);
    decoder.decode(data, actions);
    decoder.finish(actions);
    for (const term::Parser_action& action : actions) {
        if (const auto* mutation = std::get_if<term::Screen_mutation>(&action.payload)) {
            if (const auto* image = std::get_if<term::Screen_sixel_image_mutation>(mutation)) {
                return *image;
            }
        }
    }
    return {};
}

bool slice_equals_band(
    const std::shared_ptr<const term::Terminal_image_slice>& slice,
    const QImage&                                            raster,
    int                                                      band_top,
    int                                                      band_width,
    int                                                      first_column)
{
    const int band_height = std::min(k_cell.height, raster.height() - band_top);
    if (slice == nullptr                        ||
        slice->first_column    != first_column  ||
        slice->cell_pixel_size != k_cell        ||
        slice->pixels.format() != QImage::Format_RGBA8888_Premultiplied ||
        slice->pixels.width()  != band_width    ||
        slice->pixels.height() != band_height)
    {
        return false;
    }

    for (int y = 0; y < band_height; ++y) {
        if (std::memcmp(
                slice->pixels.constScanLine(y),
                raster.constScanLine(band_top + y),
                static_cast<std::size_t>(band_width) * 4U) != 0)
        {
            return false;
        }
    }
    return true;
}

bool slices_equal(
    const std::shared_ptr<const term::Terminal_image_slice>& left,
    const std::shared_ptr<const term::Terminal_image_slice>& right)
{
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    return
        left->first_column    == right->first_column    &&
        left->cell_pixel_size == right->cell_pixel_size &&
        left->revision        == right->revision        &&
        left->pixels          == right->pixels;
}

int active_row(const term::Terminal_screen_model& model, int row)
{
    return model.scrollback_size() + row;
}

std::shared_ptr<const term::Terminal_image_slice> slice_at(
    const term::Terminal_screen_model& model,
    int                                row)
{
    return model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, active_row(model, row));
}

QString history_row_text(const term::Terminal_screen_model& model, int row)
{
    const std::optional<std::vector<term::terminal_retained_history_cell_state_for_testing_t>> cells =
        model.retained_history_row_cells_for_testing(term::Terminal_buffer_id::PRIMARY, row);
    QString text;
    if (cells.has_value()) {
        for (const term::terminal_retained_history_cell_state_for_testing_t& cell : *cells) {
            text += cell.occupied ? cell.text : QStringLiteral(" ");
        }
    }
    return text.trimmed();
}

std::vector<term::Terminal_reply> replies_in(const term::Terminal_screen_model_result& result)
{
    std::vector<term::Terminal_reply> replies;
    for (const term::Parser_action& action : result.actions) {
        if (const auto* reply = std::get_if<term::Terminal_reply>(&action.payload)) {
            replies.push_back(*reply);
        }
    }
    return replies;
}

std::vector<term::Parser_payload_diagnostic> diagnostics_in(
    const term::Terminal_screen_model_result& result)
{
    std::vector<term::Parser_payload_diagnostic> diagnostics;
    for (const term::Parser_action& action : result.actions) {
        if (const auto* diagnostic = std::get_if<term::Parser_payload_diagnostic>(&action.payload)) {
            diagnostics.push_back(*diagnostic);
        }
    }
    return diagnostics;
}

const term::Terminal_render_cell* snapshot_cell(
    const term::Terminal_render_snapshot& snapshot,
    int                                   row,
    int                                   column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return &cell;
        }
    }
    return nullptr;
}

// An image of three colors whose sixel rows differ in width and pattern, so
// each band has its own content, with undrawn pixels among the drawn ones.
QByteArray patterned_rows(int rows)
{
    QByteArray data("#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100");
    for (int row = 0; row < rows; ++row) {
        data += "#1!" + QByteArray::number(3 + row) + '~';
        data += "#2!5" + QByteArray(1, static_cast<char>('?' + (1 << (row % 6))));
        data += "#3!2~";
        if (row + 1 < rows) {
            data += '-';
        }
    }
    return data;
}

bool test_slices_equal_raster_bands()
{
    bool ok = true;

    // Seven sixel rows at 1:1 are 42 pixels: bands of 20, 20 and 2 rows.
    const QByteArray data = patterned_rows(7);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);
    ok &= check(image.raster.width() == 16 && image.raster.height() == 42,
        "patterned reference image decodes to 16 x 42 pixels");

    term::Terminal_screen_model model = make_model(8, 30);
    model.ingest(cursor_to(2, 3) + sixel("9;1", data));

    for (int band = 0; band < 3; ++band) {
        ok &= check(slice_equals_band(slice_at(model, 2 + band), image.raster, band * 20, 16, 3),
            "each text row holds the raster band that falls on it, from the cursor column");
    }
    ok &= check(slice_at(model, 1) == nullptr && slice_at(model, 5) == nullptr,
        "rows the image does not reach hold no slice");
    ok &= check(slice_at(model, 2)->revision < slice_at(model, 3)->revision &&
            slice_at(model, 3)->revision < slice_at(model, 4)->revision,
        "every placed slice takes a new revision");

    // A band in which nothing is drawn leaves its row alone (provisional:
    // no reference settles an all-transparent band).
    QByteArray gapped("#1;2;100;0;0#1!4~");
    gapped += QByteArray(7, '-');
    gapped += "#1!4~";
    term::Terminal_screen_model gapped_model = make_model(8, 30);
    gapped_model.ingest(sixel("9;1", gapped));
    ok &= check(slice_at(gapped_model, 0) != nullptr &&
            slice_at(gapped_model, 1) == nullptr &&
            slice_at(gapped_model, 2) != nullptr,
        "a band with no drawn pixel places no slice on its row");

    return ok;
}

bool test_images_clip_at_the_right_margin()
{
    bool ok = true;

    const QByteArray data = solid_rows(55, 1);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);
    term::Terminal_screen_model model = make_model(4, 30);
    model.ingest(cursor_to(0, 27) + sixel("9;1", data));
    ok &= check(slice_equals_band(slice_at(model, 0), image.raster, 0, 30, 27),
        "an image is cut at the right margin, keeping only the columns on the grid");
    return ok;
}

bool test_cursor_follows_the_final_sixel_row()
{
    bool ok = true;

    struct Cursor_case
    {
        const char* name;
        QByteArray  parameters;
        QByteArray  data;
        int         expected_row;
    };

    // Rows are relative to an origin at (2, 4) on a 20-row grid, so nothing
    // scrolls; cells are 20 pixels high.
    const std::vector<Cursor_case> cases = {
        {"one sixel row at 1:1 leaves the cursor on the origin row",
            "9;1", solid_rows(3, 1), 0},
        {"three sixel rows at 2:1 put the final row's top at 24 pixels",
            "0;1", solid_rows(3, 3), 1},
        {"two sixel rows at 3:1 put the final row's top at 18 pixels",
            "3;1", solid_rows(3, 2), 0},
        {"two sixel rows at 5:1 put the final row's top at 30 pixels",
            "2;1", solid_rows(3, 2), 1},
        {"ten sixel rows at 1:1 put the final row's top at 54 pixels",
            "9;1", solid_rows(3, 10), 2},
        {"two sixel rows at 2:1 put the final row's top at 12 pixels",
            "0;1", solid_rows(3, 2), 0},
        {"a trailing graphics new line counts: 2:1 rows end at 24 pixels",
            "0;1", solid_rows(3, 2) + '-', 1},
    };

    for (const Cursor_case& cursor_case : cases) {
        term::Terminal_screen_model model = make_model(20, 30);
        model.ingest(cursor_to(2, 4) + sixel(cursor_case.parameters, cursor_case.data));
        ok &= check(model.cursor_position().row    == 2 + cursor_case.expected_row &&
                model.cursor_position().column == 4,
            cursor_case.name);
    }

    // A cursor that does not move keeps a pending wrap; one that moves drops it.
    term::Terminal_screen_model wrap_model = make_model(20, 10);
    wrap_model.ingest(QByteArray("0123456789") + sixel("9;1", solid_rows(3, 1)) + "X");
    ok &= check(wrap_model.row_text(1) == QStringLiteral("X"),
        "an image that leaves the cursor in place keeps the pending wrap");
    term::Terminal_screen_model moved_model = make_model(20, 10);
    moved_model.ingest(QByteArray("0123456789") + sixel("0;1", solid_rows(3, 3)) + "X");
    ok &= check(moved_model.cursor_position().row == 1 &&
            moved_model.row_text(1).endsWith(QStringLiteral("X")) &&
            moved_model.row_text(2).isEmpty(),
        "an image that moves the cursor drops the pending wrap");

    return ok;
}

bool test_images_scroll_the_region_into_history()
{
    bool ok = true;

    // Six 20-pixel bands from the bottom row of a three-row screen: the region
    // scrolls five times, and the first three bands reach history.
    const QByteArray data = solid_rows(12, 20);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);
    term::Terminal_screen_model model = make_model(3, 10);
    model.ingest(QByteArray("top\r\nmid\r\nbottom\x1b[1;1H\x1b[3;1H") + sixel("9;1", data));
    ok &= check(model.scrollback_size() == 5,
        "an image taller than the screen scrolls the screen into history");
    ok &= check(model.row_text(0).isEmpty() && model.cursor_position().row == 2,
        "the cursor ends on the row of the final sixel row's top");
    for (int band = 0; band < 3; ++band) {
        ok &= check(slice_equals_band(
                model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 2 + band),
                image.raster,
                band * 20,
                12,
                0),
            "bands scrolled off the screen reach history with their rows");
    }
    for (int band = 3; band < 6; ++band) {
        ok &= check(slice_equals_band(slice_at(model, band - 3), image.raster, band * 20, 12, 0),
            "the last bands stay on the screen");
    }

    // Inside DECSTBM the region scrolls and nothing reaches history; the
    // ConPTY cursor-sync gate matches these scroll counts with OpenConsole.
    term::Terminal_screen_model region_model = make_model(6, 10);
    region_model.ingest(
        QByteArray("r0\r\nr1\r\nr2\r\nr3\r\nr4\r\nr5\x1b[2;4r\x1b[4;1H") + sixel("9;1", data));
    ok &= check(region_model.scrollback_size() == 0 &&
            region_model.row_text(0) == QStringLiteral("r0") &&
            region_model.row_text(4) == QStringLiteral("r4") &&
            region_model.row_text(5) == QStringLiteral("r5"),
        "an image inside a scroll region scrolls only the region, into no history");
    ok &= check(region_model.cursor_position().row == 3,
        "the cursor ends inside the region, on the final sixel row's top");
    ok &= check(slice_equals_band(slice_at(region_model, 3), image.raster, 100, 12, 0),
        "the region's last row holds the image's last band");

    // An image that would start below the bottom margin is dropped, as
    // OpenConsole drops it (checked by the cursor-sync gate).
    term::Terminal_screen_model below_model = make_model(6, 10);
    below_model.ingest(QByteArray("\x1b[2;4r\x1b[6;3H") + sixel("9;1", solid_rows(12, 4)));
    ok &= check(below_model.cursor_position().row    == 5 &&
            below_model.cursor_position().column == 2 &&
            slice_at(below_model, 5) == nullptr,
        "an image starting below the bottom margin is not placed and leaves the cursor");

    // An image starting above the top margin keeps its bands above the region
    // where they are drawn and scrolls only the region; the cursor-sync gate
    // matches the cursor this leaves with OpenConsole's. Twelve bands from
    // row 1 over the region of rows 4 to 7 scroll it five times.
    const QByteArray above_data = solid_rows(12, 40);
    const term::Screen_sixel_image_mutation above_image = decoded_image("9;1", above_data);
    term::Terminal_screen_model above_model = make_model(10, 10);
    above_model.ingest(QByteArray("\x1b[5;8r") + cursor_to(1, 0) + sixel("9;1", above_data));
    bool above_bands_placed = slice_at(above_model, 0) == nullptr &&
        slice_at(above_model, 8) == nullptr && slice_at(above_model, 9) == nullptr;
    for (int row = 1; row <= 7; ++row) {
        const int band = row <= 3 ? row - 1 : row + 4;
        above_bands_placed = above_bands_placed &&
            slice_equals_band(slice_at(above_model, row), above_image.raster, band * 20, 12, 0);
    }
    ok &= check(above_bands_placed && above_model.scrollback_size() == 0,
        "an image from above the region keeps its upper bands and scrolls only the region");
    ok &= check(above_model.cursor_position().row == 7,
        "the cursor ends on the region row of the final sixel row's top");

    return ok;
}

bool test_sixel_display_mode()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(5, 10);
    std::vector<term::Terminal_reply> replies = replies_in(model.ingest("\x1b[?80$p"));
    ok &= check(replies.size() == 1U && replies[0].wire_bytes == QByteArray("\x1b[?80;2$y"),
        "DECRQM reports DECSDM reset by default");

    model.ingest(QByteArray("\x1b[?80h\x1b[2;4r\x1b[?6h\x1b[2;3H"));
    replies = replies_in(model.ingest("\x1b[?80$p"));
    ok &= check(replies.size() == 1U && replies[0].wire_bytes == QByteArray("\x1b[?80;1$y"),
        "DECRQM reports DECSDM set");

    const term::terminal_grid_position_t cursor_before = model.cursor_position();
    const QByteArray data = solid_rows(12, 20);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);
    model.ingest(sixel("9;1", data));
    for (int row = 0; row < 5; ++row) {
        ok &= check(slice_equals_band(slice_at(model, row), image.raster, row * 20, 12, 0),
            "DECSDM places the image at the page home whatever the margins and origin mode");
    }
    ok &= check(model.scrollback_size() == 0,
        "DECSDM clips an image at the bottom of the page instead of scrolling");
    ok &= check(model.cursor_position().row    == cursor_before.row &&
            model.cursor_position().column == cursor_before.column,
        "DECSDM leaves the cursor where it was");

    model.ingest(QByteArray("\x1b[?80l"));
    replies = replies_in(model.ingest("\x1b[?80$p"));
    ok &= check(replies.size() == 1U && replies[0].wire_bytes == QByteArray("\x1b[?80;2$y"),
        "DECRQM reports DECSDM reset again");

    replies = replies_in(model.ingest("\x1b[?1070$p"));
    ok &= check(replies.size() == 1U && replies[0].wire_bytes == QByteArray("\x1b[?1070;3$y"),
        "DECRQM reports private color registers as permanently set");
    const term::Terminal_screen_model_result set_result = model.ingest("\x1b[?1070l");
    ok &= check(diagnostics_in(set_result).size() == 1U &&
            diagnostics_in(set_result)[0].code == term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "setting private color registers is diagnosed as unsupported");
    replies = replies_in(model.ingest("\x1b[?1070$p"));
    ok &= check(replies.size() == 1U && replies[0].wire_bytes == QByteArray("\x1b[?1070;3$y"),
        "private color registers stay set");

    return ok;
}

bool test_aspect_ratios_clamp_to_one_region()
{
    bool ok = true;

    // D5, as OpenConsole: a sixel row is at most as tall as the scroll region,
    // or the page with DECSDM set, so a larger ratio places exactly what the
    // largest one that fits places: (rows x 20) / 6 on 20-pixel cells.
    const auto place = [](int rows, const QByteArray& setup, const QByteArray& aspect) {
        term::Terminal_screen_model model = make_model(rows, 12);
        model.ingest(setup + sixel("9;1", "\"" + aspect + ";1" + patterned_rows(3)));
        return model;
    };
    const auto same_placement = [](
        const term::Terminal_screen_model& left,
        const term::Terminal_screen_model& right,
        int                                rows)
    {
        bool same =
            left.scrollback_size()          == right.scrollback_size()          &&
            left.cursor_position().row      == right.cursor_position().row      &&
            left.cursor_position().column   == right.cursor_position().column;
        bool drawn = false;
        for (int row = 0; same && row < left.scrollback_size() + rows; ++row) {
            const auto l = left.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, row);
            const auto r = right.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, row);
            drawn = drawn || l != nullptr;
            same  = (l == nullptr) == (r == nullptr) &&
                (l == nullptr || (l->first_column == r->first_column && l->pixels == r->pixels));
        }
        return same && drawn;
    };

    ok &= check(same_placement(place(10, "", "40"), place(10, "", "33"), 10),
        "a ratio above the screen's clamp places what the clamped ratio places, history included");
    ok &= check(same_placement(
            place(10, "\x1b[3;6r\x1b[3;1H", "20"),
            place(10, "\x1b[3;6r\x1b[3;1H", "13"),
            10),
        "a scroll region clamps the ratio to its own height");
    const QByteArray display_mode("\x1b[?80h\x1b[2;4r");
    ok &= check(same_placement(place(5, display_mode, "30"), place(5, display_mode, "16"), 5) &&
            !same_placement(place(5, display_mode, "16"), place(5, display_mode, "15"), 5),
        "with DECSDM set the page height, not the region, clamps the ratio");

    return ok;
}

bool test_images_erase_the_text_they_cover()
{
    bool ok = true;

    // "ab" plain, "cd" red, "ef" linked, "g", a wide glyph, "j".
    term::Terminal_screen_model model = make_model(3, 12);
    model.ingest(QByteArray(
        "ab\x1b[31mcd\x1b[m\x1b]8;;https://example.test/\x1b\\ef\x1b]8;;\x1b\\g"
        "\xe7\x95\x8cj"));
    const std::uint64_t generation_before =
        model.retained_line_provenance_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            active_row(model, 0)).content_generation;

    // Columns 2 and 3 drawn, column 4 left transparent, columns 5 to 7 drawn,
    // column 7 by a single pixel column.
    model.ingest(cursor_to(0, 2) + sixel("9;1", "#1;2;100;0;0#1!20~!10?#1!21~"));

    ok &= check(model.row_text(0) == QStringLiteral("ab  e    j"),
        "covered cells lose their text, a transparent column keeps it");
    ok &= check(model.retained_line_provenance_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            active_row(model, 0)).content_generation > generation_before,
        "erasing text under an image advances the row's content generation");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    const term::Terminal_render_cell* red_cell    = snapshot_cell(snapshot, 0, 2);
    const term::Terminal_render_cell* linked_cell = snapshot_cell(snapshot, 0, 4);
    ok &= check(red_cell != nullptr && red_cell->style_id != term::k_default_terminal_style_id,
        "a covered cell keeps its style (provisional)");
    ok &= check(snapshot_cell(snapshot, 0, 5) == nullptr,
        "a covered linked cell loses its hyperlink (provisional)");
    ok &= check(linked_cell != nullptr && linked_cell->hyperlink_id != term::k_no_terminal_hyperlink_id,
        "an uncovered linked cell keeps its hyperlink");
    ok &= check(snapshot_cell(snapshot, 0, 8) == nullptr,
        "a wide glyph with one covered cell is cleared whole");

    return ok;
}

bool test_images_on_one_row_composite()
{
    bool ok = true;

    const QByteArray first_data  = solid_rows(30, 1);
    const QByteArray second_data = "#2;2;0;100;0#2!5?!35~-#2!40~";
    const term::Screen_sixel_image_mutation first  = decoded_image("9;1", first_data);
    const term::Screen_sixel_image_mutation second = decoded_image("9;1", second_data);

    term::Terminal_screen_model model = make_model(3, 20);
    model.ingest(sixel("9;1", first_data));
    const std::uint64_t first_revision = slice_at(model, 0)->revision;
    model.ingest(cursor_to(0, 2) + sixel("9;1", second_data));

    // Reference: the second image's drawn pixels over the first, over the
    // union of their columns.
    QImage expected(60, 12, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(0U);
    for (int y = 0; y < first.raster.height(); ++y) {
        std::memcpy(expected.scanLine(y), first.raster.constScanLine(y), 30U * 4U);
    }
    for (int y = 0; y < second.raster.height(); ++y) {
        const auto* source = reinterpret_cast<const std::uint32_t*>(second.raster.constScanLine(y));
        auto*       target = reinterpret_cast<std::uint32_t*>(expected.scanLine(y)) + 20;
        for (int x = 0; x < second.raster.width(); ++x) {
            if (source[x] != 0U) {
                target[x] = source[x];
            }
        }
    }

    const std::shared_ptr<const term::Terminal_image_slice> slice = slice_at(model, 0);
    ok &= check(slice != nullptr &&
            slice->first_column == 0 &&
            slice->cell_pixel_size == k_cell &&
            slice->pixels == expected,
        "a second image on a row composites over the first into one slice");
    ok &= check(slice != nullptr && slice->revision > first_revision,
        "the composite is a new slice with a new revision");

    // A row's image keeps one cell size: an image placed on another cell is
    // resampled to the new one first (pixel values provisional).
    term::Terminal_screen_model resized = make_model(3, 20);
    resized.ingest(sixel("9;1", first_data));
    resized.set_cell_pixel_size({5, 10});
    resized.ingest(cursor_to(0, 8) + sixel("9;1", solid_rows(10, 1)));
    const std::shared_ptr<const term::Terminal_image_slice> mixed = slice_at(resized, 0);
    ok &= check(mixed != nullptr &&
            mixed->cell_pixel_size == term::terminal_cell_pixel_size_t{5, 10} &&
            mixed->first_column == 0 &&
            mixed->pixels.width() == 50 &&
            mixed->pixels.height() == 6,
        "a composite of images on different cells is kept at the newest cell");

    return ok;
}

bool test_images_without_geometry_or_pixels()
{
    bool ok = true;

    // No cell pixel size: nothing to place against (provisional).
    term::Terminal_screen_model headless = make_model(3, 10, std::nullopt);
    const term::Terminal_screen_model_result result =
        headless.ingest(QByteArray("ab") + sixel("0;1", solid_rows(4, 8)));
    const std::vector<term::Parser_payload_diagnostic> diagnostics = diagnostics_in(result);
    ok &= check(diagnostics.size() == 1U &&
            diagnostics[0].code == term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE &&
            diagnostics[0].family == term::Parser_sequence_family::DCS,
        "an image without a cell pixel size is one unsupported DCS diagnostic");
    ok &= check(headless.cursor_position().row == 0 && headless.cursor_position().column == 2 &&
            headless.scrollback_size() == 0 &&
            headless.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 0) == nullptr,
        "an image without a cell pixel size places nothing and moves nothing");

    // Over the decoded-size cap (1 MiB ring: 131072 bytes) the image keeps
    // its geometry: it scrolls and moves the cursor but stores no pixels. Of
    // its six scrolls the sixth, of an already blank screen, is skipped (D5).
    term::Terminal_screen_model capped = make_model(5, 30, k_cell, 100, 1024U * 1024U);
    const term::Terminal_screen_model_result capped_result =
        capped.ingest(sixel("9;1", solid_rows(200, 34)));
    ok &= check(diagnostics_in(capped_result).size() == 1U &&
            diagnostics_in(capped_result)[0].code ==
                term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        "an image over the decoded-size cap is diagnosed once");
    ok &= check(capped.scrollback_size() == 5 && capped.cursor_position().row == 3,
        "an image over the cap still scrolls and moves the cursor");
    bool capped_has_slice = false;
    for (int row = 0; row < capped.scrollback_size() + 5; ++row) {
        capped_has_slice = capped_has_slice ||
            capped.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, row) != nullptr;
    }
    ok &= check(!capped_has_slice, "an image over the cap stores no pixels anywhere");

    // An image that draws nothing still moves the cursor by its sixel rows.
    term::Terminal_screen_model empty = make_model(5, 30);
    empty.ingest(cursor_to(1, 3) + sixel("0;1", "--"));
    ok &= check(empty.cursor_position().row == 2 && empty.cursor_position().column == 3 &&
            slice_at(empty, 1) == nullptr && slice_at(empty, 2) == nullptr,
        "an empty image moves the cursor by its graphics new lines and stores nothing");

    // Geometry far past the screen is bounded work even with the product's
    // unlimited scrollback (D5): a graphics new line moves at most one region,
    // and the scrolls past the pixels stop once the region is blank. Before
    // the clamp these 1016 bytes asked for 9.8 million scrolls.
    term::Terminal_screen_model runaway =
        make_model(24, 10, k_cell, std::numeric_limits<int>::max());
    const term::Terminal_screen_model_result runaway_result = runaway.ingest(
        QByteArray("keep\r\n") + sixel("9;1", QByteArray("\"32767;1") + QByteArray(1000, '-')));
    int appended_rows = 0;
    for (const term::terminal_backing_delta_t& delta : runaway_result.backing_deltas) {
        appended_rows += delta.appended_scrollback_rows;
    }
    ok &= check(appended_rows == 24 && runaway.scrollback_size() == 24,
        "a runaway image height scrolls one region into history");
    ok &= check(history_row_text(runaway, 0) == QStringLiteral("keep") &&
            runaway.visible_text().trimmed().isEmpty() &&
            runaway.cursor_position().row == 0,
        "a runaway image height leaves a blank screen with the cursor at its top");

    return ok;
}

bool test_image_rows_move_and_die_with_their_rows()
{
    bool ok = true;

    const QByteArray image = sixel("9;1", solid_rows(12, 1));

    term::Terminal_screen_model model = make_model(4, 10);
    model.ingest(cursor_to(1, 0) + image);
    const std::shared_ptr<const term::Terminal_image_slice> placed = slice_at(model, 1);
    ok &= check(placed != nullptr, "the moving-row fixture places one slice");

    model.ingest("\x1b[T");
    ok &= check(slice_at(model, 2) == placed && slice_at(model, 1) == nullptr,
        "SD moves the slice down with its row");
    model.ingest("\x1b[S");
    ok &= check(slice_at(model, 1) == placed && slice_at(model, 2) == nullptr,
        "SU moves the slice up with its row");
    model.ingest(QByteArray("\x1b[1;1H\x1b[L"));
    ok &= check(slice_at(model, 2) == placed && slice_at(model, 0) == nullptr,
        "IL moves the slice down with its row");
    model.ingest(QByteArray("\x1b[1;1H\x1b[M"));
    ok &= check(slice_at(model, 1) == placed && slice_at(model, 3) == nullptr,
        "DL moves the slice up with its row");
    model.ingest(QByteArray("\x1b[2;1H\x1b[M"));
    bool any_slice = false;
    for (int row = 0; row < 4; ++row) {
        any_slice = any_slice || slice_at(model, row) != nullptr;
    }
    ok &= check(!any_slice, "a deleted row takes its slice with it");

    // A slice that scrolls into history decodes as the live slice it was.
    term::Terminal_screen_model history_model = make_model(3, 10);
    history_model.ingest(image);
    const std::shared_ptr<const term::Terminal_image_slice> live = slice_at(history_model, 0);
    history_model.ingest("\r\n\r\n\r\n");
    ok &= check(history_model.scrollback_size() == 1 &&
            slices_equal(
                history_model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 0),
                live),
        "a history row decodes to the slice its live row held");
    history_model.ingest("\x1b[3J");
    ok &= check(history_model.scrollback_size() == 0,
        "ED3 clears history, image rows included");

    // The alternate screen places images the same way, never feeds history,
    // and drops its images when it is cleared; primary images stay.
    term::Terminal_screen_model alternate = make_model(3, 10);
    alternate.ingest(image);
    const std::shared_ptr<const term::Terminal_image_slice> primary_slice = slice_at(alternate, 0);
    alternate.ingest(QByteArray("\x1b[?1049h\x1b[3;1H") + sixel("9;1", solid_rows(12, 10)));
    ok &= check(alternate.scrollback_size() == 0,
        "scrolls on the alternate screen never feed history");
    ok &= check(alternate.image_slice_for_testing(term::Terminal_buffer_id::ALTERNATE, 2) != nullptr,
        "the alternate screen holds its own image rows");
    ok &= check(alternate.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 0) == primary_slice,
        "primary image rows are untouched while the alternate screen is active");
    alternate.ingest("\x1b[?1049l");
    ok &= check(alternate.image_slice_for_testing(term::Terminal_buffer_id::ALTERNATE, 2) == nullptr &&
            slice_at(alternate, 0) == primary_slice,
        "leaving a cleared alternate screen drops its images and keeps the primary's");

    return ok;
}

bool test_placement_marks_rows_dirty()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(6, 10);
    model.ingest("\x1b[3;1H");
    const std::uint64_t generation_before =
        model.retained_line_provenance_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            active_row(model, 3)).content_generation;
    const term::Terminal_screen_model_result result =
        model.ingest(sixel("9;1", solid_rows(12, 6)));
    ok &= check(result.terminal_content_changed, "placing an image changes terminal content");
    ok &= check(std::find(result.dirty_rows.begin(), result.dirty_rows.end(), 2) != result.dirty_rows.end() &&
            std::find(result.dirty_rows.begin(), result.dirty_rows.end(), 3) != result.dirty_rows.end(),
        "every row an image lands on is dirty");
    ok &= check(model.retained_line_provenance_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            active_row(model, 3)).content_generation == generation_before,
        "an image over no text leaves the row's content generation alone");

    return ok;
}

// The record limit is a eighth of the ring: 131072 bytes at 1 MiB. A 1638 x 20
// image is 131040 bytes and passes the decoder cap, but its row record adds a
// header, the section header, the cells and the ring framing.
bool test_oversized_image_rows_keep_their_text_in_history()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 170, k_cell, 100, 1024U * 1024U);
    const term::Terminal_screen_model_result result = model.ingest(
        cursor_to(0, 165) + QByteArray("KEPT") + cursor_to(0, 0) + sixel("1;0", "\"1;1;1638;20"));
    ok &= check(diagnostics_in(result).empty(), "the near-cap image passes the decoder cap");
    ok &= check(slice_at(model, 0) != nullptr && slice_at(model, 0)->pixels.width() == 1638,
        "the near-cap image is placed on its row");

    model.ingest("\r\n\r\n\r\n\r\n");
    ok &= check(model.scrollback_size() == 1, "the image row scrolls into history");
    ok &= check(history_row_text(model, 0) == QStringLiteral("KEPT"),
        "a row whose image makes its record too large keeps its text in history");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 0) == nullptr,
        "and drops its image");

    return ok;
}

bool test_capacity_shrink_keeps_text_rows_around_an_oversized_image()
{
    bool ok = true;

    // 16 x 32 cells: an 1400 x 32 image row is 179200 pixel bytes, over the
    // 131072 byte record limit of a 1 MiB ring; a 100 x 32 one is not.
    const term::terminal_cell_pixel_size_t cell{16, 32};
    term::Terminal_screen_model model = make_model(5, 90, cell, 100);
    model.ingest(QByteArray("older-0\r\nolder-1\r\n"));
    model.ingest(cursor_to(2, 88) + QByteArray("IM") + cursor_to(2, 0) + sixel("1;0", "\"1;1;1400;32"));
    model.ingest(cursor_to(3, 10) + QByteArray("small") + cursor_to(3, 0) + sixel("1;0", "\"1;1;100;32"));
    model.ingest(cursor_to(4, 0) + QByteArray("newer\r\n\r\n\r\n\r\n\r\n"));

    ok &= check(model.scrollback_size() == 5, "the shrink fixture scrolls five rows into history");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 2) != nullptr &&
            model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 3) != nullptr,
        "both image rows reach history with their images");

    model.set_retained_history_capacity_bytes(1024U * 1024U);
    ok &= check(model.scrollback_size() == 5,
        "shrinking below an image row's record keeps every row");
    ok &= check(history_row_text(model, 0) == QStringLiteral("older-0") &&
            history_row_text(model, 1) == QStringLiteral("older-1") &&
            history_row_text(model, 2) == QStringLiteral("IM") &&
            history_row_text(model, 3) == QStringLiteral("small") &&
            history_row_text(model, 4) == QStringLiteral("newer"),
        "rows older than the oversized image row keep their text");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 2) == nullptr,
        "the image that no longer fits a record is dropped");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 3) != nullptr,
        "an image that still fits keeps its pixels");

    for (int row = 0; row < model.scrollback_size(); ++row) {
        const std::optional<term::terminal_history_handle_t> handle =
            model.retained_history_handle_at_logical_row(term::Terminal_buffer_id::PRIMARY, row);
        const term::Terminal_retained_line_lookup_result lookup = handle.has_value()
            ? model.retained_line_lookup(term::Terminal_buffer_id::PRIMARY, *handle)
            : term::Terminal_retained_line_lookup_result{};
        ok &= check(lookup.resolution_status == term::Terminal_history_resolution_status::OK &&
                lookup.exact_match &&
                lookup.exact_logical_row == row,
            "each kept row's new handle resolves to that row");
    }

    return ok;
}

bool test_capacity_shrink_rebuilds_a_nearly_full_ring()
{
    bool ok = true;

    // A 2 MiB ring (record limit 256 KiB) filled past its capacity with 16 x
    // 32 cell rows: 1400 x 32 images (179200 pixel bytes, over the 128 KiB
    // limit of a 1 MiB ring), two 1000 x 32 images (128000 bytes, under it)
    // and a text row per round, each image row labeled beside its image.
    const term::terminal_cell_pixel_size_t cell{16, 32};
    term::Terminal_screen_model model = make_model(5, 100, cell, 5000, 2U * 1024U * 1024U);
    for (int round = 0; round < 8; ++round) {
        const QByteArray number = QByteArray::number(round);
        model.ingest(cursor_to(4, 0) + "text-" + number + "\r\n");
        model.ingest(cursor_to(4, 88) + "big-" + number + cursor_to(4, 0) +
            sixel("1;0", "\"1;1;1400;32") + "\r\n");
        for (const char* part : {"a", "b"}) {
            model.ingest(cursor_to(4, 64) + "small-" + number + '-' + part + cursor_to(4, 0) +
                sixel("1;0", "\"1;1;1000;32") + "\r\n");
        }
    }

    std::vector<QString> texts_before;
    for (int row = 0; row < model.scrollback_size(); ++row) {
        texts_before.push_back(history_row_text(model, row));
    }
    ok &= check(!texts_before.empty() && texts_before.front() != QStringLiteral("text-0"),
        "the 2 MiB ring has evicted its oldest rows before the shrink");

    model.set_retained_history_capacity_bytes(1024U * 1024U);
    const int kept = model.scrollback_size();
    ok &= check(kept > 0 && kept < static_cast<int>(texts_before.size()),
        "the shrink keeps as many of the newest rows as the new capacity holds");
    ok &= check(model.retained_history_diagnostics().retained_record_bytes <= 1024U * 1024U,
        "the kept records fit the new capacity");

    bool texts_kept = true;
    bool images_fit = true;
    for (int row = 0; row < kept; ++row) {
        const QString text = history_row_text(model, row);
        texts_kept = texts_kept &&
            text == texts_before[texts_before.size() - static_cast<std::size_t>(kept - row)];
        const std::shared_ptr<const term::Terminal_image_slice> slice =
            model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, row);
        images_fit = images_fit &&
            (text.startsWith(QStringLiteral("big-"))   ? slice == nullptr :
             text.startsWith(QStringLiteral("small-")) ? slice != nullptr :
                                                         slice == nullptr);

        const std::optional<term::terminal_history_handle_t> handle =
            model.retained_history_handle_at_logical_row(term::Terminal_buffer_id::PRIMARY, row);
        const term::Terminal_retained_line_lookup_result lookup = handle.has_value()
            ? model.retained_line_lookup(term::Terminal_buffer_id::PRIMARY, *handle)
            : term::Terminal_retained_line_lookup_result{};
        ok &= check(lookup.exact_match && lookup.exact_logical_row == row,
            "each row kept by a rebuild resolves to itself");
    }
    ok &= check(texts_kept, "the kept rows are the newest rows, in order, text intact");
    ok &= check(images_fit,
        "images over the new record limit are dropped and images under it are kept");

    const QString top_row = model.row_text(0).trimmed();
    model.ingest(cursor_to(4, 0) + "after-shrink\r\n");
    ok &= check(model.scrollback_size() == kept + 1 && history_row_text(model, kept) == top_row,
        "the rebuilt ring takes new rows");

    return ok;
}

term::Terminal_render_snapshot scrolled_back_snapshot(
    const term::Terminal_screen_model& model,
    int                                offset_from_tail)
{
    term::Terminal_render_snapshot_request request;
    request.sequence                  = 1U;
    request.viewport.active_buffer    = model.active_buffer_id();
    request.viewport.visible_rows     = model.grid_size().rows;
    request.viewport.scrollback_rows  = model.scrollback_size();
    request.viewport.offset_from_tail = offset_from_tail;
    request.viewport.follow_tail      = false;
    return model.render_snapshot(request);
}

bool test_capacity_shrink_signals_a_history_rewrite()
{
    bool ok = true;

    // Every row fits the smaller ring, but one image no longer fits a record:
    // the rebuild evicts nothing, yet it changes what a history row shows and
    // replaces every retained handle.
    const term::terminal_cell_pixel_size_t cell{16, 32};
    term::Terminal_screen_model model = make_model(5, 90, cell, 100);
    model.ingest(cursor_to(0, 88) + "IM" + cursor_to(0, 0) + sixel("1;0", "\"1;1;1400;32") +
        cursor_to(4, 0) + "\r\n");
    ok &= check(model.scrollback_size() == 1 &&
            term::render_snapshot_row_image(scrolled_back_snapshot(model, 1), 0) != nullptr,
        "a viewport scrolled back over the image row shows its image");

    const term::Terminal_screen_model_result result =
        model.set_retained_history_capacity_bytes(1024U * 1024U);
    const bool rewritten = std::any_of(
        result.backing_deltas.begin(),
        result.backing_deltas.end(),
        [](const term::terminal_backing_delta_t& delta) {
            return delta.kind == term::Terminal_backing_delta_kind::PRIMARY_HISTORY_REWRITTEN;
        });
    const bool evicted = std::any_of(
        result.backing_deltas.begin(),
        result.backing_deltas.end(),
        [](const term::terminal_backing_delta_t& delta) {
            return delta.kind == term::Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED;
        });
    ok &= check(model.scrollback_size() == 1 && rewritten && !evicted,
        "a rebuild that evicts nothing still reports that history was rewritten");
    ok &= check(result.terminal_content_changed && result.viewport_changed,
        "a rebuild that evicts nothing marks content and viewport changed");

    const term::Terminal_render_snapshot after = scrolled_back_snapshot(model, 1);
    ok &= check(term::render_snapshot_row_image(after, 0) == nullptr,
        "a viewport scrolled back over the row no longer shows the dropped image");
    ok &= check(history_row_text(model, 0) == QStringLiteral("IM"),
        "the rewritten row keeps its text");

    return ok;
}

bool test_capacity_decrease_drops_screen_images_over_the_cap()
{
    bool ok = true;

    // I5 on the screens, by A7's rule for history rows: a 1400 x 32 image row
    // is 179200 bytes, within the default cap and over the 131072 byte cap of
    // a 1 MiB ring, so the decrease drops it on both screens and the rows keep
    // their text; a 100 x 32 image stays.
    const term::terminal_cell_pixel_size_t cell{16, 32};
    const QByteArray large_image = sixel("1;0", "\"1;1;1400;32");
    term::Terminal_screen_model model = make_model(4, 90, cell);
    model.ingest(
        cursor_to(1, 88) + "ab" + cursor_to(1, 0) + large_image +
        cursor_to(2, 0) + sixel("1;0", "\"1;1;100;32") +
        "\x1b[?1049h" + cursor_to(0, 88) + "cd" + cursor_to(0, 0) + large_image);
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 1) != nullptr &&
            model.image_slice_for_testing(term::Terminal_buffer_id::ALTERNATE, 0) != nullptr,
        "the large images are placed under the default cap");

    const term::Terminal_screen_model_result result =
        model.set_retained_history_capacity_bytes(1024U * 1024U);
    const std::vector<term::Parser_payload_diagnostic> diagnostics = diagnostics_in(result);
    ok &= check(diagnostics.size() == 1U &&
            diagnostics[0].code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED &&
            diagnostics[0].family == term::Parser_sequence_family::DCS &&
            diagnostics[0].raw_payload_size == 179200U &&
            diagnostics[0].limit_bytes == term::terminal_history_ring_max_record_bytes(
                term::terminal_history_ring_aligned_capacity(1024U * 1024U)),
        "the dropped screen images are reported once, with the largest size and the cap");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::ALTERNATE, 0) == nullptr &&
            model.row_text(0).endsWith(QStringLiteral("cd")),
        "the active screen's row drops its image and keeps its text");
    ok &= check(result.terminal_content_changed &&
            std::find(result.dirty_rows.begin(), result.dirty_rows.end(), 0) != result.dirty_rows.end(),
        "the active screen's row is published");

    model.ingest("\x1b[?1049l");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 1) == nullptr &&
            model.row_text(1).endsWith(QStringLiteral("ab")),
        "the inactive screen's row drops its image and keeps its text");
    ok &= check(model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 2) != nullptr,
        "an image within the lower cap stays");

    return ok;
}

// One sixel row of `cells` ten-pixel blocks, each in its own color, so a
// moved or cleared block shows in the pixels.
QByteArray striped_cells(int cells)
{
    QByteArray data;
    for (int cell = 0; cell < cells; ++cell) {
        data += '#' + QByteArray::number(cell + 1) + ";2;" + QByteArray::number(cell * 10) +
            ";" + QByteArray::number(100 - cell * 10) + ";50";
    }
    for (int cell = 0; cell < cells; ++cell) {
        data += '#' + QByteArray::number(cell + 1) + "!10~";
    }
    return data;
}

// The decoder's raster with the pixels of cells [first_cell, end_cell)
// cleared, cells counted from the image's first column.
QImage raster_without_cells(const QImage& raster, int first_cell, int end_cell)
{
    QImage expected = raster.copy();
    for (int y = 0; y < expected.height(); ++y) {
        auto* line = reinterpret_cast<std::uint32_t*>(expected.scanLine(y));
        std::fill(
            line + first_cell * k_cell.width,
            line + std::min(expected.width(), end_cell * k_cell.width),
            0U);
    }
    return expected;
}

struct Cell_move
{
    int source_first_cell = 0;
    int source_end_cell   = 0;
    int target_first_cell = 0;
};

// The decoder's raster rebuilt from whole-cell blocks moved to new cells.
QImage raster_with_moved_cells(const QImage& raster, int width, const std::vector<Cell_move>& moves)
{
    QImage expected(width, raster.height(), QImage::Format_RGBA8888_Premultiplied);
    expected.fill(0U);
    for (const Cell_move& move : moves) {
        const std::size_t bytes =
            static_cast<std::size_t>((move.source_end_cell - move.source_first_cell) * k_cell.width) * 4U;
        for (int y = 0; y < expected.height(); ++y) {
            std::memcpy(
                expected.scanLine(y) + move.target_first_cell * k_cell.width * 4,
                raster.constScanLine(y) + move.source_first_cell * k_cell.width * 4,
                bytes);
        }
    }
    return expected;
}

// Empty when history has no such row, so no wrap state matches a missing row.
std::optional<term::Terminal_retained_row_wrap_state> history_wrap_state(
    const term::Terminal_screen_model& model,
    int                                row)
{
    const std::optional<term::terminal_retained_row_record_metadata_t> metadata =
        model.retained_row_record_metadata_for_testing(term::Terminal_buffer_id::PRIMARY, row);
    if (!metadata.has_value()) {
        return std::nullopt;
    }
    return metadata->wrap_state;
}

bool test_text_writes_and_erases_clear_image_cells()
{
    bool ok = true;

    const QByteArray data = striped_cells(10);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);
    ok &= check(image.raster.width() == 100 && image.raster.height() == 6,
        "the striped reference image decodes to 100 x 6 pixels");

    struct Edit_case
    {
        const char* name;
        QByteArray  edit;
        int         first_cell;
        int         end_cell;
    };

    const std::vector<Edit_case> cases = {
        {"printing clears the image under the printed cells",
            cursor_to(0, 2) + "ab", 2, 4},
        {"a wide glyph clears the image under both its cells",
            cursor_to(0, 4) + "\xe7\x95\x8c", 4, 6},
        {"EL 0 clears the image from the cursor on, over cells with no text",
            cursor_to(0, 6) + "\x1b[K", 6, 10},
        {"EL 1 clears the image up to the cursor",
            cursor_to(0, 3) + "\x1b[1K", 0, 4},
        {"ECH clears the image under the erased cells",
            cursor_to(0, 4) + "\x1b[3X", 4, 7},
        {"ED 0 clears the image from the cursor on",
            cursor_to(0, 7) + "\x1b[J", 7, 10},
        {"ED 1 clears the image up to the cursor",
            cursor_to(0, 2) + "\x1b[1J", 0, 3},
    };

    for (const Edit_case& edit_case : cases) {
        term::Terminal_screen_model model = make_model(4, 20);
        model.ingest(sixel("9;1", data));
        const std::uint64_t revision_before = slice_at(model, 0)->revision;
        model.ingest(edit_case.edit);
        const std::shared_ptr<const term::Terminal_image_slice> slice = slice_at(model, 0);
        ok &= check(slice != nullptr &&
                slice->first_column == 0 &&
                slice->pixels == raster_without_cells(
                    image.raster,
                    edit_case.first_cell,
                    edit_case.end_cell),
            edit_case.name);
        ok &= check(slice != nullptr && slice->revision > revision_before,
            "an edited row image is a new slice with a new revision");
    }

    const std::vector<std::pair<const char*, QByteArray>> clearing_edits = {
        {"EL 2 drops the row image", cursor_to(0, 5) + "\x1b[2K"},
        {"ED 2 drops the screen's images", cursor_to(0, 5) + "\x1b[2J"},
        {"printing over every image cell drops the row image",
            cursor_to(0, 0) + "0123456789"},
        {"edits that together clear every image cell drop the row image",
            cursor_to(0, 4) + "\x1b[1K" + cursor_to(0, 5) + "\x1b[5X" + cursor_to(0, 4) + "x"},
    };
    for (const auto& [name, edit] : clearing_edits) {
        term::Terminal_screen_model model = make_model(4, 20);
        model.ingest(sixel("9;1", data));
        model.ingest(edit);
        ok &= check(slice_at(model, 0) == nullptr, name);
    }

    // ED 0 erases whole rows below the cursor, and their images with them.
    term::Terminal_screen_model rows_below = make_model(4, 20);
    rows_below.ingest(cursor_to(2, 0) + sixel("9;1", data) + cursor_to(0, 0) + "\x1b[J");
    ok &= check(slice_at(rows_below, 2) == nullptr,
        "ED 0 drops the images of the rows below the cursor");

    return ok;
}

bool test_ich_and_dch_move_image_columns()
{
    bool ok = true;

    const QByteArray data = striped_cells(10);
    const term::Screen_sixel_image_mutation image = decoded_image("9;1", data);

    // ICH 2 at column 3 of an 11-column row: cells 3 to 8 move to 5 to 10,
    // and cell 9 is pushed past the right margin.
    term::Terminal_screen_model inserted = make_model(3, 11);
    inserted.ingest(sixel("9;1", data) + cursor_to(0, 3) + "\x1b[2@");
    const std::shared_ptr<const term::Terminal_image_slice> inserted_slice = slice_at(inserted, 0);
    ok &= check(inserted_slice != nullptr &&
            inserted_slice->first_column == 0 &&
            inserted_slice->pixels == raster_with_moved_cells(image.raster, 110, {{0, 3, 0}, {3, 9, 5}}),
        "ICH moves the image cells with the text cells and loses those pushed past the margin");

    // DCH 2 at column 3: cells 3 and 4 go, and cells 5 to 9 move to 3 to 7.
    term::Terminal_screen_model deleted = make_model(3, 20);
    deleted.ingest(sixel("9;1", data) + cursor_to(0, 3) + "\x1b[2P");
    const std::shared_ptr<const term::Terminal_image_slice> deleted_slice = slice_at(deleted, 0);
    ok &= check(deleted_slice != nullptr &&
            deleted_slice->first_column == 0 &&
            deleted_slice->pixels == raster_with_moved_cells(image.raster, 80, {{0, 3, 0}, {5, 10, 3}}),
        "DCH deletes the image cells with the text cells and moves the rest left");

    term::Terminal_screen_model shifted = make_model(3, 20);
    shifted.ingest(sixel("9;1", data) + cursor_to(0, 0) + "\x1b[3@");
    ok &= check(slice_at(shifted, 0) != nullptr &&
            slice_at(shifted, 0)->first_column == 3 &&
            slice_at(shifted, 0)->pixels == raster_without_cells(image.raster, 10, 10),
        "ICH at the image's first cell moves the whole image right");

    term::Terminal_screen_model removed = make_model(3, 20);
    removed.ingest(sixel("9;1", data) + cursor_to(0, 0) + "\x1b[10P");
    ok &= check(slice_at(removed, 0) == nullptr, "DCH over every image cell drops the row image");

    term::Terminal_screen_model untouched = make_model(3, 20);
    untouched.ingest(sixel("9;1", data));
    const std::shared_ptr<const term::Terminal_image_slice> placed = slice_at(untouched, 0);
    untouched.ingest(cursor_to(0, 12) + "\x1b[2@\x1b[2P");
    ok &= check(slice_at(untouched, 0) == placed,
        "ICH and DCH right of the image leave the row image as it was");

    return ok;
}

bool test_image_rows_start_their_own_logical_lines()
{
    bool ok = true;

    // Thirty characters on a ten-column screen: rows 0 and 1 wrap softly.
    const QByteArray text("0123456789abcdefghijklmnopqrst");
    const QByteArray push_into_history = cursor_to(3, 0) + "\n\n\n\n";

    term::Terminal_screen_model control = make_model(4, 10);
    control.ingest(text + push_into_history);
    ok &= check(history_wrap_state(control, 0) == term::Terminal_retained_row_wrap_state::SOFT_WRAP &&
            history_wrap_state(control, 1) == term::Terminal_retained_row_wrap_state::SOFT_WRAP,
        "without an image the wrapped rows stay soft");

    // D2: an image placed on row 1 hard-terminates the wrap into it and the
    // wrap out of it.
    term::Terminal_screen_model placed = make_model(4, 10);
    placed.ingest(text + cursor_to(1, 9) + sixel("9;1", solid_rows(3, 1)) + push_into_history);
    ok &= check(history_wrap_state(placed, 0) == term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "placing an image hard-terminates the soft wrap into its row");
    ok &= check(history_wrap_state(placed, 1) == term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "placing an image hard-terminates the soft wrap out of its row");

    // A row that shows an image starts a logical line, so text wrapping onto
    // it later wraps hard as well.
    term::Terminal_screen_model wrapped_onto = make_model(4, 10);
    wrapped_onto.ingest(
        cursor_to(1, 7) + sixel("9;1", solid_rows(3, 1)) + cursor_to(0, 0) + "0123456789abcde" +
        push_into_history);
    ok &= check(history_wrap_state(wrapped_onto, 0) == term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "text wrapping onto an image row wraps hard");
    ok &= check(wrapped_onto.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 1) != nullptr,
        "the wrapped text leaves the image cells it does not reach");

    // D2 also keeps text printed after an image on a separate logical line
    // when it runs past that row's margin. Reflow must not join it back.
    term::Terminal_screen_model wrapped_off = make_model(4, 10);
    wrapped_off.ingest(cursor_to(1, 0) + sixel("9;1", solid_rows(3, 1)));
    const std::shared_ptr<const term::Terminal_image_slice> placed_slice = slice_at(wrapped_off, 1);
    wrapped_off.ingest(cursor_to(1, 5) + "vwxyz12");
    wrapped_off.resize(term::terminal_grid_size_t{4, 20});
    ok &= check(wrapped_off.row_text(1) == QStringLiteral("     vwxyz") &&
            wrapped_off.row_text(2) == QStringLiteral("12"),
        "widening leaves later text beyond an image row on its own logical line");
    wrapped_off.resize(term::terminal_grid_size_t{4, 10});
    ok &= check(slice_at(wrapped_off, 1) == placed_slice,
        "wrapping off an image row and resizing keeps its original pixels");
    wrapped_off.ingest(push_into_history);
    ok &= check(history_wrap_state(wrapped_off, 1) == term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "text wrapping off an image row keeps a hard boundary in history");
    ok &= check(slices_equal(
            wrapped_off.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 1),
            placed_slice),
        "the hard-bounded image row reaches history without losing pixels");

    // At the bottom margin the source row scrolls upward while the text
    // continues onto a fresh row. Its outgoing boundary still belongs to it.
    term::Terminal_screen_model scrolled_off = make_model(4, 10);
    scrolled_off.ingest(
        cursor_to(3, 0) + sixel("9;1", solid_rows(3, 1)) + cursor_to(3, 5) + "vwxyz12" +
        push_into_history);
    ok &= check(scrolled_off.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 3) != nullptr &&
            history_wrap_state(scrolled_off, 3) == term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "wrapping off an image at the bottom margin keeps its hard boundary after scrolling");

    return ok;
}

bool test_reflow_keeps_the_image_on_its_line_first_row()
{
    bool ok = true;

    // S2: an image stays on the first row of its logical line through a
    // reflow, and its columns never make continuation rows.
    term::Terminal_screen_model model = make_model(4, 20);
    model.ingest(sixel("9;1", striped_cells(10)) + cursor_to(0, 12) + "abcdefgh");
    const std::shared_ptr<const term::Terminal_image_slice> placed = slice_at(model, 0);

    model.resize(term::terminal_grid_size_t{4, 10});
    ok &= check(slice_at(model, 0) == placed && slice_at(model, 1) == nullptr,
        "narrowing keeps the image on the first row of its line");
    ok &= check(model.row_text(1) == QStringLiteral("  abcdefgh"),
        "narrowing wraps the row's text onto a continuation row");

    model.resize(term::terminal_grid_size_t{4, 20});
    ok &= check(slice_at(model, 0) == placed,
        "widening back restores the image unchanged");
    ok &= check(model.row_text(0) == QStringLiteral("            abcdefgh") &&
            model.row_text(1).isEmpty(),
        "widening back restores the row's text");

    term::Terminal_screen_model image_only = make_model(4, 20);
    image_only.ingest(sixel("9;1", solid_rows(150, 1)) + cursor_to(1, 0) + "next");
    const std::shared_ptr<const term::Terminal_image_slice> wide = slice_at(image_only, 0);
    image_only.resize(term::terminal_grid_size_t{4, 10});
    ok &= check(slice_at(image_only, 0) == wide &&
            image_only.row_text(1) == QStringLiteral("next"),
        "an image wider than the narrowed row makes no continuation rows");

    return ok;
}

bool test_recovered_history_rows_keep_their_images()
{
    bool ok = true;

    term::Terminal_screen_model_config config;
    config.grid_size                                = {4, 8};
    config.scrollback_limit                         = 8;
    config.cell_pixel_size                          = k_cell;
    config.recover_scrollback_from_primary_repaints = true;
    term::Terminal_screen_model model(config);

    const auto rows_stream = [](std::initializer_list<const char*> rows, bool cursor_hidden) {
        QByteArray stream = cursor_hidden ? QByteArray("\x1b[?25l") : QByteArray();
        int row = 1;
        for (const char* text : rows) {
            stream += "\x1b[" + QByteArray::number(row++) + ";1H" + text + "\x1b[K";
        }
        if (cursor_hidden) {
            stream += "\x1b[?25h";
        }
        return stream;
    };

    model.ingest(rows_stream({"aa", "bb", "cc", "dd"}, false));
    model.ingest(cursor_to(0, 4) + sixel("9;1", solid_rows(20, 1)));
    const std::shared_ptr<const term::Terminal_image_slice> placed = slice_at(model, 0);
    ok &= check(placed != nullptr, "the recovery fixture places an image on row 0");

    // The application repaints the screen shifted up by one row; recovery
    // keeps the row that left the top as history.
    model.ingest(rows_stream({"bb", "cc", "dd", "ee"}, true));
    ok &= check(model.scrollback_size() == 1 && history_row_text(model, 0) == QStringLiteral("aa"),
        "the repaint recovers the row that left the screen");
    ok &= check(slices_equal(
            model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 0),
            placed),
        "a recovered history row keeps the image it showed before the repaint");
    ok &= check(slice_at(model, 0) == nullptr,
        "the repainted screen row loses the image to the repaint's erase");

    return ok;
}

// The width and height of an XTSMGRAPHICS geometry reply, CSI ? 2 ; 0 ; w ; h S.
QSize reported_geometry(const std::vector<term::Terminal_reply>& replies)
{
    if (replies.size() != 1U || !replies[0].wire_bytes.startsWith("\x1b[?2;0;")) {
        return {};
    }
    const QByteArray&       wire_bytes = replies[0].wire_bytes;
    const QList<QByteArray> fields     = wire_bytes.mid(3, wire_bytes.size() - 4).split(';');
    return fields.size() == 4 ? QSize(fields[2].toInt(), fields[3].toInt()) : QSize();
}

// A client session as an encoder such as img2sixel drives it: it reads DA1 for
// attribute 4, XTSMGRAPHICS for the geometry and CSI 16 t for the cell, sends an
// image with raster attributes, color definitions, repeats, a graphics carriage
// return and a trailing graphics new line in the pieces a pty delivers, and the
// shell prints after it. Oracles: xterm ctlseqs (the replies), DEC ch. 14 (the
// sixel data, the image at the cursor scrolling the screen), V1 (the cursor on
// the row of the final sixel row's top, confirmed against OpenConsole by the
// ConPTY cursor-sync gate). The byte stream is hand-written.
bool test_encoder_session_end_to_end()
{
    bool ok = true;

    // Four rows of 40 columns of 10 x 20 pixel cells: a 400 x 80 pixel text area.
    term::Terminal_screen_model model = make_model(4, 40);
    model.ingest(QByteArray("$ img2sixel photo.png\r\n"));

    const term::Terminal_screen_model_result query_result =
        model.ingest(QByteArray("\x1b[c\x1b[?2;1;0S\x1b[16t"));
    const std::vector<term::Terminal_reply> replies = replies_in(query_result);
    ok &= check(diagnostics_in(query_result).empty() && replies.size() == 3U &&
            replies[0].wire_bytes == QByteArray("\x1b[?61;4c") &&
            replies[1].wire_bytes == QByteArray("\x1b[?2;0;400;80S") &&
            replies[2].wire_bytes == QByteArray("\x1b[6;20;10t"),
        "the encoder learns of sixel graphics, a 400 x 80 geometry and a 10 x 20 cell");

    // 30 x 60 pixels at 1:1 in ten sixel rows: the upper five red on the left
    // half and green on the right, the lower five blue with a green stripe at x
    // 12 to 17 drawn over the blue after a graphics carriage return.
    QByteArray data("\"1;1;30;60#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100");
    for (int row = 0; row < 5; ++row) {
        data += "#1!15~#2!15~-";
    }
    for (int row = 0; row < 5; ++row) {
        data += "#3!30~$!12?#2!6~-";
    }
    const QByteArray image_bytes = sixel("0;1;0", data);
    const term::Screen_sixel_image_mutation image = decoded_image("0;1;0", data);
    ok &= check(image.raster.width() == 30 && image.raster.height() == 60,
        "the image decodes to its 30 x 60 raster");

    bool image_diagnostics = false;
    for (qsizetype offset = 0; offset < image_bytes.size(); offset += 7) {
        image_diagnostics |= !diagnostics_in(model.ingest(image_bytes.mid(offset, 7))).empty();
    }
    ok &= check(!image_diagnostics, "the image arrives in pieces without diagnostics");

    // From row 1, the final sixel row's top at 60 pixels falls three rows down
    // and the sixel row below it needs a fifth row: the screen scrolls once.
    ok &= check(model.scrollback_size() == 1 &&
            history_row_text(model, 0) == QStringLiteral("$ img2sixel photo.png"),
        "the image scrolls the command line into history");
    for (int band = 0; band < 3; ++band) {
        ok &= check(slice_equals_band(slice_at(model, band), image.raster, band * 20, 30, 0),
            "each band of the image lies on its row after the scroll");
    }
    ok &= check(model.cursor_position().row == 3 && model.cursor_position().column == 0,
        "the cursor ends below the image, where the trailing graphics new line puts it");

    const auto pixel_is = [&](int row, int x, int y, const QColor& color) {
        const std::shared_ptr<const term::Terminal_image_slice> slice = slice_at(model, row);
        return slice != nullptr && slice->pixels.pixelColor(x, y) == color;
    };
    const QColor red(255, 0, 0);
    const QColor green(0, 255, 0);
    const QColor blue(0, 0, 255);
    ok &= check(pixel_is(0, 0, 0, red) && pixel_is(0, 29, 19, green),
        "the first band is red on the left and green on the right");
    ok &= check(pixel_is(1, 0, 9, red) && pixel_is(1, 0, 10, blue) &&
            pixel_is(1, 14, 15, green) && pixel_is(1, 20, 15, blue),
        "the second band turns blue at pixel row 30, under the green stripe");
    ok &= check(pixel_is(2, 12, 19, green) && pixel_is(2, 29, 19, blue),
        "the third band keeps the stripe drawn after the graphics carriage return");

    model.ingest(QByteArray("done\r\n$ "));
    ok &= check(model.scrollback_size() == 2 &&
            slice_equals_band(
                model.image_slice_for_testing(term::Terminal_buffer_id::PRIMARY, 1),
                image.raster,
                0,
                30,
                0),
        "the shell's new line scrolls the first band into history with its row");
    ok &= check(slice_equals_band(slice_at(model, 0), image.raster, 20, 30, 0) &&
            slice_equals_band(slice_at(model, 1), image.raster, 40, 30, 0),
        "the other bands move up with their rows");
    ok &= check(model.row_text(2) == QStringLiteral("done") && slice_at(model, 2) == nullptr,
        "the text after the image sits on its own row, clear of the image");
    ok &= check(model.cursor_position().row == 3 && model.cursor_position().column == 2,
        "the prompt follows on the next row");

    return ok;
}

// XTSMGRAPHICS reports a geometry within the decoded-size cap (anchor A2, owner
// decision D4), so at the smallest ring an encoder that fills it gets its image
// decoded and placed, not discarded over the cap.
// I5: no stored image exceeds the decoded-size cap. Two small images on one
// row can span more than the cap together, and a row image kept across a cell
// enlargement is resampled larger. A composite that would exceed the cap
// gives way to the newer band, with the cap's diagnostic, and the image's
// cursor and scroll geometry stand.
bool test_row_images_stay_within_the_decoded_size_cap()
{
    bool ok = true;

    const std::size_t capacity_bytes = 1024U * 1024U;
    const std::size_t cap_bytes      = term::terminal_history_ring_max_record_bytes(
        term::terminal_history_ring_aligned_capacity(capacity_bytes));

    const auto check_newer_band_only = [&](
        const term::Terminal_screen_model&                 model,
        const term::Terminal_screen_model_result&          result,
        int                                                first_column,
        term::terminal_cell_pixel_size_t                   cell,
        std::size_t                                        composite_bytes,
        const char*                                        label)
    {
        const std::shared_ptr<const term::Terminal_image_slice> slice = slice_at(model, 0);
        ok &= check(slice != nullptr &&
                slice->first_column == first_column &&
                slice->cell_pixel_size == cell &&
                slice->pixels.width() == cell.width &&
                slice->pixels.height() == cell.height,
            label);
        const std::vector<term::Parser_payload_diagnostic> diagnostics = diagnostics_in(result);
        ok &= check(diagnostics.size() == 1U &&
                diagnostics[0].code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED &&
                diagnostics[0].family == term::Parser_sequence_family::DCS &&
                diagnostics[0].raw_payload_size == composite_bytes &&
                diagnostics[0].limit_bytes == cap_bytes,
            "a row image that would exceed the cap is reported with its size and the cap");
        ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == first_column,
            "a refused composite leaves the image's cursor geometry as it was");
    };

    // Two 10 x 40 images at the two ends of a 160-column row of 10 x 40 cells
    // would span 1600 x 40 pixels: 256000 bytes against a 131072 byte cap.
    const term::terminal_cell_pixel_size_t tall_cell{10, 40};
    term::Terminal_screen_model spread = make_model(3, 160, tall_cell, 100, capacity_bytes);
    spread.ingest(sixel("1;0", "\"1;1;10;40"));
    const term::Terminal_screen_model_result spread_result =
        spread.ingest(cursor_to(0, 159) + sixel("1;0", "\"1;1;10;40"));
    check_newer_band_only(spread, spread_result, 159, tall_cell, 256000U,
        "two images whose union exceeds the cap leave the newer one on the row");

    // An 800 x 20 image kept across a cell doubling is resampled to 1600 x 40,
    // and with a 20 x 40 image at column 100 the union is 2020 x 40 pixels:
    // 323200 bytes.
    term::Terminal_screen_model enlarged = make_model(3, 120, k_cell, 100, capacity_bytes);
    enlarged.ingest(sixel("1;0", "\"1;1;800;20"));
    const term::terminal_cell_pixel_size_t doubled_cell{20, 40};
    enlarged.set_cell_pixel_size(doubled_cell);
    const term::Terminal_screen_model_result enlarged_result =
        enlarged.ingest(cursor_to(0, 100) + sixel("1;0", "\"1;1;20;40"));
    check_newer_band_only(enlarged, enlarged_result, 100, doubled_cell, 323200U,
        "an image resampled larger by a cell change gives way to the newer band");

    return ok;
}

bool test_reported_geometry_decodes_within_the_cap()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(
        24,
        80,
        k_cell,
        100,
        term::k_terminal_min_retained_history_capacity_bytes);
    const QSize geometry =
        reported_geometry(replies_in(model.ingest(QByteArray("\x1b[?2;1;0S"))));
    const qint64 limit_bytes = static_cast<qint64>(term::terminal_history_ring_max_record_bytes(
        term::k_terminal_min_retained_history_capacity_bytes));
    ok &= check(!geometry.isEmpty() &&
            geometry.width() <= 800 && geometry.height() <= 480 &&
            qint64{geometry.width()} * geometry.height() * 4 <= limit_bytes,
        "the reported geometry fits the text area and the decoded-size cap");

    // Full sixel rows, then a last one that sets only the bits the height needs.
    const QByteArray width  = QByteArray::number(geometry.width());
    const QByteArray height = QByteArray::number(geometry.height());
    QByteArray data = "\"1;1;" + width + ';' + height + "#1;2;100;0;0";
    for (int row = 0; row < geometry.height() / 6; ++row) {
        data += "#1!" + width + "~-";
    }
    if (geometry.height() % 6 != 0) {
        data += "#1!" + width + static_cast<char>('?' + (1 << (geometry.height() % 6)) - 1);
    }

    const term::Terminal_screen_model_result result = model.ingest(sixel("0;1;0", data));
    const int bands = (geometry.height() + k_cell.height - 1) / k_cell.height;
    ok &= check(diagnostics_in(result).empty() &&
            slice_at(model, 0) != nullptr &&
            slice_at(model, 0)->pixels.width() == geometry.width() &&
            slice_at(model, bands - 1) != nullptr &&
            slice_at(model, bands) == nullptr,
        "an image of the reported geometry decodes and is placed whole");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_slices_equal_raster_bands();
    ok &= test_images_clip_at_the_right_margin();
    ok &= test_cursor_follows_the_final_sixel_row();
    ok &= test_images_scroll_the_region_into_history();
    ok &= test_sixel_display_mode();
    ok &= test_aspect_ratios_clamp_to_one_region();
    ok &= test_images_erase_the_text_they_cover();
    ok &= test_images_on_one_row_composite();
    ok &= test_images_without_geometry_or_pixels();
    ok &= test_image_rows_move_and_die_with_their_rows();
    ok &= test_placement_marks_rows_dirty();
    ok &= test_oversized_image_rows_keep_their_text_in_history();
    ok &= test_capacity_shrink_keeps_text_rows_around_an_oversized_image();
    ok &= test_capacity_shrink_rebuilds_a_nearly_full_ring();
    ok &= test_capacity_shrink_signals_a_history_rewrite();
    ok &= test_capacity_decrease_drops_screen_images_over_the_cap();
    ok &= test_text_writes_and_erases_clear_image_cells();
    ok &= test_ich_and_dch_move_image_columns();
    ok &= test_image_rows_start_their_own_logical_lines();
    ok &= test_reflow_keeps_the_image_on_its_line_first_row();
    ok &= test_recovered_history_rows_keep_their_images();
    ok &= test_encoder_session_end_to_end();
    ok &= test_reported_geometry_decodes_within_the_cap();
    ok &= test_row_images_stay_within_the_decoded_size_cap();
    return ok ? 0 : 1;
}
