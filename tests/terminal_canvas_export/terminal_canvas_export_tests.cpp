#include "helpers/test_check.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QGuiApplication>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace term = vnm_terminal::internal;

bool terminal_canvas_public_header_contract();

namespace {

using vnm_terminal::test_helpers::check;

std::shared_ptr<term::Terminal_render_snapshot> make_snapshot(int rows, int columns)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = rows;
    auto snapshot = std::make_shared<term::Terminal_render_snapshot>(
        term::make_empty_render_snapshot({rows, columns}, viewport, 1U));
    snapshot->cursor.visible = false;
    return snapshot;
}

term::Terminal_render_cell make_cell(
    int                     row,
    int                     column,
    QString                 text,
    int                     display_width = 1,
    term::Terminal_style_id style_id = term::k_default_terminal_style_id)
{
    term::Terminal_render_cell cell;
    cell.position      = {row, column};
    cell.text          = term::Terminal_render_cell_text::from_source_cell(
        text,
        display_width,
        false);
    cell.display_width = display_width;
    cell.style_id      = style_id;
    cell.text_category = cell.text.category();
    return cell;
}

term::Terminal_render_cell make_wide_continuation(
    int                     row,
    int                     column,
    term::Terminal_style_id style_id)
{
    term::Terminal_render_cell cell;
    cell.position          = {row, column};
    cell.display_width     = 0;
    cell.wide_continuation = true;
    cell.style_id          = style_id;
    cell.text_category     = cell.text.category();
    return cell;
}

vnm_terminal::Terminal_canvas_export_result export_snapshot(
    VNM_TerminalSurface&                              surface,
    std::shared_ptr<term::Terminal_render_snapshot> snapshot)
{
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::move(snapshot));
    return vnm_terminal::export_terminal_canvas_frame(surface);
}

bool test_no_frame_and_invalid_source_are_explicit()
{
    bool                ok = true;
    VNM_TerminalSurface surface;

    const vnm_terminal::Terminal_canvas_export_result no_frame =
        vnm_terminal::export_terminal_canvas_frame(surface);
    ok &= check(no_frame.status == vnm_terminal::Terminal_canvas_export_status::NO_FRAME,
        "surface without a publication reports NO_FRAME");
    ok &= check(no_frame.frame == nullptr,
        "NO_FRAME never returns a partial frame");

    auto invalid = make_snapshot(1, 1);
    invalid->styles.clear();
    const vnm_terminal::Terminal_canvas_export_result invalid_result =
        export_snapshot(surface, std::move(invalid));
    ok &= check(
        invalid_result.status ==
            vnm_terminal::Terminal_canvas_export_status::INVALID_SOURCE_FRAME,
        "invalid source publication reports INVALID_SOURCE_FRAME");
    ok &= check(invalid_result.frame == nullptr,
        "invalid source publication never returns a partial frame");
    return ok;
}

bool test_frame_preserves_canvas_authority_and_prior_immutability()
{
    bool                ok = true;
    VNM_TerminalSurface surface;
    auto                snapshot = make_snapshot(2, 6);
    snapshot->metadata.sequence               = 41U;
    snapshot->metadata.publication_generation = 7U;
    snapshot->metadata.row_origin_generation  = 5U;
    snapshot->color_state.default_foreground_rgba = 0xffeeddccU;
    snapshot->color_state.default_background_rgba = 0xff112233U;
    snapshot->color_state.cursor_rgba             = 0xffabcdefU;
    snapshot->color_state.palette_rgba[12]         = 0xff123456U;
    snapshot->modes.reverse_video                  = true;
    snapshot->cursor.position                      = {1, 5};
    snapshot->cursor.shape                         = term::Terminal_cursor_shape::UNDERLINE;
    snapshot->cursor.visible                       = true;
    snapshot->cursor.blink_enabled                 = true;

    term::Terminal_text_style styled;
    styled.foreground = term::make_palette_terminal_color_ref(12U);
    styled.background = term::make_rgb_terminal_color_ref(0xff654321U);
    styled.attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::BOLD) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::FAINT) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::UNDERLINE) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::INVERSE) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::INVISIBLE) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::STRIKE);
    snapshot->styles.push_back(styled);

    snapshot->cells.push_back(make_cell(0, 0, QString::fromUtf8("\xe7\x95\x8c"), 2, 1U));
    snapshot->cells.push_back(make_wide_continuation(0, 1, 1U));
    snapshot->cells.push_back(make_cell(0, 2, QString::fromUtf8("e\xcc\x81"), 1, 1U));
    snapshot->cells.push_back(make_cell(0, 3, QStringLiteral(" "), 1, 1U));

    const vnm_terminal::Terminal_canvas_export_result first =
        export_snapshot(surface, std::move(snapshot));
    ok &= check(first.status == vnm_terminal::Terminal_canvas_export_status::OK,
        "valid styled canvas exports");
    ok &= check(first.frame != nullptr, "successful export returns a frame");
    if (first.frame == nullptr) {
        return false;
    }

    ok &= check(first.frame->rows == 2 && first.frame->columns == 6,
        "grid geometry survives export");
    ok &= check(
        first.frame->sequence == 41U &&
        first.frame->publication_generation == 7U &&
        first.frame->row_origin_generation == 5U,
        "publication identity survives export");
    ok &= check(
        first.frame->default_foreground_rgba == 0xffeeddccU &&
        first.frame->default_background_rgba == 0xff112233U &&
        first.frame->cursor_rgba == 0xffabcdefU &&
        first.frame->reverse_video,
        "color state and reverse-video authority survive export");
    ok &= check(first.frame->styles.size() == 2U,
        "source style table cardinality survives export");
    ok &= check(
        first.frame->styles[1].foreground_rgba == 0xff123456U &&
        first.frame->styles[1].background_rgba == 0xff654321U &&
        first.frame->styles[1].attributes == styled.attributes,
        "palette/truecolor resolution and source attributes survive export");
    ok &= check(first.frame->cells.size() == 3U,
        "wide continuation is represented only by its base cell");
    ok &= check(
        first.frame->cells[0].text == QString::fromUtf8("\xe7\x95\x8c") &&
        first.frame->cells[0].display_width == 2,
        "wide glyph and display width survive export");
    ok &= check(
        first.frame->cells[1].text == QString::fromUtf8("e\xcc\x81") &&
        first.frame->cells[1].display_width == 1,
        "combining sequence remains one owned canvas cell");
    ok &= check(
        first.frame->cells[2].column == 3 &&
        first.frame->cells[2].text == QStringLiteral(" ") &&
        first.frame->cells[2].style_index == 1U,
        "styled blank remains distinct from absent sparse cells");
    ok &= check(
        first.frame->cursor.row == 1 &&
        first.frame->cursor.column == 5 &&
        first.frame->cursor.shape ==
            vnm_terminal::Terminal_canvas_cursor_shape::UNDERLINE &&
        first.frame->cursor.visible && first.frame->cursor.blink_enabled,
        "cursor authority survives export");

    auto replacement = make_snapshot(1, 1);
    replacement->metadata.sequence = 42U;
    const vnm_terminal::Terminal_canvas_export_result second =
        export_snapshot(surface, std::move(replacement));
    ok &= check(second.status == vnm_terminal::Terminal_canvas_export_status::OK,
        "replacement canvas exports");
    ok &= check(
        first.frame->sequence == 41U &&
        first.frame->rows == 2 &&
        first.frame->cells.size() == 3U,
        "prior frame remains immutable after a later surface publication");
    return ok;
}

