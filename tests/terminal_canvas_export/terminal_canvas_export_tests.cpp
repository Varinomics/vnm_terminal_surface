#include "helpers/primary_backing_test_config.h"
#include "helpers/test_check.h"
#include "vnm_terminal/terminal_canvas_export.h"
#include "vnm_terminal/terminal_canvas_appearance.h"
#include "vnm_terminal/vnm_terminal_canvas.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_content_tail_projection.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_canvas_render_bridge.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QByteArray>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QSizeF>
#include <QString>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

bool terminal_canvas_public_header_contract();
bool terminal_canvas_public_export_header_contract();

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
    std::shared_ptr<term::Terminal_render_snapshot> snapshot,
    bool allow_wide_styles = false)
{
    const term::terminal_cell_metrics_t cell_metrics =
        term::VNM_TerminalSurface_render_bridge::cell_metrics(surface);
    surface.setWidth(snapshot->grid_size.columns * cell_metrics.width);
    surface.setHeight(snapshot->grid_size.rows * cell_metrics.height);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::move(snapshot));
    return vnm_terminal::export_terminal_canvas_frame(surface, allow_wide_styles);
}

bool expect_content_extent(
    const vnm_terminal::Terminal_canvas_export_result& result,
    int                                                 content_bottom,
    int                                                 scrollback_rows,
    int                                                 offset_from_tail,
    vnm_terminal::Terminal_canvas_buffer                active_buffer,
    const char*                                         message)
{
    return check(
        result.status == vnm_terminal::Terminal_canvas_export_status::OK &&
        result.frame != nullptr &&
        result.frame->content_extent.has_value() &&
        result.frame->content_extent->record_version ==
            vnm_terminal::k_terminal_canvas_content_extent_version &&
        result.frame->content_extent->content_bottom_row_exclusive ==
            content_bottom &&
        result.frame->content_extent->scrollback_rows == scrollback_rows &&
        result.frame->content_extent->offset_from_tail == offset_from_tail &&
        result.frame->content_extent->active_buffer == active_buffer,
        message);
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

    auto invalid_style = make_snapshot(1, 1);
    invalid_style->cells.push_back(make_cell(0, 0, QStringLiteral("x"), 1, 1U));
    const auto invalid_style_result = export_snapshot(surface, std::move(invalid_style));
    ok &= check(
        invalid_style_result.status ==
            vnm_terminal::Terminal_canvas_export_status::INVALID_SOURCE_FRAME &&
        invalid_style_result.frame == nullptr,
        "a cell referring outside the source style table fails before palette remapping");
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
        "referenced source styles survive export");
    ok &= check(
        first.frame->styles[1].foreground_rgba == 0xff123456U &&
        first.frame->styles[1].background_rgba == 0xff654321U &&
        first.frame->styles[1].attributes == styled.attributes,
        "palette/truecolor resolution and source attributes survive export");
    ok &= check(
        vnm_terminal::terminal_canvas_color_scheme_available(*first.frame) &&
        first.frame->color_references->styles[0].foreground ==
            vnm_terminal::k_terminal_canvas_color_default &&
        first.frame->color_references->styles[1].foreground == 12U &&
        first.frame->color_references->styles[1].background ==
            vnm_terminal::k_terminal_canvas_color_rgba,
        "export preserves default, indexed, and explicit RGB source semantics");
    auto local_frame = *first.frame;
    const QString local_scheme = vnm_terminal::terminal_canvas_default_color_scheme_name();
    ok &= check(vnm_terminal::apply_terminal_canvas_color_scheme(local_frame, local_scheme),
        "supported source references permit local color scheme selection");
    ok &= check(
        local_frame.styles[1].background_rgba == first.frame->styles[1].background_rgba &&
        local_frame.styles[1].attributes == first.frame->styles[1].attributes &&
        local_frame.reverse_video == first.frame->reverse_video,
        "local scheme retains explicit RGB and leaves presentation attributes unapplied");
    auto changed_desktop = *first.frame;
    changed_desktop.default_foreground_rgba = 0xff556677U;
    changed_desktop.default_background_rgba = 0xff223344U;
    changed_desktop.styles[0].foreground_rgba = changed_desktop.default_foreground_rgba;
    changed_desktop.styles[0].background_rgba = changed_desktop.default_background_rgba;
    changed_desktop.styles[1].foreground_rgba = 0xff8899aaU;
    ok &= check(vnm_terminal::apply_terminal_canvas_color_scheme(changed_desktop, local_scheme) &&
        changed_desktop.styles[0].foreground_rgba == local_frame.styles[0].foreground_rgba &&
        changed_desktop.styles[0].background_rgba == local_frame.styles[0].background_rgba &&
        changed_desktop.styles[1].foreground_rgba == local_frame.styles[1].foreground_rgba &&
        changed_desktop.styles[1].background_rgba == local_frame.styles[1].background_rgba,
        "desktop palette changes cannot change a companion's selected local scheme");
    auto unknown_colors = *first.frame;
    unknown_colors.color_references->record_version += 1U;
    ok &= check(!vnm_terminal::apply_terminal_canvas_color_scheme(unknown_colors, local_scheme) &&
        unknown_colors.default_background_rgba == first.frame->default_background_rgba,
        "unknown color records leave local scheme selection explicitly unavailable");
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
    ok &= expect_content_extent(
        first,
        2,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "canvas extent is derived from the same immutable snapshot");

    auto replacement = make_snapshot(1, 1);
    replacement->metadata.sequence = 42U;
    const vnm_terminal::Terminal_canvas_export_result second =
        export_snapshot(surface, std::move(replacement));
    ok &= check(second.status == vnm_terminal::Terminal_canvas_export_status::OK,
        "replacement canvas exports");
    ok &= check(
        first.frame->sequence == 41U &&
        first.frame->rows == 2 &&
        first.frame->cells.size() == 3U &&
        first.frame->content_extent.has_value() &&
        first.frame->content_extent->content_bottom_row_exclusive == 2,
        "prior frame remains immutable after a later surface publication");
    return ok;
}

