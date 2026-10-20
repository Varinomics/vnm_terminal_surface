#include "vnm_terminal/internal/terminal_content_tail_projection.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

namespace {

bool presentation_is_identity(
    const Terminal_render_snapshot& source,
    const Terminal_viewport_state& viewport)
{
    if (viewport.visible_rows != source.grid_size.rows ||
        viewport.offset_from_tail != source.viewport.offset_from_tail)
    {
        return false;
    }
    const int first_native_row = render_snapshot_first_visible_logical_row(source);
    for (int row = 0; row < source.grid_size.rows; ++row) {
        if (presentation_requested_logical_row(viewport, row) != first_native_row + row) {
            return false;
        }
    }
    return true;
}

// Native source and consumer windows have independent heights and offsets.
// Resolve rows against actual immutable coverage, including rows after the
// native window when a larger consumer crosses clear-page padding.
std::optional<std::vector<int>> resolved_logical_rows(
    const Terminal_render_snapshot& source,
    const Terminal_viewport_state& viewport)
{
    const int rows             = viewport.visible_rows;
    const int first_native_row = render_snapshot_first_visible_logical_row(source);
    const int native_end_row = first_native_row + source.grid_size.rows - source.uncovered_trailing_rows;
    const auto* coverage = render_snapshot_row_coverage_is_coherent(source)
        ? source.row_coverage_snapshot.get() : nullptr;
    const int coverage_first = coverage ? render_snapshot_first_visible_logical_row(*coverage) : first_native_row;
    const int coverage_end = coverage ? coverage_first + coverage->grid_size.rows : coverage_first;

    std::vector<int> logical_rows;
    logical_rows.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const int requested = presentation_requested_logical_row(viewport, row);
        if (requested < 0 ||
            (source.viewport.active_buffer == Terminal_buffer_id::ALTERNATE &&
             requested >= source.grid_size.rows))
        {
            logical_rows.push_back(-1);
            continue;
        }
        if ((requested >= first_native_row && requested < native_end_row) ||
            (coverage && requested >= coverage_first && requested < coverage_end))
        {
            logical_rows.push_back(requested);
            continue;
        }
        return std::nullopt;
    }
    return logical_rows;
}

}

bool render_snapshot_presentable(
    const Terminal_render_snapshot& source, int display_rows, int display_offset_from_tail)
{
    if (display_rows < 0 || display_offset_from_tail < -1) {
        return false;
    }
    if (source.display_source) {
        return ((display_rows == 0 || display_rows == source.grid_size.rows) &&
                (display_offset_from_tail < 0 || display_offset_from_tail == source.viewport.offset_from_tail)) ||
            render_snapshot_presentable(*source.display_source, display_rows, display_offset_from_tail);
    }
    Terminal_viewport_state viewport = source.viewport;
    viewport.visible_rows = display_rows > 0 ? display_rows : source.grid_size.rows;
    if (display_offset_from_tail >= 0) {
        viewport.offset_from_tail = display_offset_from_tail;
    }
    viewport.offset_from_tail = presentation_clamped_offset_from_tail(viewport);
    return resolved_logical_rows(source, viewport).has_value();
}

