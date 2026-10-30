#include "vnm_terminal/terminal_canvas_export.h"

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/qt_window_metrics.h"
#include "vnm_terminal/internal/terminal_ascii_cell_text.h"
#include "vnm_terminal/internal/terminal_canvas_content_extent.h"
#include "vnm_terminal/internal/terminal_content_tail_projection.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QByteArray>
#include <QFontInfo>
#include <QThread>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

static_assert(
    vnm_terminal::k_terminal_canvas_image_column_limit ==
        term::k_terminal_image_slice_column_limit);

std::uint16_t canvas_color_reference(const term::Terminal_color_ref& color)
{
    switch (color.kind) {
        case term::Terminal_color_ref_kind::DEFAULT:
            return vnm_terminal::k_terminal_canvas_color_default;
        case term::Terminal_color_ref_kind::PALETTE_INDEX:
            return color.palette_index;
        case term::Terminal_color_ref_kind::RGB:
        default:
            return vnm_terminal::k_terminal_canvas_color_rgba;
    }
}

vnm_terminal::Terminal_canvas_cursor_shape canvas_cursor_shape(
    term::Terminal_cursor_shape shape)
{
    using Canvas_shape = vnm_terminal::Terminal_canvas_cursor_shape;
    switch (shape) {
        case term::Terminal_cursor_shape::BLOCK:     return Canvas_shape::BLOCK;
        case term::Terminal_cursor_shape::BAR:       return Canvas_shape::BAR;
        case term::Terminal_cursor_shape::UNDERLINE: return Canvas_shape::UNDERLINE;
    }

    return Canvas_shape::BLOCK;
}