bool test_content_extent_uses_semantic_cells_cursor_and_viewport()
{
    bool                ok = true;
    VNM_TerminalSurface surface;

    ok &= expect_content_extent(
        export_snapshot(surface, make_snapshot(5, 8)),
        1,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "empty primary snapshot retains the one-row semantic minimum");

    auto occupied = make_snapshot(5, 8);
    occupied->cells.push_back(make_cell(3, 0, QStringLiteral("x")));
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(occupied)),
        4,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "last occupied snapshot row determines semantic bottom in O(1)");

    auto hidden_cursor = make_snapshot(5, 8);
    hidden_cursor->cursor.position = {4, 7};
    hidden_cursor->cursor.visible  = false;
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(hidden_cursor)),
        1,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "hidden in-range cursor leaves the one-row semantic minimum");

    // The confirmed mobile update-prompt failure had text through row 10 and
    // a hidden cursor at row 44. Moving that cursor must not move the anchor.
    for (const int cursor_row : {44, 8}) {
        auto update_prompt = make_snapshot(45, 8);
        update_prompt->cells.push_back(make_cell(10, 0, QStringLiteral("Update")));
        update_prompt->cursor.position = {cursor_row, 0};
        update_prompt->cursor.visible  = false;
        ok &= expect_content_extent(
            export_snapshot(surface, std::move(update_prompt)),
            11, 0, 0,
            vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
            "hidden cursor movement preserves the visible update-prompt extent");
    }

    for (const bool blink_enabled : {false, true}) {
        auto visible_cursor = make_snapshot(45, 8);
        visible_cursor->cells.push_back(make_cell(10, 0, QStringLiteral("Update")));
        visible_cursor->cursor.position      = {44, 0};
        visible_cursor->cursor.visible       = true;
        visible_cursor->cursor.blink_enabled = blink_enabled;
        ok &= expect_content_extent(
            export_snapshot(surface, std::move(visible_cursor)),
            45, 0, 0,
            vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
            "visible cursor contributes independently of blinking");
    }

    auto off_frame_cursor = make_snapshot(5, 8);
    off_frame_cursor->cursor.position = {9, 7};
    off_frame_cursor->cursor.visible  = false;
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(off_frame_cursor)),
        1,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "hidden off-frame cursor does not force a detached frame to full height");

    auto negative_cursor = make_snapshot(5, 8);
    negative_cursor->cursor.position = {-1, 0};
    negative_cursor->cursor.visible  = false;
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(negative_cursor)),
        1,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "negative hidden cursor does not contribute to semantic bottom");

    auto styled_blank = make_snapshot(5, 8);
    term::Terminal_text_style styled = term::make_default_terminal_text_style();
    styled.attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::BOLD);
    styled_blank->styles.push_back(styled);
    styled_blank->cells.push_back(make_cell(3, 0, QStringLiteral(" "), 1, 1U));
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(styled_blank)),
        4,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "occupied styled blank contributes while sparse erasure remains absent");

    auto detached = make_snapshot(5, 8);
    detached->viewport.scrollback_rows  = 12;
    detached->viewport.offset_from_tail = 4;
    detached->viewport.follow_tail      = false;
    detached->cells.push_back(make_cell(1, 0, QStringLiteral("d")));
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(detached)),
        2,
        12,
        4,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "primary extent copies shared history and offset exactly");

    auto alternate = make_snapshot(5, 8);
    alternate->viewport.active_buffer = term::Terminal_buffer_id::ALTERNATE;
    alternate->cells.push_back(make_cell(2, 0, QStringLiteral("a")));
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(alternate)),
        3,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::ALTERNATE_BUFFER,
        "alternate extent retains honest bottom and zero shared history");

    auto public_projection = make_snapshot(5, 8);
    public_projection->basis = term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    public_projection->purpose = term::Terminal_render_snapshot_purpose::SCROLL;
    public_projection->viewport.scrollback_rows  = 9;
    public_projection->viewport.offset_from_tail = 3;
    public_projection->viewport.follow_tail      = false;
    public_projection->dirty_row_ranges          = {{0, 5}};
    public_projection->cells.push_back(make_cell(2, 0, QStringLiteral("p")));
    public_projection->cursor.position = {4, 0};
    public_projection->cursor.visible  = false;
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(public_projection)),
        3,
        9,
        3,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "public-projection extent uses only its safe copied snapshot basis");

    auto geometry_derived = make_snapshot(5, 8);
    geometry_derived->purpose =
        term::Terminal_render_snapshot_purpose::GEOMETRY_DERIVED;
    geometry_derived->cells.push_back(make_cell(1, 0, QStringLiteral("g")));
    geometry_derived->cursor.position = {2, 0};
    geometry_derived->cursor.visible  = false;
    ok &= expect_content_extent(
        export_snapshot(surface, std::move(geometry_derived)),
        2,
        0,
        0,
        vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER,
        "geometry-derived extent uses only its normalized safe snapshot basis");

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

    auto exact_cells = make_snapshot(128, 256);
    fill_cells(*exact_cells, vnm_terminal::k_terminal_canvas_max_cells);
    ok &= expect_status(
        surface,
        std::move(exact_cells),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact materialized-cell bound exports");
    auto excess_cells = make_snapshot(128, 257);
    fill_cells(*excess_cells, vnm_terminal::k_terminal_canvas_max_cells + 1U);
    ok &= expect_status(
        surface,
        std::move(excess_cells),
        vnm_terminal::Terminal_canvas_export_status::CELL_LIMIT_EXCEEDED,
        "materialized-cell bound plus one fails explicitly");

    const auto make_referenced_styles = [
        ](
            std::size_t count)
        {
            auto snapshot = make_snapshot(static_cast<int>((count + 15U) / 16U), 16);
            snapshot->styles.resize(count, term::make_default_terminal_text_style());
            for (std::size_t index = 0; index < count; ++index) {
                if (index != 0U) {
                    snapshot->styles[index].foreground = term::make_rgb_terminal_color_ref(
                        0xff000000U | static_cast<quint32>(index));
                }
                snapshot->cells.push_back(make_cell(
                    static_cast<int>(index / 16U),
                    static_cast<int>(index % 16U),
                    QStringLiteral("x"),
                    1,
                    static_cast<term::Terminal_style_id>(index)));
            }
            return snapshot;
        };
    ok &= expect_status(
        surface,
        make_referenced_styles(vnm_terminal::k_terminal_canvas_max_styles),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact referenced style bound exports");
    ok &= expect_status(
        surface,
        make_referenced_styles(vnm_terminal::k_terminal_canvas_max_styles + 1U),
        vnm_terminal::Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED,
        "referenced style bound plus one fails explicitly");
    return ok;
}

bool test_sparse_style_ids_preserve_colors_and_attributes()
{
    bool                ok = true;
    VNM_TerminalSurface surface;
    auto                snapshot = make_snapshot(1, 8);
    snapshot->styles.resize(400U, term::make_default_terminal_text_style());
    snapshot->styles[310].foreground = term::make_palette_terminal_color_ref(12U);
    snapshot->styles[310].background = term::make_rgb_terminal_color_ref(0xff123456U);
    snapshot->styles[310].attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::BOLD) |
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::UNDERLINE);
    snapshot->styles[375].background = term::make_palette_terminal_color_ref(7U);
    snapshot->styles[375].attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::ITALIC);
    snapshot->cells.push_back(make_cell(0, 0, QStringLiteral("A"), 1, 375U));
    snapshot->cells.push_back(make_cell(0, 1, QString::fromUtf8("\xe7\x95\x8c"), 2, 310U));
    snapshot->cells.push_back(make_wide_continuation(0, 2, 310U));
    snapshot->cells.push_back(make_cell(0, 3, QStringLiteral("B"), 1, 375U));
    snapshot->cells.push_back(make_cell(0, 4, QStringLiteral("C")));

    const auto result = export_snapshot(surface, snapshot);
    ok &= check(
        result.status == vnm_terminal::Terminal_canvas_export_status::OK && result.frame,
        "sparse source style ids export through a dense canvas palette");
    if (!result.frame) {
        return false;
    }
    const auto& frame = *result.frame;
    ok &= check(
        frame.styles.size() == 3U && frame.color_references &&
        frame.color_references->styles.size() == 3U && frame.cells.size() == 4U,
        "palette retains default plus referenced styles with matching color references");
    if (frame.styles.size() != 3U || !frame.color_references ||
        frame.color_references->styles.size() != 3U || frame.cells.size() != 4U)
    {
        return false;
    }
    ok &= check(
        frame.cells[0].style_index == 1U && frame.cells[1].style_index == 2U &&
        frame.cells[2].style_index == 1U && frame.cells[3].style_index == 0U &&
        frame.cells[1].display_width == 2,
        "cells remap repeated, wide and default styles consistently");
    const std::array<std::size_t, 3> source_ids{0U, 375U, 310U};
    for (std::size_t index = 0; index < source_ids.size(); ++index) {
        const std::size_t source_id = source_ids[index];
        const auto& source = snapshot->styles[source_id];
        ok &= check(
            frame.styles[index].foreground_rgba ==
                term::resolve_terminal_color_ref(source.foreground, snapshot->color_state, true) &&
            frame.styles[index].background_rgba ==
                term::resolve_terminal_color_ref(source.background, snapshot->color_state, false) &&
            frame.styles[index].attributes == source.attributes,
            "remapped palette preserves resolved colors and every style attribute");
    }
    ok &= check(
        frame.color_references->styles[0].foreground ==
            vnm_terminal::k_terminal_canvas_color_default &&
        frame.color_references->styles[1].foreground ==
            vnm_terminal::k_terminal_canvas_color_default &&
        frame.color_references->styles[1].background == 7U &&
        frame.color_references->styles[2].foreground == 12U &&
        frame.color_references->styles[2].background ==
            vnm_terminal::k_terminal_canvas_color_rgba &&
        snapshot->styles.size() == 400U && snapshot->cells[0].style_id == 375U,
        "remapping preserves color-reference semantics without mutating the snapshot");
    return ok;
}

