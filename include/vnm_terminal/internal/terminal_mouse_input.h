#pragma once

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include <QPointF>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>

namespace vnm_terminal::internal {

inline std::optional<terminal_grid_position_t> logical_grid_position_for_viewport_cell(
    const std::shared_ptr<const Terminal_render_snapshot>&   snapshot,
    terminal_grid_position_t                                 viewport_position)
{
    if (snapshot                 == nullptr                  ||
        !is_valid_grid_size(snapshot->grid_size)       ||
        viewport_position.row    <  0                        ||
        viewport_position.row    >= snapshot->grid_size.rows ||
        viewport_position.column <  0                        ||
        viewport_position.column >= snapshot->grid_size.columns)
    {
        return std::nullopt;
    }

    const int first_visible_logical_row =
        snapshot->viewport.active_buffer == Terminal_buffer_id::ALTERNATE
            ? 0
            : snapshot->viewport.scrollback_rows - snapshot->viewport.offset_from_tail;
    if (snapshot->display_source) {
        const int logical = snapshot->display_row_logical_rows[static_cast<std::size_t>(viewport_position.row)];
        return logical >= 0 ? std::optional(terminal_grid_position_t{logical, viewport_position.column}) : std::nullopt;
    }
    return terminal_grid_position_t{
        first_visible_logical_row + viewport_position.row,
        viewport_position.column,
    };
}

// While input follows the committed frame, mouse reports address the native
// screen row it shows: a displayed history row or slot receives none.
inline std::optional<terminal_grid_position_t> native_mouse_position(
    const std::shared_ptr<const Terminal_render_snapshot>& snapshot,
    terminal_grid_position_t position,
    bool follows_committed_frame)
{
    if (!snapshot) {
        return std::nullopt;
    }
    if (!follows_committed_frame || snapshot->viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return position;
    }
    const auto logical = logical_grid_position_for_viewport_cell(snapshot, position);
    if (!logical) {
        return std::nullopt;
    }
    const int row = logical->row - snapshot->viewport.scrollback_rows;
    const int native_rows = snapshot->display_source
        ? snapshot->display_source->grid_size.rows : snapshot->grid_size.rows;
    if (row < 0 || row >= native_rows) {
        return std::nullopt;
    }
    return terminal_grid_position_t{row, position.column};
}

// The recorder supplies this immutable snapshot only after it was drawn.
inline std::optional<terminal_mouse_target_t> terminal_mouse_target(
    const std::shared_ptr<const Terminal_render_snapshot>& snapshot,
    terminal_cell_metrics_t metrics,
    QPointF position)
{
    if (!snapshot || !snapshot->mouse_input || !is_valid_cell_metrics(metrics) ||
        !std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        position.x() < 0.0 || position.y() < 0.0)
    {
        return std::nullopt;
    }
    const auto& record = *snapshot->mouse_input;
    if (record.record_version != k_terminal_canvas_mouse_input_version ||
        position.x() >= snapshot->grid_size.columns * metrics.width ||
        position.y() >= snapshot->grid_size.rows * metrics.height)
    {
        return std::nullopt;
    }
    const terminal_grid_position_t displayed{
        (int)std::floor(position.y() / metrics.height),
        (int)std::floor(position.x() / metrics.width)};
    if (!snapshot->display_source &&
        displayed.row >= snapshot->grid_size.rows - snapshot->uncovered_trailing_rows)
    {
        return std::nullopt;
    }
    const auto native = native_mouse_position(snapshot, displayed, true);
    if (!native || native->row < 0 || native->row >= record.source.rows ||
        native->column >= record.source.columns)
    {
        return std::nullopt;
    }
    return terminal_mouse_target_t{
        record.source, snapshot->metadata.sequence, snapshot->metadata.publication_generation,
        displayed.row, native->row, native->column};
}

} // namespace vnm_terminal::internal
