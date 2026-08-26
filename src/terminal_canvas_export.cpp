#include "vnm_terminal/terminal_canvas_frame.h"

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QThread>
#include <utility>

namespace term = vnm_terminal::internal;

namespace {

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

} // namespace

vnm_terminal::Terminal_canvas_export_result
vnm_terminal::export_terminal_canvas_frame(const VNM_TerminalSurface& surface)
{
    if (surface.thread() != QThread::currentThread()) {
        return {Terminal_canvas_export_status::WRONG_THREAD, {}};
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (snapshot == nullptr) {
        return {Terminal_canvas_export_status::NO_FRAME, {}};
    }

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
    if (snapshot->cells.size() > k_terminal_canvas_max_cells) {
        return {Terminal_canvas_export_status::CELL_LIMIT_EXCEEDED, {}};
    }
    if (snapshot->styles.size() > k_terminal_canvas_max_styles) {
        return {Terminal_canvas_export_status::STYLE_LIMIT_EXCEEDED, {}};
    }

    auto frame = std::make_shared<Terminal_canvas_frame>();
    frame->rows                        = snapshot->grid_size.rows;
    frame->columns                     = snapshot->grid_size.columns;
    frame->sequence                    = snapshot->metadata.sequence;
    frame->publication_generation      = snapshot->metadata.publication_generation;
    frame->row_origin_generation       = snapshot->metadata.row_origin_generation;
    frame->default_foreground_rgba      = snapshot->color_state.default_foreground_rgba;
    frame->default_background_rgba      = snapshot->color_state.default_background_rgba;
    frame->cursor_rgba                  = snapshot->color_state.cursor_rgba;
    frame->reverse_video                = snapshot->modes.reverse_video;
    frame->cursor.row                   = snapshot->cursor.position.row;
    frame->cursor.column                = snapshot->cursor.position.column;
    frame->cursor.shape                 = canvas_cursor_shape(snapshot->cursor.shape);
    frame->cursor.visible               = snapshot->cursor.visible;
    frame->cursor.blink_enabled         = snapshot->cursor.blink_enabled;

    frame->styles.reserve(snapshot->styles.size());
    for (const term::Terminal_text_style& source_style : snapshot->styles) {
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

    frame->cells.reserve(snapshot->cells.size());
    for (const term::Terminal_render_cell& source_cell : snapshot->cells) {
        if (source_cell.wide_continuation) {
            continue;
        }

        QString text;
        source_cell.text.append_to(text);
        frame->cells.push_back({
            source_cell.position.row,
            source_cell.position.column,
            source_cell.display_width,
            static_cast<std::uint16_t>(source_cell.style_id),
            std::move(text),
        });
    }

    return {
        Terminal_canvas_export_status::OK,
        std::move(frame),
    };
}