bool test_wide_style_export_is_explicit_and_lossless()
{
    namespace canvas = vnm_terminal;
    bool ok = true;
    VNM_TerminalSurface surface;
    auto snapshot = make_snapshot(52, 144);
    snapshot->styles.resize(3'640U, term::make_default_terminal_text_style());
    constexpr std::size_t k_referenced_styles = 698U;
    for (std::size_t index = 0U; index < k_referenced_styles; ++index) {
        const auto source_id = static_cast<term::Terminal_style_id>(
            index == 0U ? 0U : index + 1'000U);
        auto& source = snapshot->styles[source_id];
        if (index != 0U) {
            const auto palette_index = static_cast<std::uint8_t>(index % 256U);
            source.foreground = index % 2U == 0U
                ? term::make_rgb_terminal_color_ref(0xff100000U | static_cast<quint32>(index))
                : term::make_palette_terminal_color_ref(palette_index);
            source.background = index % 2U == 0U
                ? term::make_palette_terminal_color_ref(palette_index)
                : term::make_rgb_terminal_color_ref(0xff200000U | static_cast<quint32>(index));
            source.attributes = static_cast<std::uint16_t>(index % 256U);
        }
        snapshot->cells.push_back(make_cell(
            static_cast<int>(index / 144U),
            static_cast<int>(index % 144U),
            QStringLiteral("x"),
            1,
            source_id));
    }
    const auto unrequested = export_snapshot(surface, snapshot);
    ok &= check(
        unrequested.status == canvas::Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED &&
            !unrequested.frame,
        "698 referenced styles remain unavailable without explicit wide-style opt-in");

    const auto result = export_snapshot(surface, snapshot, true);
    ok &= check(result.status == canvas::Terminal_canvas_export_status::OK && result.frame,
        "explicit opt-in exports the 698-style workload");
    if (!result.frame) {
        return false;
    }
    const auto& frame = *result.frame;
    if (!check(
            frame.styles.size() == k_referenced_styles && frame.wide_styles &&
                frame.wide_styles->record_version == canvas::k_terminal_canvas_wide_styles_version &&
                frame.cells.size() == k_referenced_styles && frame.color_references &&
                frame.color_references->styles.size() == k_referenced_styles,
            "wide export contains exactly the referenced palette and its matching color selectors"))
    {
        return false;
    }
    bool exact_styles = true;
    for (std::size_t index = 0U; index < k_referenced_styles; ++index) {
        const auto& source = snapshot->styles[index == 0U ? 0U : index + 1'000U];
        const auto& style  = frame.styles[index];
        const auto& refs   = frame.color_references->styles[index];
        const auto palette_index = static_cast<std::uint16_t>(index % 256U);
        const std::uint16_t expected_foreground = index == 0U
            ? canvas::k_terminal_canvas_color_default
            : index % 2U == 0U ? canvas::k_terminal_canvas_color_rgba : palette_index;
        const std::uint16_t expected_background = index == 0U
            ? canvas::k_terminal_canvas_color_default
            : index % 2U == 0U ? palette_index : canvas::k_terminal_canvas_color_rgba;
        exact_styles &=
            frame.cells[index].style_index == index &&
            style.foreground_rgba ==
                term::resolve_terminal_color_ref(source.foreground, snapshot->color_state, true) &&
            style.background_rgba ==
                term::resolve_terminal_color_ref(source.background, snapshot->color_state, false) &&
            style.attributes == source.attributes &&
            refs.foreground == expected_foreground && refs.background == expected_background;
    }
    ok &= check(exact_styles && frame.cells.back().style_index == 697U,
        "all 698 resolved colors, attributes, selectors and indices survive without truncation");
    auto themed = frame;
    ok &= check(canvas::apply_terminal_canvas_color_scheme(
            themed, canvas::terminal_canvas_default_color_scheme_name()),
        "wide palettes retain local color-scheme selection");
    bool explicit_colors_preserved = true;
    for (std::size_t index = 1U; index < k_referenced_styles; ++index) {
        explicit_colors_preserved &= themed.styles[index].attributes == frame.styles[index].attributes;
        explicit_colors_preserved &= index % 2U == 0U
            ? themed.styles[index].foreground_rgba == frame.styles[index].foreground_rgba
            : themed.styles[index].background_rgba == frame.styles[index].background_rgba;
    }
    ok &= check(explicit_colors_preserved,
        "local schemes preserve every explicit RGB channel and presentation attribute");

    snapshot->cells.resize(canvas::k_terminal_canvas_max_styles);
    const auto narrow = export_snapshot(surface, snapshot, true);
    ok &= check(narrow.status == canvas::Terminal_canvas_export_status::OK && narrow.frame &&
            narrow.frame->styles.size() == canvas::k_terminal_canvas_max_styles &&
            !narrow.frame->wide_styles,
        "opt-in keeps an exact 256-style publication in the unmarked representation");
    return ok;
}

bool test_text_bounds_fail_closed_without_truncation()
{
    bool                ok = true;
    VNM_TerminalSurface surface;

    const QString exact_cell_text(
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units,
        QLatin1Char('x'));
    auto exact_cell = make_snapshot(1, 1);
    exact_cell->cells.push_back(make_cell(0, 0, exact_cell_text));
    ok &= expect_status(
        surface,
        std::move(exact_cell),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact per-cell UTF-16 code-unit limit exports");

    QString excess_cell_text = exact_cell_text;
    excess_cell_text += QLatin1Char('x');
    auto excess_cell = make_snapshot(1, 1);
    excess_cell->cells.push_back(make_cell(0, 0, std::move(excess_cell_text)));
    ok &= expect_status(
        surface,
        std::move(excess_cell),
        vnm_terminal::Terminal_canvas_export_status::
            CELL_TEXT_UTF16_CODE_UNIT_LIMIT_EXCEEDED,
        "per-cell UTF-16 code-unit limit plus one fails explicitly");

    constexpr qsizetype k_multibyte_utf8_bytes_per_code_unit = 2;
    static_assert(
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes %
            (vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units *
                k_multibyte_utf8_bytes_per_code_unit) == 0);
    const qsizetype aggregate_cell_count =
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes /
        (vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units *
            k_multibyte_utf8_bytes_per_code_unit);
    const QString multibyte_cell_text(
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units,
        QChar(0x00e9U));

    auto exact_frame = make_snapshot(1, static_cast<int>(aggregate_cell_count));
    for (int column = 0; column < exact_frame->grid_size.columns; ++column) {
        exact_frame->cells.push_back(
            make_cell(0, column, multibyte_cell_text));
    }
    ok &= expect_status(
        surface,
        std::move(exact_frame),
        vnm_terminal::Terminal_canvas_export_status::OK,
        "exact aggregate UTF-8 byte limit exports multibyte text");

    auto excess_frame = make_snapshot(
        1,
        static_cast<int>(aggregate_cell_count) + 1);
    for (int column = 0; column < aggregate_cell_count; ++column) {
        excess_frame->cells.push_back(
            make_cell(0, column, multibyte_cell_text));
    }
    excess_frame->cells.push_back(make_cell(
        0,
        static_cast<int>(aggregate_cell_count),
        QStringLiteral("x")));
    ok &= expect_status(
        surface,
        std::move(excess_frame),
        vnm_terminal::Terminal_canvas_export_status::
            FRAME_TEXT_UTF8_BYTE_LIMIT_EXCEEDED,
        "aggregate UTF-8 byte limit plus one fails explicitly");
    return ok;
}

bool canvas_cells_equal(
    const std::vector<vnm_terminal::Terminal_canvas_cell>& left,
    const std::vector<vnm_terminal::Terminal_canvas_cell>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].row           != right[index].row           ||
            left[index].column        != right[index].column        ||
            left[index].display_width != right[index].display_width ||
            left[index].style_index   != right[index].style_index   ||
            left[index].text          != right[index].text)
        {
            return false;
        }
    }

    return true;
}

bool test_row_images_export_with_publication()
{
    bool                ok = true;
    VNM_TerminalSurface surface;

    auto plain = make_snapshot(2, 6);
    plain->cells.push_back(make_cell(0, 0, QStringLiteral("a")));
    auto with_image = std::make_shared<term::Terminal_render_snapshot>(*plain);
    QImage pixels(30, 20, QImage::Format_RGBA8888_Premultiplied);
    pixels.fill(QColor(200, 40, 40));
    term::set_render_snapshot_row_image(
        *with_image,
        1,
        std::make_shared<const term::Terminal_image_slice>(
            term::Terminal_image_slice{pixels, 1, {10, 20}, 3U}));

    const vnm_terminal::Terminal_canvas_export_result plain_result =
        export_snapshot(surface, plain);
    const vnm_terminal::Terminal_canvas_export_result image_result =
        export_snapshot(surface, with_image);
    ok &= check(
        plain_result.status == vnm_terminal::Terminal_canvas_export_status::OK &&
        image_result.status == vnm_terminal::Terminal_canvas_export_status::OK &&
        plain_result.frame != nullptr &&
        image_result.frame != nullptr,
        "a snapshot showing an image exports");
    if (plain_result.frame == nullptr || image_result.frame == nullptr) {
        return false;
    }

    ok &= check(
        canvas_cells_equal(image_result.frame->cells, plain_result.frame->cells) &&
        image_result.frame->content_extent.has_value() &&
        plain_result.frame->content_extent.has_value() &&
        image_result.frame->content_extent->content_bottom_row_exclusive ==
            plain_result.frame->content_extent->content_bottom_row_exclusive,
        "image capability preserves the independent text/cursor extent contract");
    ok &= check(
        plain_result.frame->images && plain_result.frame->images->rows.empty() &&
        plain_result.frame->images->status == vnm_terminal::Terminal_canvas_images_status::AVAILABLE,
        "an image-free publication explicitly clears prior images");
    if (!check(image_result.frame->images && image_result.frame->images->rows.size() == 1U,
            "an image publication exports its row slice"))
    {
        return false;
    }
    const auto& exported = image_result.frame->images->rows.front();
    ok &= check(
        image_result.frame->images->record_version == vnm_terminal::k_terminal_canvas_images_version &&
        image_result.frame->images->status == vnm_terminal::Terminal_canvas_images_status::AVAILABLE &&
        exported.row == 1 && exported.first_column == 1 &&
        exported.cell_pixel_width == 10 && exported.cell_pixel_height == 20 &&
        exported.revision == 3U && exported.pixels == pixels,
        "export preserves source RGBA pixels, cell scale, row, column and revision");

    pixels.fill(QColor(20, 160, 80));
    const auto cleared = export_snapshot(surface, plain);
    ok &= check(
        cleared.frame && cleared.frame->images && cleared.frame->images->rows.empty() &&
        exported.pixels.pixelColor(0, 0) == QColor(200, 40, 40),
        "new publications and caller pixel mutation do not alter an exported frame");

    auto invalid = std::make_shared<term::Terminal_render_snapshot>(*with_image);
    term::set_render_snapshot_row_image(
        *invalid,
        1,
        std::make_shared<const term::Terminal_image_slice>(
            term::Terminal_image_slice{pixels, 1, {10, 20}, 0U}));
    const auto invalid_result = export_snapshot(surface, std::move(invalid));
    ok &= check(
        invalid_result.status == vnm_terminal::Terminal_canvas_export_status::OK &&
        invalid_result.frame && invalid_result.frame->images &&
        invalid_result.frame->images->status == vnm_terminal::Terminal_canvas_images_status::INVALID &&
        invalid_result.frame->images->rows.empty() &&
        canvas_cells_equal(invalid_result.frame->cells, plain_result.frame->cells),
        "invalid source images are explicit and leave the publication's text available");
    return ok;
}

bool test_leading_rows_preserve_base_frame_and_bound_the_attachment()
{
    VNM_TerminalSurface surface;
    auto base = make_snapshot(6, 8);
    base->viewport.scrollback_rows = 2;
    base->cells.push_back(make_cell(0, 0, QStringLiteral("live")));
    base->cursor.position = {1, 4};
    base->cursor.visible = true;
    const auto plain = export_snapshot(surface, base);
    auto combined = make_snapshot(6, 8);
    combined->viewport.scrollback_rows = 2;
    combined->viewport.offset_from_tail = 2;
    combined->viewport.follow_tail = false;
    combined->cells.push_back(make_cell(0, 0, QStringLiteral("old")));
    combined->cells.push_back(make_cell(2, 0, QStringLiteral("live")));
    QImage pixels(10, 20, QImage::Format_RGBA8888_Premultiplied);
    pixels.fill(QColor(20, 40, 200));
    term::set_render_snapshot_row_image(*combined, 1,
        std::make_shared<const term::Terminal_image_slice>(
            term::Terminal_image_slice{pixels, 0, {10, 20}, 9U}));
    base->leading_row_count = 2;
    base->leading_rows_snapshot = combined;
    const auto exported = export_snapshot(surface, base);
    if (!check(plain.frame && exported.frame && exported.frame->leading_rows,
            "a separate retained-row projection exports an optional attachment"))
    {
        return false;
    }
    const auto& leading = *exported.frame->leading_rows;
    bool ok = check(exported.frame->rows == plain.frame->rows &&
            exported.frame->cells == plain.frame->cells && exported.frame->styles == plain.frame->styles &&
            exported.frame->cursor.row == plain.frame->cursor.row &&
            exported.frame->content_extent->offset_from_tail == plain.frame->content_extent->offset_from_tail &&
            exported.frame->content_extent->content_bottom_row_exclusive ==
                plain.frame->content_extent->content_bottom_row_exclusive,
        "the optional prefix leaves native cells, cursor, geometry and viewport metadata unchanged");
    if (!check(leading.rows == 2 && leading.cells.size() == 1U &&
            leading.cells.front().text == QStringLiteral("old") && leading.images &&
            leading.images->rows.size() == 1U && leading.images->rows.front().row == 1 &&
            leading.images->rows.front().pixels == pixels &&
            vnm_terminal::terminal_canvas_leading_rows_are_valid(leading, 6, 8),
        "the attachment contains only preceding cells and exact image slices within its own bounds"))
    {
        return false;
    }
    auto invalid = leading;
    invalid.cells.front().row = invalid.rows;
    ok &= check(!vnm_terminal::terminal_canvas_leading_rows_are_valid(invalid, 6, 8),
        "a prefix cannot smuggle active rows beyond its declared extent");
    invalid = leading;
    invalid.images->rows.front().row = invalid.rows;
    ok &= check(!vnm_terminal::terminal_canvas_leading_rows_are_valid(invalid, 6, 8),
        "prefix image rows obey the same declared extent");
    invalid = leading;
    invalid.record_version = 99U;
    ok &= check(vnm_terminal::terminal_canvas_leading_rows_are_valid(invalid, 6, 8),
        "an unknown optional prefix version does not invalidate ordinary terminal data");
    base->leading_row_count = 6;
    const auto malformed = export_snapshot(surface, base);
    ok &= check(malformed.frame && malformed.frame->leading_rows &&
            malformed.frame->leading_rows->status == vnm_terminal::Terminal_canvas_leading_rows_status::INVALID &&
            malformed.frame->cells == plain.frame->cells &&
            exported.frame->leading_rows->rows == 2,
        "an incoherent prefix becomes unavailable without damaging the base or prior immutable export");
    return ok;
}

class Receiver_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&, term::Terminal_backend_callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
        return term::backend_accept();
    }
    term::Terminal_backend_result write(QByteArray) override { return term::backend_accept(); }
    term::Terminal_backend_result resize(term::Terminal_backend_resize_request) override { return term::backend_accept(); }
    term::Terminal_backend_result set_output_paused(bool) override { return term::backend_accept(); }
    term::Terminal_backend_result interrupt() override { return term::backend_accept(); }
    term::Terminal_backend_result terminate() override { return term::backend_accept(); }
    void output(QByteArray bytes) { m_callbacks.output_received(std::move(bytes)); }