std::shared_ptr<const Terminal_render_snapshot> make_presented_snapshot(
    std::shared_ptr<const Terminal_render_snapshot> source,
    int display_rows,
    int display_offset_from_tail)
{
    if (!source || display_rows < 0 || display_offset_from_tail < -1) {
        return {};
    }
    if (source->display_source) {
        if ((display_rows > 0 && display_rows != source->grid_size.rows) ||
            (display_offset_from_tail >= 0 && display_offset_from_tail != source->viewport.offset_from_tail))
        {
            return make_presented_snapshot(source->display_source, display_rows, display_offset_from_tail);
        }
        return source;
    }

    Terminal_viewport_state viewport = source->viewport;
    viewport.visible_rows = display_rows > 0 ? display_rows : source->grid_size.rows;
    if (display_offset_from_tail >= 0) {
        viewport.offset_from_tail = display_offset_from_tail;
    }
    viewport.offset_from_tail = presentation_clamped_offset_from_tail(viewport);
    viewport.follow_tail = viewport.offset_from_tail == 0;

    // Availability first: a mapping that shows the native rows as they are
    // still needs every one of them.
    const std::optional<std::vector<int>> logical_rows = resolved_logical_rows(*source, viewport);
    if (!logical_rows.has_value()) {
        return {};
    }
    const bool touch_presentation_matches = !source->touch_selection ||
        (source->touch_selection->presentation &&
            source->touch_selection->presentation->visible_rows == viewport.visible_rows &&
            source->touch_selection->presentation->offset_from_tail == viewport.offset_from_tail);
    if (presentation_is_identity(*source, viewport) && touch_presentation_matches) {
        return source;
    }

    const int rows = viewport.visible_rows;
    const Terminal_render_snapshot* coverage = render_snapshot_row_coverage_is_coherent(*source)
        ? source->row_coverage_snapshot.get()
        : nullptr;
    // Frozen geometry-adapted frames render known cells without retained
    // identities; the presented frame then carries none either.
    const bool provenance_available =
        render_snapshot_visible_line_provenance_is_valid(*source) &&
        (!coverage || render_snapshot_visible_line_provenance_is_valid(*coverage));

    auto displayed = std::make_shared<Terminal_render_snapshot>(*source);
    displayed->grid_size.rows = rows;
    displayed->viewport = viewport;
    displayed->display_source = source;
    displayed->display_row_logical_rows = *logical_rows;
    displayed->row_coverage_snapshot.reset();
    displayed->cells.clear();
    displayed->visible_line_provenance.clear();
    displayed->visible_row_images.clear();
    displayed->selection_spans.clear();
    displayed->search_match_spans.clear();
    displayed->dirty_row_ranges = {{0, rows}};
    if (displayed->touch_selection) {
        displayed->touch_selection->presentation = terminal_touch_selection_presentation_t{
            .visible_rows = rows,
            .offset_from_tail = viewport.offset_from_tail,
        };
        displayed->touch_selection->visible_rows.clear();
        displayed->touch_selection->spans.clear();
        displayed->touch_selection->start.visible = false;
        displayed->touch_selection->end.visible = false;
    }

    const Terminal_render_snapshot_row_content_view native_rows(*source);
    std::optional<Terminal_render_snapshot_row_content_view> coverage_rows;
    const auto coverage_style_offset = static_cast<Terminal_style_id>(displayed->styles.size());
    std::map<Terminal_hyperlink_id, Terminal_hyperlink_id> coverage_hyperlinks;
    if (coverage) {
        coverage_rows.emplace(*coverage);
        displayed->styles.insert(displayed->styles.end(), coverage->styles.begin(), coverage->styles.end());
        Terminal_hyperlink_id next_id = 1;
        for (const auto& link : displayed->hyperlinks) {
            next_id = std::max(next_id, link.hyperlink_id + 1);
        }
        for (auto link : coverage->hyperlinks) {
            coverage_hyperlinks.emplace(link.hyperlink_id, next_id);
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

        const bool from_coverage = logical_row < first_native_row ||
            logical_row >= first_native_row + source->grid_size.rows - source->uncovered_trailing_rows;
        const auto& row_source  = from_coverage ? *coverage : *source;
        const int source_row    = logical_row - render_snapshot_first_visible_logical_row(row_source);
        const auto cells = (from_coverage ? *coverage_rows : native_rows).row_at(source_row);
        for (auto cell : cells) {
            cell.position.row = row;
            if (from_coverage) {
                cell.style_id += coverage_style_offset;
                if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                    cell.hyperlink_id = coverage_hyperlinks.at(cell.hyperlink_id);
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
        viewport, first_native_row + source->cursor.position.row);
    displayed->cursor.position.row = cursor_row.value_or(-1);
    displayed->cursor.visible = source->modes.cursor_visible && cursor_row.has_value() &&
        *cursor_row >= 0 && *cursor_row < rows;
    return displayed;
}

}