void fill_cells(
    term::Terminal_render_snapshot& snapshot,
    std::size_t                     count)
{
    snapshot.cells.reserve(count);
    const int columns = snapshot.grid_size.columns;
    for (std::size_t index = 0U; index < count; ++index) {
        snapshot.cells.push_back(make_cell(
            static_cast<int>(index / static_cast<std::size_t>(columns)),
            static_cast<int>(index % static_cast<std::size_t>(columns)),
            QStringLiteral("x")));
    }
}

bool expect_status(
    VNM_TerminalSurface&                       surface,
    std::shared_ptr<term::Terminal_render_snapshot> snapshot,
    vnm_terminal::Terminal_canvas_export_status     expected,
    const char*                                message)
{
    const vnm_terminal::Terminal_canvas_export_result result =
        export_snapshot(surface, std::move(snapshot));
    return
        check(result.status == expected, message) &&
        check(
            (expected == vnm_terminal::Terminal_canvas_export_status::OK) ==
                (result.frame != nullptr),
            "only successful bounded export returns a frame");
}

bool test_exact_bounds_and_one_over_fail_closed()
{
    bool                ok = true;
    VNM_TerminalSurface surface;

    ok &= expect_status(
        surface,
        make_snapshot(vnm_terminal::k_terminal_canvas_max_rows, 1),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact row bound exports");
    ok &= expect_status(
        surface,
        make_snapshot(vnm_terminal::k_terminal_canvas_max_rows + 1, 1),
        vnm_terminal::Terminal_canvas_export_status::ROW_LIMIT_EXCEEDED,
        "row bound plus one fails explicitly");
    ok &= expect_status(
        surface,
        make_snapshot(1, vnm_terminal::k_terminal_canvas_max_columns),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact column bound exports");
    ok &= expect_status(
        surface,
        make_snapshot(1, vnm_terminal::k_terminal_canvas_max_columns + 1),
        vnm_terminal::Terminal_canvas_export_status::COLUMN_LIMIT_EXCEEDED,
        "column bound plus one fails explicitly");

    auto exact_cells = make_snapshot(110, 300);
    fill_cells(*exact_cells, vnm_terminal::k_terminal_canvas_max_cells);
    ok &= expect_status(
        surface,
        std::move(exact_cells),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact materialized-cell bound exports");
    auto excess_cells = make_snapshot(110, 300);
    fill_cells(*excess_cells, vnm_terminal::k_terminal_canvas_max_cells + 1U);
    ok &= expect_status(
        surface,
        std::move(excess_cells),
        vnm_terminal::Terminal_canvas_export_status::CELL_LIMIT_EXCEEDED,
        "materialized-cell bound plus one fails explicitly");

    auto exact_styles = make_snapshot(1, 1);
    exact_styles->styles.resize(
        vnm_terminal::k_terminal_canvas_max_styles,
        term::make_default_terminal_text_style());
    ok &= expect_status(
        surface,
        std::move(exact_styles),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact style bound exports");
    auto excess_styles = make_snapshot(1, 1);
    excess_styles->styles.resize(
        vnm_terminal::k_terminal_canvas_max_styles + 1U,
        term::make_default_terminal_text_style());
    ok &= expect_status(
        surface,
        std::move(excess_styles),
        vnm_terminal::Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED,
        "style bound plus one fails explicitly");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    bool            ok = true;
    ok &= check(terminal_canvas_public_header_contract(),
        "public header compiles independently and publishes exact bounds");
    ok &= test_no_frame_and_invalid_source_are_explicit();
    ok &= test_frame_preserves_canvas_authority_and_prior_immutability();
    ok &= test_exact_bounds_and_one_over_fail_closed();
    return ok ? 0 : 1;
}