private:
    term::Terminal_backend_callbacks m_callbacks;
};

struct Receiver_session
{
    std::unique_ptr<term::Terminal_session> session;
    Receiver_backend*                       backend = nullptr;

    void write_output(QByteArray bytes)
    {
        backend->output(std::move(bytes));
        session->process_backend_callback_events();
    }
};

Receiver_session start_receiver_session(int rows, QByteArray output, bool companion)
{
    auto backend = std::make_unique<Receiver_backend>();
    Receiver_session result;
    result.backend = backend.get();
    auto config = vnm_terminal::test_helpers::recovery_disabled_primary_backing_session_config();
    config.selection_viewport_projection_enabled = true;
    result.session = std::make_unique<term::Terminal_session>(std::move(backend), config);
    term::Terminal_launch_config launch;
    launch.argv = {QStringLiteral("receiver-equivalence-fixture")};
    launch.initial_grid_size = term::terminal_grid_size_t{rows, 20};
    (void)result.session->start(launch);
    result.session->set_companion_presentation(companion);
    result.write_output(std::move(output));
    return result;
}

void resize_rows(Receiver_session& fixture, int rows)
{
    (void)fixture.session->resize(QSizeF(200.0, 20.0 * rows), {rows, 20});
}

QByteArray numbered_lines(int count)
{
    QByteArray bytes;
    for (int line = 0; line < count; ++line) {
        bytes += QStringLiteral("line-%1\r\n").arg(line, 2, 10, QLatin1Char('0')).toUtf8();
    }
    return bytes;
}