vnm_terminal::Terminal_canvas_export_result export_snapshot_frame(
    const VNM_TerminalSurface& surface,
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot,
    bool allow_wide_styles,
    int exported_rows)
{
    using namespace vnm_terminal;
    if (term::validate_render_snapshot(*snapshot).status !=
        term::Terminal_render_snapshot_status::OK)
    {
        return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
    }

    if (snapshot->grid_size.rows > k_terminal_canvas_max_rows) {
        return {Terminal_canvas_export_status::ROW_LIMIT_EXCEEDED, {}};
    }
    if (snapshot->grid_size.columns > k_terminal_canvas_max_columns) {
        return {Terminal_canvas_export_status::COLUMN_LIMIT_EXCEEDED, {}};
    }
    if (!terminal_canvas_grid_fits_cell_budget(
            snapshot->grid_size.rows,
            snapshot->grid_size.columns) ||
        snapshot->cells.size() > k_terminal_canvas_max_cells)
    {
        return {Terminal_canvas_export_status::CELL_LIMIT_EXCEEDED, {}};
    }
    // A render snapshot also retains styles used only by earlier output.
    // The bounded canvas palette belongs to the cells this frame exports.
    std::vector<int> exported_style_ids(snapshot->styles.size(), -1);
    std::vector<term::Terminal_style_id> referenced_styles{
        term::k_default_terminal_style_id,
    };
    exported_style_ids[term::k_default_terminal_style_id] = 0;
    const std::size_t style_limit = allow_wide_styles
        ? k_terminal_canvas_max_wide_styles
        : k_terminal_canvas_max_styles;
    for (const term::Terminal_render_cell& source_cell : snapshot->cells) {
        if (source_cell.wide_continuation || source_cell.position.row >= exported_rows) {
            continue;
        }
        int& exported_style_id = exported_style_ids[source_cell.style_id];
        if (exported_style_id >= 0) {
            continue;
        }
        if (referenced_styles.size() == style_limit) {
            return {Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED, {}};
        }
        exported_style_id = static_cast<int>(referenced_styles.size());
        referenced_styles.push_back(source_cell.style_id);
    }

    auto frame = std::make_shared<Terminal_canvas_frame>();
    const term::terminal_cell_metrics_t cell_metrics =
        term::VNM_TerminalSurface_render_bridge::cell_metrics(surface);
    if (!term::is_valid_cell_metrics(cell_metrics) ||
        !std::isfinite(surface.width()) || surface.width() <= 0.0 ||
        !std::isfinite(surface.height()) || surface.height() <= 0.0)
    {
        return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
    }
    frame->rows                        = snapshot->grid_size.rows;
    frame->columns                     = snapshot->grid_size.columns;
    frame->font_size                   = surface.font_size();
    const QFontInfo active_font(term::vnm_terminal_font(
        surface.font_family(),
        surface.effective_font_size(),
        term::window_logical_dpi(surface.window())));
    frame->font_family                 = active_font.family();
    frame->font_style                  = active_font.styleName();
    frame->font_weight                 = active_font.weight();
    frame->font_italic                 = active_font.italic();
    frame->cell_width                  = cell_metrics.width;
    frame->cell_height                 = cell_metrics.height;
    frame->content_width               = surface.width();
    frame->content_height              = surface.height();
    frame->sequence                    = snapshot->metadata.sequence;
    frame->publication_generation      = snapshot->metadata.publication_generation;
    frame->row_origin_generation       = snapshot->metadata.row_origin_generation;
    if (snapshot->mouse_input &&
        snapshot->mouse_input->source.rows == frame->rows &&
        snapshot->mouse_input->source.columns == frame->columns &&
        snapshot->mouse_input->source.row_origin_generation == frame->row_origin_generation &&
        snapshot->mouse_input->source.scrollback_rows == snapshot->viewport.scrollback_rows)
    {
        frame->mouse_input = snapshot->mouse_input;
    }
    frame->default_foreground_rgba      = snapshot->color_state.default_foreground_rgba;
    frame->default_background_rgba      = snapshot->color_state.default_background_rgba;
    frame->cursor_rgba                  = snapshot->color_state.cursor_rgba;
    frame->reverse_video                = snapshot->modes.reverse_video;
    frame->cursor.row                   = snapshot->cursor.position.row;
    frame->cursor.column                = snapshot->cursor.position.column;
    frame->cursor.shape                 = canvas_cursor_shape(snapshot->cursor.shape);
    frame->cursor.visible               = snapshot->cursor.visible;
    frame->cursor.blink_enabled         = snapshot->cursor.blink_enabled;
    if (snapshot->touch_selection) {
        frame->selection = *snapshot->touch_selection;
        const auto colors = surface.touch_selection_projection();
        frame->selection->background_rgba = colors.background_rgba;
        frame->selection->foreground_rgba = colors.foreground_rgba;
    }

    if (referenced_styles.size() > k_terminal_canvas_max_styles) {
        frame->wide_styles.emplace();
    }
    frame->styles.reserve(referenced_styles.size());
    frame->color_references.emplace();
    frame->color_references->styles.reserve(referenced_styles.size());
    for (const term::Terminal_style_id source_style_id : referenced_styles) {
        const term::Terminal_text_style& source_style = snapshot->styles[source_style_id];
        frame->color_references->styles.push_back({
            canvas_color_reference(source_style.foreground),
            canvas_color_reference(source_style.background),
        });
        frame->styles.push_back({
            term::resolve_terminal_color_ref(
                source_style.foreground,
                snapshot->color_state,
                true),
            term::resolve_terminal_color_ref(
                source_style.background,
                snapshot->color_state,
                false),
            source_style.attributes,
        });
    }

    std::size_t frame_text_utf8_bytes = 0U;
    int         occupied_bottom       = 0;
    frame->cells.reserve(snapshot->cells.size());
    for (const term::Terminal_render_cell& source_cell : snapshot->cells) {
        if (source_cell.wide_continuation || source_cell.position.row >= exported_rows) {
            continue;
        }

        if (source_cell.text.code_unit_count() >
            k_terminal_canvas_max_cell_text_utf16_code_units)
        {
            return {
                Terminal_canvas_export_status::
                    CELL_TEXT_UTF16_CODE_UNIT_LIMIT_EXCEEDED,
                {},
            };
        }

        const auto printable_ascii = source_cell.text.single_printable_ascii_char();
        QString text = printable_ascii
            ? term::printable_ascii_cell_text(*printable_ascii)
            : source_cell.text.to_qstring();
        const auto counted_text_bytes = terminal_canvas_text_utf8_size(text);
        const std::size_t cell_text_utf8_bytes =
            static_cast<std::size_t>(counted_text_bytes ? *counted_text_bytes : text.toUtf8().size());
        if (cell_text_utf8_bytes >
            static_cast<std::size_t>(k_terminal_canvas_max_frame_text_utf8_bytes) -
                frame_text_utf8_bytes)
        {
            return {
                Terminal_canvas_export_status::FRAME_TEXT_UTF8_BYTE_LIMIT_EXCEEDED,
                {},
            };
        }
        frame_text_utf8_bytes += cell_text_utf8_bytes;
        occupied_bottom = source_cell.position.row + 1;
        frame->cells.push_back({
            source_cell.position.row,
            source_cell.position.column,
            source_cell.display_width,
            static_cast<std::uint16_t>(exported_style_ids[source_cell.style_id]),
            std::move(text),
        });
    }

    terminal_canvas_content_extent_t content_extent;
    content_extent.content_bottom_row_exclusive =
        term::terminal_canvas_content_bottom_row_exclusive(
            snapshot->grid_size.rows,
            occupied_bottom,
            snapshot->cursor.visible ? snapshot->cursor.position.row : -1);
    content_extent.scrollback_rows = snapshot->viewport.scrollback_rows;
    content_extent.offset_from_tail = snapshot->viewport.offset_from_tail;
    content_extent.active_buffer =
        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
        ? Terminal_canvas_buffer::ALTERNATE_BUFFER
        : Terminal_canvas_buffer::PRIMARY_BUFFER;
    frame->content_extent = content_extent;

    frame->images.emplace();
    if (!snapshot->visible_row_images.empty()) {
        if (snapshot->visible_row_images.size() != static_cast<std::size_t>(frame->rows)) {
            frame->images->status = Terminal_canvas_images_status::INVALID;
        }
        else {
            for (int row = 0; row < exported_rows; ++row) {
                const auto& slice = snapshot->visible_row_images[static_cast<std::size_t>(row)];
                if (slice == nullptr) {
                    continue;
                }
                frame->images->rows.push_back({
                    row,
                    slice->first_column,
                    slice->cell_pixel_size.width,
                    slice->cell_pixel_size.height,
                    slice->revision,
                    slice->pixels,
                });
            }
            if (!terminal_canvas_images_are_valid(*frame->images, frame->rows)) {
                frame->images->status = Terminal_canvas_images_status::INVALID;
                frame->images->rows.clear();
            }
        }
    }

    return {
        Terminal_canvas_export_status::OK,
        std::move(frame),
    };
}

