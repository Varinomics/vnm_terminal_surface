#pragma once

#include "vnm_terminal/internal/ime_contract.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <set>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace vnm_terminal::internal {

enum class Terminal_cursor_shape
{
    BLOCK,
    BAR,
    UNDERLINE,
};

enum class Terminal_mouse_tracking_mode
{
    NONE,
    BUTTON,
    DRAG,
    ANY,
};

struct Terminal_mode_state
{
    bool                         application_cursor_keys = false;
    bool                         reverse_video           = false;
    bool                         origin_mode             = false;
    bool                         autowrap                = true;
    bool                         cursor_visible          = true;
    Terminal_mouse_tracking_mode mouse_tracking          = Terminal_mouse_tracking_mode::NONE;
    bool                         focus_reporting         = false;
    bool                         sgr_mouse_encoding      = false;
    bool                         alternate_scroll        = false;
    bool                         bracketed_paste         = false;
    bool                         synchronized_output     = false;
};

enum class Terminal_render_snapshot_status
{
    OK,
    INVALID_GRID_SIZE,
    INVALID_CELL_POSITION,
    INVALID_CELL_WIDTH,
    INVALID_CELL_OVERLAP,
    INVALID_WIDE_CELL_CONTINUATION,
    INVALID_WIDE_CELL_STYLE,
    INVALID_STYLE_ID,
    INVALID_CURSOR_POSITION,
    INVALID_VIEWPORT,
    INVALID_DIRTY_ROW_RANGE,
    INVALID_SELECTION_SPAN,
    INVALID_LINE_PROVENANCE,
    INVALID_HYPERLINK_METADATA,
};

struct Terminal_render_cell
{
    terminal_grid_position_t   position;
    QString                    text;
    std::uint64_t              hyperlink_id      = 0U;
    int                        display_width     = 1;
    bool                       wide_continuation = false;
    Terminal_style_id          style_id          = k_default_terminal_style_id;
};

struct Terminal_render_cursor
{
    terminal_grid_position_t   position;
    Terminal_cursor_shape      shape         = Terminal_cursor_shape::BLOCK;
    bool                       visible       = true;
    bool                       blink_enabled = true;
};

struct Terminal_render_dirty_row_range
{
    int                        first_row = 0;
    int                        row_count = 0;
};

struct Terminal_render_selection_span
{
    Terminal_selection_range   source_range;
    int                        row          = 0;
    int                        first_column = 0;
    int                        column_count = 0;
};

struct Terminal_render_line_provenance
{
    std::int64_t               logical_row        = 0;
    std::uint64_t              retained_line_id   = 0U;
    std::uint64_t              content_generation = 0U;
};

struct Terminal_render_selection_request
{
    Terminal_selection_range                         range;
    std::vector<terminal_selection_line_lease_t>     expected_lines;
};

inline bool operator==(
    const Terminal_render_line_provenance& left,
    const Terminal_render_line_provenance& right)
{
    return
        left.logical_row        == right.logical_row        &&
        left.retained_line_id   == right.retained_line_id   &&
        left.content_generation == right.content_generation;
}

struct Terminal_render_hyperlink_metadata
{
    std::uint64_t              hyperlink_id                 = 0U;
    QByteArray                 identity_key;
    QByteArray                 uri;
};

struct Terminal_render_metadata
{
    std::uint64_t              sequence                     = 0U;
    std::uint64_t              row_origin_generation         = 0U;
    bool                       backend_geometry_in_sync     = true;
    bool                       visual_bell_active           = false;
    bool                       mouse_reporting_mode_changed = false;
};

struct Terminal_render_snapshot_request
{
    std::uint64_t                         sequence = 0U;
    std::uint64_t                         row_origin_generation = 0U;
    Terminal_viewport_state               viewport;
    std::vector<int>                      dirty_rows;
    bool                                  viewport_changed = false;
    Terminal_cursor_shape                 cursor_shape = Terminal_cursor_shape::BLOCK;
    bool                                  cursor_blink_enabled = true;
    Ime_preedit_state                     ime_preedit;
    std::vector<Terminal_render_selection_request> selections;
    bool                                  backend_geometry_in_sync = true;
    bool                                  visual_bell_active = false;
    bool                                  mouse_reporting_mode_changed = false;
};