// What a cell looks like: its text and width, resolved colors and attributes.
struct Cell_look
{
    int           column     = 0;
    int           width      = 1;
    QString       text;
    quint32       foreground = 0U;
    quint32       background = 0U;
    std::uint16_t attributes = 0U;

    bool operator==(const Cell_look&) const = default;
};

std::vector<Cell_look> row_looks(const term::Terminal_render_snapshot& snapshot, int row)
{
    std::vector<Cell_look> looks;
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row != row || cell.wide_continuation) {
            continue;
        }
        const term::Terminal_text_style& style = snapshot.styles[cell.style_id];
        looks.push_back({
            cell.position.column,
            cell.display_width,
            cell.text.to_qstring(),
            term::resolve_terminal_color_ref(style.foreground, snapshot.color_state, true),
            term::resolve_terminal_color_ref(style.background, snapshot.color_state, false),
            style.attributes,
        });
    }
    std::sort(looks.begin(), looks.end(), [](const Cell_look& left, const Cell_look& right) {
        return left.column < right.column;
    });
    return looks;
}

QString row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    QString text(snapshot.grid_size.columns, QLatin1Char(' '));
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && !cell.wide_continuation) {
            text.replace(cell.position.column, 1, cell.text.to_qstring());
        }
    }
    while (text.endsWith(QLatin1Char(' '))) {
        text.chop(1);
    }
    return text;
}