vnm_terminal::Terminal_canvas_presentation canvas_presentation(
    const term::Terminal_render_snapshot& snapshot)
{
    using namespace vnm_terminal;
    Terminal_canvas_presentation presentation;
    presentation.policy = snapshot.viewport.presentation.policy == term::Terminal_presentation_policy::CONTENT_TAIL
        ? Terminal_canvas_presentation_policy::CONTENT_TAIL
        : Terminal_canvas_presentation_policy::NATIVE;
    presentation.content_end_row_exclusive = snapshot.viewport.presentation.content_end_row_exclusive;
    presentation.clear_page_start_row      = snapshot.viewport.presentation.clear_page_start_row;
    presentation.covered_rows              = snapshot.grid_size.rows - snapshot.uncovered_trailing_rows;
    presentation.visible_rows             = snapshot.presentation_rows > 0
        ? snapshot.presentation_rows : snapshot.grid_size.rows;
    presentation.offset_from_tail         = snapshot.presentation_offset_from_tail >= 0
        ? snapshot.presentation_offset_from_tail : snapshot.viewport.offset_from_tail;
    presentation.frozen                   = snapshot.frozen_basis;
    presentation.logical_cursor_visible   = snapshot.modes.cursor_visible;
    return presentation;
}

} // namespace

vnm_terminal::Terminal_canvas_export_result
vnm_terminal::export_terminal_canvas_frame(const VNM_TerminalSurface& surface)
{
    return export_terminal_canvas_frame(surface, false);
}

vnm_terminal::Terminal_canvas_export_result
vnm_terminal::export_terminal_canvas_frame(
    const VNM_TerminalSurface& surface,
    bool allow_wide_styles)
{
    if (surface.thread() != QThread::currentThread()) {
        return {Terminal_canvas_export_status::WRONG_THREAD, {}};
    }
    const auto snapshot = term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (snapshot == nullptr) {
        return {Terminal_canvas_export_status::NO_FRAME, {}};
    }
    const auto authoritative_grid = term::grid_size_for_geometry(
        surface.size(), term::VNM_TerminalSurface_render_bridge::cell_metrics(surface));
    if (authoritative_grid.status != term::Terminal_metrics_status::OK ||
        authoritative_grid.grid_size.rows != snapshot->grid_size.rows ||
        authoritative_grid.grid_size.columns != snapshot->grid_size.columns)
    {
        return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
    }
    const auto presentation = canvas_presentation(*snapshot);
    if (!term::render_snapshot_presentable(
            *snapshot, presentation.visible_rows, presentation.offset_from_tail))
    {
        return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
    }
    auto result = export_snapshot_frame(surface, snapshot, allow_wide_styles, snapshot->grid_size.rows);
    if (result.status != Terminal_canvas_export_status::OK) {
        return result;
    }
    auto frame = std::make_shared<Terminal_canvas_frame>(*result.frame);
    if (snapshot->row_coverage_snapshot) {
        if (!term::render_snapshot_row_coverage_is_coherent(*snapshot)) {
            return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
        }
        const auto& source_coverage = snapshot->row_coverage_snapshot;
        const auto exported = export_snapshot_frame(
            surface, source_coverage, allow_wide_styles, source_coverage->grid_size.rows);
        if (exported.status != Terminal_canvas_export_status::OK) {
            return exported;
        }
        Terminal_canvas_row_coverage coverage;
        coverage.first_logical_row = term::render_snapshot_first_visible_logical_row(*source_coverage);
        coverage.rows              = source_coverage->grid_size.rows;
        coverage.styles            = exported.frame->styles;
        coverage.color_references  = exported.frame->color_references;
        coverage.cells             = exported.frame->cells;
        coverage.images            = exported.frame->images;
        coverage.selection         = exported.frame->selection;
        coverage.wide_styles       = exported.frame->wide_styles;
        if (!terminal_canvas_row_coverage_is_valid(
                coverage, frame->rows, frame->columns, snapshot->viewport.scrollback_rows))
        {
            return {Terminal_canvas_export_status::INVALID_SOURCE_FRAME, {}};
        }
        frame->row_coverage = std::move(coverage);
    }
    frame->presentation = presentation;
    result.frame = std::move(frame);
    return result;
}