struct Terminal_render_snapshot
{
    terminal_grid_size_t                               grid_size;
    Terminal_viewport_state                            viewport;
    Terminal_color_state                               color_state;
    std::vector<Terminal_text_style>                   styles;
    std::vector<Terminal_render_cell>                  cells;
    std::vector<Terminal_render_line_provenance>       visible_line_provenance;
    std::vector<Terminal_render_dirty_row_range>       dirty_row_ranges;
    std::vector<Terminal_render_hyperlink_metadata>    hyperlinks;
    Terminal_render_cursor                             cursor;
    Ime_preedit_state                                  ime_preedit;
    std::vector<Terminal_render_selection_span>        selection_spans;
    Terminal_render_metadata                           metadata;
    Terminal_mode_state                                modes;
};

struct Terminal_render_snapshot_validation
{
    Terminal_render_snapshot_status status = Terminal_render_snapshot_status::OK;
};

inline Terminal_render_snapshot make_empty_render_snapshot(
    terminal_grid_size_t       grid_size,
    Terminal_viewport_state    viewport,
    std::uint64_t              sequence)
{
    Terminal_render_snapshot snapshot;
    snapshot.grid_size             = grid_size;
    snapshot.viewport              = viewport;
    snapshot.viewport.visible_rows = grid_size.rows > 0 ? grid_size.rows : viewport.visible_rows;
    snapshot.metadata.sequence     = sequence;
    snapshot.styles.push_back(make_default_terminal_text_style());
    return snapshot;
}

inline std::vector<Terminal_render_dirty_row_range> compact_dirty_row_ranges(
    std::vector<int>           dirty_rows,
    int                        visible_rows,
    bool                       full_repaint)
{
    if (visible_rows <= 0) {
        return {};
    }

    if (full_repaint) {
        return {{0, visible_rows}};
    }

    std::sort(dirty_rows.begin(), dirty_rows.end());
    dirty_rows.erase(std::unique(dirty_rows.begin(), dirty_rows.end()), dirty_rows.end());

    std::vector<Terminal_render_dirty_row_range> ranges;
    for (int row : dirty_rows) {
        if (row < 0 || row >= visible_rows) {
            continue;
        }

        if (!ranges.empty() &&
            ranges.back().first_row + ranges.back().row_count == row)
        {
            ++ranges.back().row_count;
            continue;
        }

        ranges.push_back({row, 1});
    }

    return ranges;
}

inline terminal_grid_position_t normalized_selection_start(
    const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.start;
    }

    if (range.start.row > range.end.row) {
        return range.end;
    }

    return range.start.column <= range.end.column ? range.start : range.end;
}

inline terminal_grid_position_t normalized_selection_end(
    const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.end;
    }

    if (range.start.row > range.end.row) {
        return range.start;
    }

    return range.start.column <= range.end.column ? range.end : range.start;
}

inline std::size_t normalized_selection_row_count(
    const Terminal_selection_range& range)
{
    const terminal_grid_position_t start = normalized_selection_start(range);
    const terminal_grid_position_t end   = normalized_selection_end(range);
    const std::int64_t row_count =
        static_cast<std::int64_t>(end.row) - static_cast<std::int64_t>(start.row) + 1;
    return row_count > 0 ? static_cast<std::size_t>(row_count) : 0U;
}

inline int render_snapshot_first_visible_logical_row(
    const Terminal_render_snapshot& snapshot)
{
    return snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
        ? 0
        : snapshot.viewport.scrollback_rows - snapshot.viewport.offset_from_tail;
}

inline bool render_snapshot_visible_line_provenance_is_valid(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0) {
        return false;
    }

    if (snapshot.visible_line_provenance.size() !=
        static_cast<std::size_t>(snapshot.grid_size.rows))
    {
        return false;
    }

    const std::int64_t first_visible_logical_row =
        render_snapshot_first_visible_logical_row(snapshot);
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
        const std::int64_t expected_logical_row =
            first_visible_logical_row + static_cast<std::int64_t>(row);
        if (provenance.logical_row != expected_logical_row ||
            provenance.retained_line_id == 0U)
        {
            return false;
        }
    }

    return true;
}

inline void suppress_selection_spans_without_valid_line_provenance(
    Terminal_render_snapshot& snapshot)
{
    if (!snapshot.selection_spans.empty() &&
        !render_snapshot_visible_line_provenance_is_valid(snapshot))
    {
        snapshot.selection_spans.clear();
    }
}