// The receiver composes the exported frame through the same producer as the
// desktop, so both present the same cells, row identities and slots, cursor,
// images, selection and maximum (DESIGN 5.6).
bool expect_receiver_matches_desktop(
    VNM_TerminalSurface&                                   surface,
    const std::shared_ptr<const term::Terminal_render_snapshot>& native,
    const char*                                            message)
{
    const auto desktop = term::make_presented_snapshot(native);
    auto source = std::make_shared<term::Terminal_render_snapshot>(*native);
    const auto exported = export_snapshot(surface, source);
    VNM_TerminalCanvas canvas;
    if (!check(desktop && exported.frame && exported.frame->presentation &&
            canvas.set_canvas_frame(exported.frame), message))
    {
        return false;
    }
    const auto received = term::VNM_TerminalCanvas_render_bridge::render_snapshot(canvas);
    bool same = received != nullptr &&
        received->display_row_logical_rows == desktop->display_row_logical_rows &&
        received->cursor.visible == desktop->cursor.visible &&
        (!desktop->cursor.visible || received->cursor.position == desktop->cursor.position) &&
        received->visible_line_provenance.size() == desktop->visible_line_provenance.size() &&
        received->selection_spans.size() == desktop->selection_spans.size() &&
        canvas.scrollback_rows() == term::presentation_max_offset_from_tail(native->viewport);
    for (int row = 0; same && row < desktop->grid_size.rows; ++row) {
        const auto image_at = [row](const term::Terminal_render_snapshot& snapshot) {
            return snapshot.visible_row_images.empty()
                ? std::shared_ptr<const term::Terminal_image_slice>{}
                : snapshot.visible_row_images[static_cast<std::size_t>(row)];
        };
        const auto desktop_image = image_at(*desktop);
        const auto received_image = image_at(*received);
        same = row_looks(*received, row) == row_looks(*desktop, row) &&
            static_cast<bool>(desktop_image) == static_cast<bool>(received_image) &&
            (!desktop_image ||
                (desktop_image->pixels == received_image->pixels &&
                    desktop_image->first_column == received_image->first_column &&
                    desktop_image->cell_pixel_size.width == received_image->cell_pixel_size.width &&
                    desktop_image->cell_pixel_size.height == received_image->cell_pixel_size.height));
        if (same && !desktop->visible_line_provenance.empty()) {
            same = received->visible_line_provenance[static_cast<std::size_t>(row)].retained_line_id ==
                desktop->visible_line_provenance[static_cast<std::size_t>(row)].retained_line_id;
        }
    }
    for (std::size_t index = 0; same && index < desktop->selection_spans.size(); ++index) {
        same = received->selection_spans[index].row == desktop->selection_spans[index].row &&
            received->selection_spans[index].first_column == desktop->selection_spans[index].first_column &&
            received->selection_spans[index].column_count == desktop->selection_spans[index].column_count;
    }
    return check(same, message);
}

// Leading rows whose text exceeds the frame budget: twelve maximal cells fit
// the 96 KiB limit and the thirteenth does not.
std::shared_ptr<term::Terminal_render_snapshot> oversized_prefix_frame(
    term::Terminal_presentation_mapping presentation,
    bool                                frozen)
{
    constexpr int k_columns = 13;
    auto base = make_snapshot(6, k_columns);
    base->viewport.scrollback_rows = 2;
    base->viewport.presentation    = presentation;
    base->frozen_basis             = frozen;
    base->cells.push_back(make_cell(0, 0, QStringLiteral("live")));
    auto leading = make_snapshot(6, k_columns);
    leading->viewport.scrollback_rows  = 2;
    leading->viewport.offset_from_tail = 2;
    leading->viewport.follow_tail      = false;
    const QString multibyte_cell_text(
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units,
        QChar(0x00e9U));
    for (int column = 0; column < k_columns; ++column) {
        leading->cells.push_back(make_cell(1, column, multibyte_cell_text));
    }
    base->leading_row_count     = 2;
    base->leading_rows_snapshot = leading;
    return base;
}

// A prefix the mapping presents is part of the frame: when it cannot be
// serialized the export fails instead of sending a frame that presents
// differently. A prefix the mapping does not present stays an optional
// attachment.
bool test_required_prefix_export_failures_are_reported()
{
    VNM_TerminalSurface surface;
    bool ok = true;
    const auto live = export_snapshot(surface, oversized_prefix_frame(
        {term::Terminal_presentation_policy::AUTOMATIC, 2, 0}, false));
    ok &= check(live.status == vnm_terminal::Terminal_canvas_export_status::FRAME_TEXT_UTF8_BYTE_LIMIT_EXCEEDED &&
            !live.frame,
        "a live frame whose presented prefix exceeds the text limit fails export");
    const auto frozen = export_snapshot(surface, oversized_prefix_frame(
        {term::Terminal_presentation_policy::AUTOMATIC, 4, 0}, true));
    ok &= check(frozen.status == vnm_terminal::Terminal_canvas_export_status::FRAME_TEXT_UTF8_BYTE_LIMIT_EXCEEDED &&
            !frozen.frame,
        "a frozen frame whose captured prefix exceeds the text limit fails export");
    const auto supplemental = export_snapshot(surface, oversized_prefix_frame({}, false));
    ok &= check(supplemental.status == vnm_terminal::Terminal_canvas_export_status::OK &&
            supplemental.frame && supplemental.frame->leading_rows &&
            supplemental.frame->leading_rows->status == vnm_terminal::Terminal_canvas_leading_rows_status::OVER_LIMIT &&
            supplemental.frame->presentation && supplemental.frame->presentation->leading_rows == 0,
        "a native frame's supplemental prefix over the limit stays an unavailable attachment");
    return ok;
}

