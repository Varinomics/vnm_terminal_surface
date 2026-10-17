#include "vnm_terminal/internal/terminal_content_tail_projection.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace vnm_terminal::internal {

namespace {

int padding_rows(const Terminal_render_snapshot& snapshot)
{
    return snapshot.viewport.active_buffer == Terminal_buffer_id::PRIMARY &&
        snapshot.native_content_bottom_row_exclusive > 0
        ? snapshot.grid_size.rows - snapshot.native_content_bottom_row_exclusive : 0;
}

}

int content_tail_display_row_to_logical_row(const Terminal_render_snapshot& snapshot, int display_row)
{
    if (display_row < 0 || display_row >= snapshot.grid_size.rows) {
        return -1;
    }
    const int first = render_snapshot_first_visible_logical_row(snapshot);
    const int stream_row = first + display_row;
    const int insertion = snapshot.viewport.scrollback_rows - snapshot.resize_displaced_scrollback_rows;
    const int padding = padding_rows(snapshot);
    if (padding == 0 || stream_row < insertion) {
        return stream_row;
    }
    return stream_row < insertion + padding ? -1 : stream_row - padding;
}

int content_tail_logical_row_to_display_row(const Terminal_render_snapshot& snapshot, int logical_row)
{
    const int insertion = snapshot.viewport.scrollback_rows - snapshot.resize_displaced_scrollback_rows;
    return logical_row - render_snapshot_first_visible_logical_row(snapshot) +
        (logical_row >= insertion ? padding_rows(snapshot) : 0);
}

std::shared_ptr<const Terminal_render_snapshot> make_content_tail_display_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> source)
{
    if (!source || source->display_source || padding_rows(*source) == 0 ||
        !render_snapshot_visible_line_provenance_is_valid(*source) ||
        (source->leading_rows_snapshot &&
            !render_snapshot_visible_line_provenance_is_valid(*source->leading_rows_snapshot)))
    {
        return source;
    }
    auto displayed = std::make_shared<Terminal_render_snapshot>(*source);
    displayed->display_source = source;
    displayed->leading_rows_snapshot.reset();
    displayed->leading_row_count = 0;
    displayed->cells.clear();
    displayed->visible_line_provenance.clear();
    displayed->visible_row_images.assign(static_cast<std::size_t>(source->grid_size.rows), {});
    displayed->selection_spans.clear();
    displayed->search_match_spans.clear();
    displayed->dirty_row_ranges = {{0, source->grid_size.rows}};
    if (displayed->touch_selection) {
        displayed->touch_selection->visible_rows.clear();
        displayed->touch_selection->spans.clear();
        displayed->touch_selection->start.visible = false;
        displayed->touch_selection->end.visible = false;
    }
    const Terminal_render_snapshot* leading = source->leading_rows_snapshot.get();
    const Terminal_render_snapshot_row_content_view native_rows(*source);
    std::optional<Terminal_render_snapshot_row_content_view> leading_rows;
    const auto leading_style_offset = static_cast<Terminal_style_id>(displayed->styles.size());
    std::map<Terminal_hyperlink_id, Terminal_hyperlink_id> leading_hyperlinks;
    if (leading) {
        leading_rows.emplace(*leading);
        displayed->styles.insert(displayed->styles.end(), leading->styles.begin(), leading->styles.end());
        Terminal_hyperlink_id next_id = 1;
        for (const auto& link : displayed->hyperlinks) {
            next_id = std::max(next_id, link.hyperlink_id + 1);
        }
        for (auto link : leading->hyperlinks) {
            leading_hyperlinks.emplace(link.hyperlink_id, next_id);
            link.hyperlink_id = next_id++;
            displayed->hyperlinks.push_back(std::move(link));
        }
    }
    for (int row = 0; row < source->grid_size.rows; ++row) {
        const int logical_row = content_tail_display_row_to_logical_row(*source, row);
        displayed->display_row_logical_rows.push_back(logical_row);
        if (logical_row < 0) {
            Terminal_render_line_provenance padding;
            padding.logical_row = -1;
            displayed->visible_line_provenance.push_back(padding);
            if (displayed->touch_selection) {
                displayed->touch_selection->visible_rows.push_back({});
            }
            continue;
        }
        int source_row = logical_row - render_snapshot_first_visible_logical_row(*source);
        const bool from_leading = source_row < 0;
        if (from_leading && !leading) {
            return source;
        }
        const auto& row_source = from_leading ? *leading : *source;
        if (from_leading) {
            source_row = logical_row - render_snapshot_first_visible_logical_row(row_source);
        }
        const auto cells = (from_leading ? *leading_rows : native_rows).row_at(source_row);
        for (auto cell : cells) {
            cell.position.row = row;
            if (from_leading) {
                cell.style_id += leading_style_offset;
                if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                    cell.hyperlink_id = leading_hyperlinks.at(cell.hyperlink_id);
                }
            }
            displayed->cells.push_back(std::move(cell));
        }
        displayed->visible_line_provenance.push_back(
            row_source.visible_line_provenance[static_cast<std::size_t>(source_row)]);
        if (!row_source.visible_row_images.empty()) {
            displayed->visible_row_images[static_cast<std::size_t>(row)] =
                row_source.visible_row_images[static_cast<std::size_t>(source_row)];
        }
        for (auto span : row_source.selection_spans) {
            if (span.row == source_row) {
                span.row = row;
                displayed->selection_spans.push_back(std::move(span));
            }
        }
        for (auto span : row_source.search_match_spans) {
            if (span.row == source_row) {
                span.row = row;
                displayed->search_match_spans.push_back(std::move(span));
            }
        }
        if (displayed->touch_selection && row_source.touch_selection) {
            auto& touch = *displayed->touch_selection;
            const auto& original = *row_source.touch_selection;
            touch.visible_rows.push_back(original.visible_rows[static_cast<std::size_t>(source_row)]);
            if (original.start.visible && original.start.row == source_row) {
                touch.start = {row, original.start.column, true};
            }
            if (original.end.visible && original.end.row == source_row) {
                touch.end = {row, original.end.column, true};
            }
            for (auto span : original.spans) {
                if (span.row == source_row) {
                    span.row = row;
                    touch.spans.push_back(span);
                }
            }
        }
    }
    displayed->cursor.position.row = content_tail_logical_row_to_display_row(*source,
        render_snapshot_first_visible_logical_row(*source) + source->cursor.position.row);
    displayed->cursor.visible = source->cursor.visible && displayed->cursor.position.row >= 0 &&
        displayed->cursor.position.row < source->grid_size.rows;
    return displayed;
}

}