inline std::vector<const Terminal_render_cell*> render_snapshot_cells_by_position(
    const Terminal_render_snapshot& snapshot)
{
    std::vector<const Terminal_render_cell*> cells_by_position(
        static_cast<std::size_t>(snapshot.grid_size.rows) *
            static_cast<std::size_t>(snapshot.grid_size.columns),
        nullptr);
    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row    <  0                          ||
            cell.position.row    >= snapshot.grid_size.rows     ||
            cell.position.column <  0                          ||
            cell.position.column >= snapshot.grid_size.columns)
        {
            continue;
        }

        const std::size_t index =
            static_cast<std::size_t>(cell.position.row) *
                static_cast<std::size_t>(snapshot.grid_size.columns) +
            static_cast<std::size_t>(cell.position.column);
        cells_by_position[index] = &cell;
    }
    return cells_by_position;
}

inline const Terminal_render_cell* render_snapshot_cell_at(
    const std::vector<const Terminal_render_cell*>& cells_by_position,
    terminal_grid_size_t                            grid_size,
    int                                             row,
    int                                             column)
{
    const std::size_t index =
        static_cast<std::size_t>(row) *
            static_cast<std::size_t>(grid_size.columns) +
        static_cast<std::size_t>(column);
    return cells_by_position[index];
}

inline QString selected_text_from_render_snapshot_row(
    const Terminal_render_snapshot& snapshot,
    const std::vector<const Terminal_render_cell*>&
                                    cells_by_position,
    int                             viewport_row,
    int                             first_column,
    int                             end_column,
    bool                            trim_trailing_spaces)
{
    QString text;
    for (int column = first_column; column < end_column; ++column) {
        const Terminal_render_cell* cell =
            render_snapshot_cell_at(
                cells_by_position,
                snapshot.grid_size,
                viewport_row,
                column);
        if (cell == nullptr) {
            text += QLatin1Char(' ');
            continue;
        }

        if (cell->wide_continuation) {
            continue;
        }

        text += cell->text;
    }

    if (trim_trailing_spaces) {
        while (text.endsWith(QLatin1Char(' '))) {
            text.chop(1);
        }
    }
    return text;
}