bool test_receiver_presents_through_the_producer()
{
    VNM_TerminalSurface surface;
    bool ok = true;

    // AUTOMATIC: S = 12, H = 6, P = 4, at the tail and three rows up, with a
    // selection on an acquired row.
    Receiver_session automatic = start_receiver_session(2, numbered_lines(13) + QByteArrayLiteral("$ "), false);
    resize_rows(automatic, 6);
    automatic.session->set_selection_range({{9, 0}, {9, 4}, term::Terminal_selection_mode::NORMAL});
    const auto automatic_frame = automatic.session->latest_render_snapshot_handle();
    ok &= check(automatic_frame &&
            automatic_frame->viewport.presentation.policy == term::Terminal_presentation_policy::AUTOMATIC &&
            automatic_frame->viewport.presentation.hidden_rows == 4 && automatic_frame->leading_row_count == 4,
        "the automatic fixture hides four rows and acquires four rows above its view");
    ok &= expect_receiver_matches_desktop(surface, automatic_frame,
        "the receiver presents the automatic tail like the desktop");
    (void)automatic.session->scroll_viewport_lines(3);
    ok &= expect_receiver_matches_desktop(surface, automatic.session->latest_render_snapshot_handle(),
        "the receiver presents a scrolled automatic view like the desktop");

    // A missing live payload is an invalid frame.
    auto source = std::make_shared<term::Terminal_render_snapshot>(*automatic_frame);
    const auto exported = export_snapshot(surface, source);
    VNM_TerminalCanvas canvas;
    if (check(exported.frame && exported.frame->presentation,
            "the automatic frame exports its mapping record"))
    {
        auto without_rows = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*exported.frame);
        without_rows->leading_rows.reset();
        ok &= check(!canvas.set_canvas_frame(without_rows),
            "a frame whose record names leading rows it does not carry is invalid");
        auto without_record_rows = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*exported.frame);
        without_record_rows->leading_rows.reset();
        without_record_rows->presentation->leading_rows = 0;
        ok &= check(!canvas.set_canvas_frame(without_record_rows),
            "a live frame that lacks a requested row is invalid");
        auto unknown = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*exported.frame);
        unknown->presentation->record_version = 99U;
        ok &= check(canvas.set_canvas_frame(unknown) &&
                canvas.scrollback_rows() == automatic_frame->viewport.scrollback_rows &&
                !term::VNM_TerminalCanvas_render_bridge::render_snapshot(canvas)->display_source,
            "an unknown record version leaves the native frame available");
    }

    // Images on a native row and on an acquired row travel with their rows.
    {
        QImage native_pixels(10, 20, QImage::Format_RGBA8888_Premultiplied);
        native_pixels.fill(QColor(200, 40, 20));
        QImage leading_pixels(24, 16, QImage::Format_RGBA8888_Premultiplied);
        leading_pixels.fill(QColor(20, 160, 90));
        auto with_images = std::make_shared<term::Terminal_render_snapshot>(*automatic_frame);
        term::set_render_snapshot_row_image(*with_images, 0,
            std::make_shared<const term::Terminal_image_slice>(
                term::Terminal_image_slice{native_pixels, 1, {10, 20}, 41U}));
        auto leading_with_image =
            std::make_shared<term::Terminal_render_snapshot>(*with_images->leading_rows_snapshot);
        term::set_render_snapshot_row_image(*leading_with_image, 1,
            std::make_shared<const term::Terminal_image_slice>(
                term::Terminal_image_slice{leading_pixels, 3, {12, 16}, 42U}));
        with_images->leading_rows_snapshot = leading_with_image;
        ok &= expect_receiver_matches_desktop(surface, with_images,
            "the receiver presents images on native and acquired rows like the desktop");
    }

    // Companion S = 12, H = 6, P = 4, o = 8 with K = 0 and with K = 8: both
    // acquire no leading rows, but present differently.
    Receiver_session companion_k0 = start_receiver_session(2, numbered_lines(13) + QByteArrayLiteral("$ "), true);
    resize_rows(companion_k0, 6);
    (void)companion_k0.session->scroll_viewport_lines(8);
    const auto k0_frame = companion_k0.session->latest_render_snapshot_handle();
    Receiver_session companion_k8 = start_receiver_session(10, numbered_lines(13) + QByteArrayLiteral("$ "), true);
    resize_rows(companion_k8, 2);
    resize_rows(companion_k8, 6);
    (void)companion_k8.session->scroll_viewport_lines(8);
    const auto k8_frame = companion_k8.session->latest_render_snapshot_handle();
    ok &= check(k0_frame && k8_frame &&
            k0_frame->viewport.scrollback_rows == 12 && k8_frame->viewport.scrollback_rows == 12 &&
            k0_frame->viewport.presentation.hidden_rows == 4 && k8_frame->viewport.presentation.hidden_rows == 4 &&
            k0_frame->viewport.presentation.insertion_row == 12 && k8_frame->viewport.presentation.insertion_row == 4 &&
            k0_frame->viewport.offset_from_tail == 8 && k8_frame->viewport.offset_from_tail == 8 &&
            k0_frame->leading_row_count == k8_frame->leading_row_count,
        "the Companion fixtures differ only in K");
    ok &= expect_receiver_matches_desktop(surface, k0_frame,
        "the receiver presents Companion with K = 0 like the desktop");
    ok &= expect_receiver_matches_desktop(surface, k8_frame,
        "the receiver presents Companion with K = 8 like the desktop");

    // A record whose coverage misses a displayed row is invalid even when the
    // mapping presents the native rows as they are: identity Companion here,
    // and NATIVE below.
    const auto expect_incomplete_coverage_rejected = [&](
        const std::shared_ptr<const term::Terminal_render_snapshot>& frame,
        const char* message)
    {
        auto source_frame = std::make_shared<term::Terminal_render_snapshot>(*frame);
        const auto exported_frame = export_snapshot(surface, source_frame);
        if (!check(exported_frame.frame && exported_frame.frame->presentation, message)) {
            return false;
        }
        auto incomplete = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*exported_frame.frame);
        incomplete->presentation->covered_rows = incomplete->rows - 1;
        VNM_TerminalCanvas receiver;
        return check(receiver.set_canvas_frame(exported_frame.frame) &&
                !receiver.set_canvas_frame(incomplete), message);
    };
    ok &= expect_incomplete_coverage_rejected(k0_frame,
        "an identity Companion record missing a displayed row is invalid");
    Receiver_session native = start_receiver_session(6, numbered_lines(3) + QByteArrayLiteral("$ "), false);
    const auto native_frame = native.session->latest_render_snapshot_handle();
    ok &= check(native_frame &&
            native_frame->viewport.presentation.policy == term::Terminal_presentation_policy::NATIVE,
        "the native fixture presents natively");
    ok &= expect_incomplete_coverage_rejected(native_frame,
        "a NATIVE record missing a displayed row is invalid");

    // Insufficient history, and ED 3 after growth, leave structural slots.
    Receiver_session short_history = start_receiver_session(2, QByteArrayLiteral("a\r\n$ "), false);
    resize_rows(short_history, 6);
    ok &= expect_receiver_matches_desktop(surface, short_history.session->latest_render_snapshot_handle(),
        "the receiver presents unused space above short history like the desktop");
    (void)automatic.session->scroll_viewport_lines(-3);
    automatic.write_output(QByteArrayLiteral("\x1b[3J"));
    ok &= expect_receiver_matches_desktop(surface, automatic.session->latest_render_snapshot_handle(),
        "the receiver presents ED 3 after growth like the desktop");

    // Companion P covers styled and attribute-only vacancy.
    Receiver_session styled = start_receiver_session(2, numbered_lines(13) + QByteArrayLiteral("$ "), true);
    resize_rows(styled, 6);
    styled.write_output(QByteArrayLiteral("\x1b[7m\x1b[J\x1b[0m"));
    const auto styled_frame = styled.session->latest_render_snapshot_handle();
    ok &= check(styled_frame && styled_frame->viewport.presentation.hidden_rows == 4,
        "Companion hides the styled vacancy");
    ok &= expect_receiver_matches_desktop(surface, styled_frame,
        "the receiver presents styled Companion vacancy like the desktop");
    Receiver_session attributed = start_receiver_session(2, numbered_lines(13) + QByteArrayLiteral("$ "), true);
    resize_rows(attributed, 6);
    attributed.write_output(QByteArrayLiteral("\x1b[3;1;6;20;7$r"));
    const auto attributed_frame = attributed.session->latest_render_snapshot_handle();
    ok &= check(attributed_frame && attributed_frame->viewport.presentation.hidden_rows == 4,
        "Companion hides attribute-only vacancy");
    ok &= expect_receiver_matches_desktop(surface, attributed_frame,
        "the receiver presents attribute-only Companion vacancy like the desktop");

    // A frozen frame shows its missing leading prefix as unused slots.
    Receiver_session frozen = start_receiver_session(2, numbered_lines(13) + QByteArrayLiteral("$ "), false);
    frozen.write_output(QByteArrayLiteral("\x1b[?2026h"));
    resize_rows(frozen, 6);
    const auto frozen_frame = frozen.session->latest_render_snapshot_handle();
    ok &= check(frozen_frame && frozen_frame->frozen_basis,
        "the held resize publishes a frozen frame");
    ok &= expect_receiver_matches_desktop(surface, frozen_frame,
        "the receiver presents the frozen frame's unused slots like the desktop");
    frozen.write_output(QByteArrayLiteral("\x1b[?2026l"));
    return ok;
}

