#include "vnm_terminal/internal/terminal_content_tail_projection.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

namespace {

bool presentation_is_identity(const Terminal_render_snapshot& source)
{
    const int first_native_row = render_snapshot_first_visible_logical_row(source);
    for (int row = 0; row < source.grid_size.rows; ++row) {
        if (presentation_requested_logical_row(source.viewport, row) != first_native_row + row) {
            return false;
        }
    }
    return true;
}

// Resolves every requested position against the frame's coverage: its own
// rows from logical S - o on, less any uncovered trailing rows, and the
// leading rows directly above them. A frozen frame's missing rows above that
// coverage become unused slots; any other missing row leaves the frame
// unpresentable.
std::optional<std::vector<int>> resolved_logical_rows(const Terminal_render_snapshot& source)
{
    const int rows             = source.grid_size.rows;
    const int first_native_row = render_snapshot_first_visible_logical_row(source);
    const int leading_rows     = source.leading_rows_snapshot ? source.leading_row_count : 0;
    if (!presentation_window_resolvable(
            source.viewport,
            first_native_row - leading_rows,
            leading_rows + rows - source.uncovered_trailing_rows,
            source.frozen_basis))
    {
        return std::nullopt;
    }

    std::vector<int> logical_rows;
    logical_rows.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const int requested = presentation_requested_logical_row(source.viewport, row);
        logical_rows.push_back(requested < first_native_row - leading_rows ? -1 : requested);
    }
    return logical_rows;
}

}

bool render_snapshot_presentable(const Terminal_render_snapshot& source)
{
    return source.display_source || resolved_logical_rows(source).has_value();
}

std::shared_ptr<const Terminal_render_snapshot> make_presented_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> source)
{
    if (!source || source->display_source) {
        return source;
    }

    // Availability first: a mapping that shows the native rows as they are
    // still needs every one of them.
    const std::optional<std::vector<int>> logical_rows = resolved_logical_rows(*source);
    if (!logical_rows.has_value()) {
        return {};
    }
    if (presentation_is_identity(*source)) {
        return source;
    }

    const int rows = source->grid_size.rows;
    const Terminal_render_snapshot* leading = source->leading_rows_snapshot.get();
    // Frozen geometry-adapted frames render known cells without retained
    // identities; the presented frame then carries none either.
    const bool provenance_available =
        render_snapshot_visible_line_provenance_is_valid(*source) &&
        (!leading || render_snapshot_visible_line_provenance_is_valid(*leading));

    auto displayed = std::make_shared<Terminal_render_snapshot>(*source);
    displayed->display_source = source;
    displayed->display_row_logical_rows = *logical_rows;
    // The leading rows are composed into the presented rows; leading_row_count
    // keeps describing the coverage, which is part of the frame's identity.
    displayed->leading_rows_snapshot.reset();
    displayed->cells.clear();
    displayed->visible_line_provenance.clear();
    displayed->visible_row_images.clear();
    displayed->selection_spans.clear();
    displayed->search_match_spans.clear();
    displayed->dirty_row_ranges = {{0, rows}};
    if (displayed->touch_selection) {
        displayed->touch_selection->visible_rows.clear();
        displayed->touch_selection->spans.clear();
        displayed->touch_selection->start.visible = false;
        displayed->touch_selection->end.visible = false;
    }

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

    const int first_native_row = render_snapshot_first_visible_logical_row(*source);
    for (int row = 0; row < rows; ++row) {
        const int logical_row = (*logical_rows)[static_cast<std::size_t>(row)];
        if (logical_row < 0) {
            if (provenance_available) {
                Terminal_render_line_provenance slot;
                slot.logical_row = -1;
                displayed->visible_line_provenance.push_back(slot);
            }
            if (displayed->touch_selection) {
                displayed->touch_selection->visible_rows.push_back({});
            }
            continue;
        }

        const bool from_leading = logical_row < first_native_row;
        const auto& row_source  = from_leading ? *leading : *source;
        const int source_row    = logical_row - render_snapshot_first_visible_logical_row(row_source);
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
        if (provenance_available) {
            displayed->visible_line_provenance.push_back(
                row_source.visible_line_provenance[static_cast<std::size_t>(source_row)]);
        }
        if (!row_source.visible_row_images.empty()) {
            set_render_snapshot_row_image(
                *displayed,
                row,
                row_source.visible_row_images[static_cast<std::size_t>(source_row)]);
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

    const std::optional<int> cursor_row = presentation_display_row_for_logical_row(
        source->viewport, first_native_row + source->cursor.position.row);
    displayed->cursor.position.row = cursor_row.value_or(-1);
    displayed->cursor.visible = source->cursor.visible && cursor_row.has_value() &&
        *cursor_row >= 0 && *cursor_row < rows;
    return displayed;
}

}