inline Terminal_selection_result selected_text_from_render_snapshot(
    const Terminal_render_snapshot& snapshot,
    const Terminal_selection_range& selection)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0 ||
        selection.mode == Terminal_selection_mode::NONE)
    {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const terminal_grid_position_t start = normalized_selection_start(selection);
    const terminal_grid_position_t end   = normalized_selection_end(selection);
    const int first_visible_logical_row  = render_snapshot_first_visible_logical_row(snapshot);

    if (start.row    < first_visible_logical_row                            ||
        end.row      >= first_visible_logical_row + snapshot.grid_size.rows ||
        start.column < 0                                                    ||
        start.column > snapshot.grid_size.columns                           ||
        end.column   < 0                                                    ||
        end.column   > snapshot.grid_size.columns)
    {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const std::vector<const Terminal_render_cell*> cells_by_position =
        render_snapshot_cells_by_position(snapshot);
    QString text;
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int first_column = logical_row == start.row ? start.column : 0;
        const int end_column =
            logical_row == end.row ? end.column : snapshot.grid_size.columns;
        if (end_column < first_column) {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        if (logical_row > start.row) {
            text += QLatin1Char('\n');
        }

        text += selected_text_from_render_snapshot_row(
            snapshot,
            cells_by_position,
            logical_row - first_visible_logical_row,
            first_column,
            end_column,
            end_column == snapshot.grid_size.columns);
    }

    return {Terminal_selection_result_code::OK, text};
}

inline Terminal_render_snapshot_validation validate_render_snapshot(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0) {
        return {Terminal_render_snapshot_status::INVALID_GRID_SIZE};
    }

    if (snapshot.styles.empty()) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (!(snapshot.styles.front() == make_default_terminal_text_style())) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (snapshot.viewport.visible_rows             <= 0                                 ||
        snapshot.viewport.visible_rows             != snapshot.grid_size.rows           ||
        snapshot.viewport.scrollback_rows          <  0                                 ||
        snapshot.viewport.offset_from_tail         <  0                                 ||
        snapshot.viewport.offset_from_tail         >  snapshot.viewport.scrollback_rows ||
        (snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE &&
            (snapshot.viewport.scrollback_rows != 0 ||
                snapshot.viewport.offset_from_tail != 0)))
    {
        return {Terminal_render_snapshot_status::INVALID_VIEWPORT};
    }

    if (!snapshot.visible_line_provenance.empty() &&
        !render_snapshot_visible_line_provenance_is_valid(snapshot))
    {
        return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
    }

    if (snapshot.visible_line_provenance.empty() && !snapshot.selection_spans.empty()) {
        return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
    }

    int next_dirty_row = 0;
    for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        if (range.first_row < 0 || range.row_count <= 0 || range.first_row < next_dirty_row ||
            range.first_row + range.row_count > snapshot.grid_size.rows)
        {
            return {Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE};
        }
        next_dirty_row = range.first_row + range.row_count;
    }

    std::set<std::uint64_t> hyperlink_ids;
    for (const Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id                     == 0U ||
            hyperlink_ids.find(hyperlink.hyperlink_id) != hyperlink_ids.end())
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }
        hyperlink_ids.insert(hyperlink.hyperlink_id);
    }

    for (std::size_t i = 0; i < snapshot.cells.size(); ++i) {
        const Terminal_render_cell& cell = snapshot.cells[i];
        if (cell.position.row    < 0  || cell.position.row    >= snapshot.grid_size.rows ||
            cell.position.column < 0  || cell.position.column >= snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_POSITION};
        }

        if (static_cast<std::size_t>(cell.style_id) >= snapshot.styles.size()) {
            return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
        }

        for (std::size_t j = 0; j < i; ++j) {
            const Terminal_render_cell& previous = snapshot.cells[j];
            if (previous.position.row    == cell.position.row &&
                previous.position.column == cell.position.column)
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
            }
        }

        if (cell.wide_continuation) {
            if (!cell.text.isEmpty() || cell.display_width != 0 || cell.position.column == 0) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }
            continue;
        }

        if (cell.display_width <= 0 ||
            cell.display_width >  snapshot.grid_size.columns - cell.position.column)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_WIDTH};
        }
    }

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.wide_continuation) {
            const Terminal_render_cell* covering_base       = nullptr;
            int                         covering_base_count = 0;
            for (const Terminal_render_cell& base_cell : snapshot.cells) {
                if (base_cell.wide_continuation || base_cell.position.row != cell.position.row) {
                    continue;
                }

                if (base_cell.position.column                        < cell.position.column &&
                    cell.position.column - base_cell.position.column < base_cell.display_width)
                {
                    covering_base = &base_cell;
                    ++covering_base_count;
                }
            }

            if (covering_base_count != 1) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }

            if (covering_base->style_id     != cell.style_id ||
                covering_base->hyperlink_id != cell.hyperlink_id)
            {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE};
            }
            continue;
        }

        int continuation_count = 0;
        for (const Terminal_render_cell& covered_cell : snapshot.cells) {
            if (covered_cell.position.row                           != cell.position.row    ||
                covered_cell.position.column                        <= cell.position.column ||
                covered_cell.position.column - cell.position.column >= cell.display_width)
            {
                continue;
            }

            if (!covered_cell.wide_continuation) {
                return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
            }

            if (covered_cell.wide_continuation) {
                ++continuation_count;
            }
        }

        if (continuation_count != cell.display_width - 1) {
            return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
        }
    }

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.hyperlink_id                     != 0U &&
            hyperlink_ids.find(cell.hyperlink_id) == hyperlink_ids.end())
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }
    }

    if (snapshot.cursor.visible &&
        (snapshot.cursor.position.row < 0 || snapshot.cursor.position.row >= snapshot.grid_size.rows ||
         snapshot.cursor.position.column <  0 ||
         snapshot.cursor.position.column >= snapshot.grid_size.columns))
    {
        return {Terminal_render_snapshot_status::INVALID_CURSOR_POSITION};
    }

    for (const Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row < 0          || span.row >= snapshot.grid_size.rows ||
            span.first_column < 0 || span.column_count <= 0              ||
            span.first_column + span.column_count > snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_SELECTION_SPAN};
        }
    }

    return {};
}

}