bool test_historical_styles_do_not_prevent_small_visible_frame_export()
{
    bool                        ok = true;
    VNM_TerminalSurface          surface;
    term::Terminal_screen_model model({{2, 8}, 4, 4});

    const auto overwrite_color = [
            &model
        ](
            int index)
        {
            model.ingest(
                QByteArrayLiteral("\r\x1b[38;2;") +
                QByteArray::number(index / 256) + ';' +
                QByteArray::number(index % 256) +
                QByteArrayLiteral(";73mX"));
        };
    for (int index = 0; index < 255; ++index) {
        overwrite_color(index);
    }
    model.ingest(QByteArrayLiteral("\r\x1b[0mOK"));

    const auto export_model = [
            &
        ](
            std::uint64_t sequence,
            const char* phase)
        {
            auto snapshot = std::make_shared<term::Terminal_render_snapshot>(
                model.render_snapshot(sequence));
            const auto validation = term::validate_render_snapshot(*snapshot);
            std::set<term::Terminal_style_id> visible_styles;
            for (const auto& cell : snapshot->cells) {
                visible_styles.insert(cell.style_id);
            }
            const auto stats = model.style_table_stats();
            std::cout << "HISTORICAL_STYLE_REPRO phase=" << phase
                      << " rows=" << snapshot->grid_size.rows
                      << " columns=" << snapshot->grid_size.columns
                      << " styles=" << snapshot->styles.size()
                      << " visible_styles=" << visible_styles.size()
                      << " compactions=" << stats.compaction_count
                      << " source_valid="
                      << (validation.status == term::Terminal_render_snapshot_status::OK)
                      << '\n';
            ok &= check(
                validation.status == term::Terminal_render_snapshot_status::OK &&
                visible_styles.size() == 1U && stats.compaction_count == 0U,
                "normal parser output leaves a valid one-style visible frame without compaction");
            const auto result = export_snapshot(surface, std::move(snapshot));
            std::cout << "HISTORICAL_STYLE_REPRO phase=" << phase
                      << " export_ok="
                      << (result.status == vnm_terminal::Terminal_canvas_export_status::OK)
                      << " style_limit="
                      << (result.status == vnm_terminal::Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED)
                      << " frame=" << (result.frame != nullptr) << '\n';
            return result;
        };

    const auto before = export_model(1U, "before_limit");
    ok &= check(
        before.status == vnm_terminal::Terminal_canvas_export_status::OK,
        "small visible frame exports before historical style count crosses the canvas limit");

    overwrite_color(255);
    model.ingest(QByteArrayLiteral("\r\x1b[0mOK"));
    const auto after = export_model(2U, "after_limit");
    ok &= check(
        after.status == vnm_terminal::Terminal_canvas_export_status::OK && after.frame,
        "unused historical styles must not prevent the unchanged small visible frame from exporting");

    const auto idle = export_model(3U, "idle_repeat");
    ok &= check(
        idle.status == vnm_terminal::Terminal_canvas_export_status::OK && idle.frame,
        "the same valid visible frame remains exportable without further terminal output");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--historical-style-repro"))) {
        return test_historical_styles_do_not_prevent_small_visible_frame_export() ? 0 : 1;
    }
    bool            ok = true;
    ok &= check(terminal_canvas_public_header_contract(),
        "public header compiles independently and publishes exact bounds");
    ok &= check(terminal_canvas_public_export_header_contract(),
        "full-only exporter header compiles independently");
    ok &= test_no_frame_and_invalid_source_are_explicit();
    ok &= test_frame_preserves_canvas_authority_and_prior_immutability();
    ok &= test_content_extent_uses_semantic_cells_cursor_and_viewport();
    ok &= test_exact_bounds_and_one_over_fail_closed();
    ok &= test_sparse_style_ids_preserve_colors_and_attributes();
    ok &= test_wide_style_export_is_explicit_and_lossless();
    ok &= test_historical_styles_do_not_prevent_small_visible_frame_export();
    ok &= test_text_bounds_fail_closed_without_truncation();
    ok &= test_row_images_export_with_publication();
    ok &= test_leading_rows_preserve_base_frame_and_bound_the_attachment();
    ok &= test_receiver_presents_through_the_producer();
    ok &= test_required_prefix_export_failures_are_reported();
    return ok ? 0 : 1;
}
