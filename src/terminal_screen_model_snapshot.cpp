#include "vnm_terminal/internal/terminal_screen_model.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/render_snapshot.h"

#include <QByteArray>
#include <QChar>
#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

namespace {

bool is_high_surrogate(QChar character)
{
    const ushort code_unit = character.unicode();
    return code_unit >= 0xd800U && code_unit <= 0xdbffU;
}

bool is_low_surrogate(QChar character)
{
    const ushort code_unit = character.unicode();
    return code_unit >= 0xdc00U && code_unit <= 0xdfffU;
}

bool is_single_unicode_scalar(QStringView text)
{
    if (text.size() == 1) {
        return !is_high_surrogate(text[0]) && !is_low_surrogate(text[0]);
    }

    return text.size() == 2 &&
        is_high_surrogate(text[0]) &&
        is_low_surrogate(text[1]);
}

QByteArray uri_from_hyperlink_identity_key(const QByteArray& identity_key)
{
    constexpr qsizetype uri_prefix_size = 4;
    constexpr qsizetype id_prefix_size  = 3;

    if (identity_key.startsWith(QByteArrayLiteral("uri:"))) {
        return identity_key.sliced(uri_prefix_size);
    }

    if (identity_key.startsWith(QByteArrayLiteral("id:"))) {
        const qsizetype separator = identity_key.indexOf('\x1f', id_prefix_size);
        if (separator >= 0 && separator + 1 < identity_key.size()) {
            return identity_key.sliced(separator + 1);
        }
    }

    return {};
}

}

Terminal_render_snapshot Terminal_screen_model::render_snapshot(std::uint64_t sequence) const
{
    Terminal_viewport_state viewport;
    viewport.active_buffer   = m_active_buffer_id;
    viewport.visible_rows    = m_config.grid_size.rows;
    viewport.scrollback_rows = m_active_buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_backing.retained_history_size()
        : 0;

    Terminal_render_snapshot_request request;
    request.sequence                     = sequence;
    request.viewport                     = viewport;
    request.dirty_rows                   = dirty_rows();
    request.viewport_changed             = m_viewport_changed;
    request.mouse_reporting_mode_changed = m_mouse_reporting_mode_changed;
    return render_snapshot(request);
}

Terminal_render_snapshot Terminal_screen_model::render_snapshot(
    const Terminal_render_snapshot_request& request) const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::render_snapshot");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_requests;
        m_profile_stats.render_snapshot_dirty_rows_requested +=
            static_cast<std::uint64_t>(request.dirty_rows.size());
        if (request.viewport_changed) {
            ++m_profile_stats.render_snapshot_full_repaint_fallbacks;
            ++m_profile_stats.render_snapshot_viewport_fallbacks;
        }
    }
#endif

    Terminal_viewport_state viewport = request.viewport;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::viewport");

        viewport.active_buffer = m_active_buffer_id;
        viewport.visible_rows  = m_config.grid_size.rows;
        if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
            viewport.scrollback_rows  = 0;
            viewport.offset_from_tail = 0;
            viewport.follow_tail      = true;
        }
        else {
            viewport.scrollback_rows = scrollback_size();
            viewport.offset_from_tail =
                std::clamp(viewport.offset_from_tail, 0, viewport.scrollback_rows);
            viewport.follow_tail = viewport.offset_from_tail == 0;
        }
    }

    Terminal_render_snapshot snapshot =
        make_empty_render_snapshot(
            m_config.grid_size,
            viewport,
            request.sequence,
            request.publication_generation);
    snapshot.basis                     = request.basis;
    snapshot.purpose                   = request.purpose;
    snapshot.public_scroll_diagnostics = request.public_scroll_diagnostics;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::metadata");

        snapshot.cells.reserve(
            static_cast<std::size_t>(m_config.grid_size.rows) *
            static_cast<std::size_t>(m_config.grid_size.columns));
        snapshot.color_state                       = m_color_state;
        snapshot.styles                            = m_styles;
        snapshot.cursor.position                   = m_cursor;
        snapshot.cursor.shape                      = request.cursor_shape;
        snapshot.cursor.visible                    = m_modes.cursor_visible;
        snapshot.cursor.blink_enabled              = request.cursor_blink_enabled;
        snapshot.ime_preedit                       = request.ime_preedit;
        snapshot.metadata.processed_backend_callback_epoch =
            request.processed_backend_callback_epoch;
        snapshot.metadata.satisfied_input_freshness_token =
            request.satisfied_input_freshness_token;
        snapshot.metadata.backend_geometry_in_sync = request.backend_geometry_in_sync;
        snapshot.metadata.row_origin_generation    = request.row_origin_generation;
        snapshot.metadata.visual_bell_active       = request.visual_bell_active;
        snapshot.metadata.mouse_reporting_mode_changed =
            request.mouse_reporting_mode_changed;
        snapshot.modes = m_modes;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::dirty_rows");

        snapshot.dirty_row_ranges = compact_dirty_row_ranges(
            viewport_dirty_rows(viewport, request.dirty_rows),
            m_config.grid_size.rows,
            request.viewport_changed);
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            std::uint64_t visible_dirty_rows = 0U;
            for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
                visible_dirty_rows += static_cast<std::uint64_t>(range.row_count);
            }
            m_profile_stats.render_snapshot_dirty_rows_visible += visible_dirty_rows;
            if (visible_dirty_rows == 0U) {
                ++m_profile_stats.render_snapshot_zero_dirty_publications;
            }
        }
#endif
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::append_rows");

#if VNM_TERMINAL_PROFILING_ENABLED
        const std::size_t snapshot_cells_before_append = snapshot.cells.size();
        std::uint64_t rows_visited                     = 0U;
        std::uint64_t rows_with_cells                  = 0U;
        std::uint64_t rows_borrowed                    = 0U;
        std::uint64_t rows_owned                       = 0U;
#endif
        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::render_snapshot::append_rows::reserve");

            snapshot.visible_line_provenance.reserve(
                static_cast<std::size_t>(m_config.grid_size.rows));
        }
        std::vector<std::uint64_t> row_referenced_hyperlink_ids;
        for (int row = 0; row < m_config.grid_size.rows; ++row) {
#if VNM_TERMINAL_PROFILING_ENABLED
            ++rows_visited;
#endif
            row_referenced_hyperlink_ids.clear();
            const viewport_row_t viewport_row{row};
            const snapshot_row_t snapshot_row = [&]() {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::snapshot_row");

                return snapshot_row_from_viewport(viewport_row);
            }();

            std::optional<Viewport_row_cells> row_cells;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::viewport_row_cells");

                row_cells = viewport_row_cells(viewport, row);
            }
            if (row_cells.has_value()) {
#if VNM_TERMINAL_PROFILING_ENABLED
                ++rows_with_cells;
                if (row_cells->materialized()) {
                    ++rows_owned;
                }
                else {
                    ++rows_borrowed;
                }
#endif
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::append_cells");

                append_snapshot_cells_from_row(
                    snapshot,
                    row_cells->cells(),
                    snapshot_row.value,
                    row_referenced_hyperlink_ids);
            }

            if (row_cells.has_value() && !row_referenced_hyperlink_ids.empty()) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::hyperlink_metadata::append");

                append_hyperlink_metadata_for_ids(
                    snapshot.hyperlinks,
                    row_referenced_hyperlink_ids,
                    row_cells->hyperlink_identity_keys());
            }

            std::optional<Terminal_retained_line_provenance> provenance;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::provenance::lookup");

                provenance = viewport_row_provenance(viewport, row);
            }
            if (provenance.has_value()) {
                std::optional<primary_backing_row_t> backing_row;
                {
                    VNM_TERMINAL_PROFILE_SCOPE(
                        "Terminal_screen_model::render_snapshot::append_rows::provenance::backing_row");

                    backing_row = primary_backing_row_from_viewport(viewport, viewport_row);
                }
                {
                    VNM_TERMINAL_PROFILE_SCOPE(
                        "Terminal_screen_model::render_snapshot::append_rows::provenance::append");

                    const int logical_row =
                        backing_row.has_value() ? backing_row->value : row;
                    snapshot.visible_line_provenance.push_back({
                        static_cast<std::int64_t>(logical_row),
                        provenance->retained_line_id,
                        provenance->content_generation,
                        provenance->content_stamp_ms,
                    });
                }
            }
        }
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            const std::uint64_t cells_emitted = static_cast<std::uint64_t>(
                snapshot.cells.size() - snapshot_cells_before_append);
            ++m_profile_stats.render_snapshots_constructed;
            m_profile_stats.render_snapshot_rows_visited      += rows_visited;
            m_profile_stats.render_snapshot_rows_materialized += rows_with_cells;
            m_profile_stats.render_snapshot_rows_borrowed     += rows_borrowed;
            m_profile_stats.render_snapshot_rows_owned        += rows_owned;
            m_profile_stats.render_snapshot_rows_built_from_model_storage +=
                rows_with_cells;
            m_profile_stats.render_snapshot_model_row_accessor_borrows +=
                rows_borrowed;
            m_profile_stats.render_snapshot_cells_scanned     +=
                rows_with_cells *
                static_cast<std::uint64_t>(m_config.grid_size.columns);
            m_profile_stats.render_snapshot_cells_emitted     += cells_emitted;
            m_profile_stats.max_render_snapshot_rows_visited =
                std::max(
                    m_profile_stats.max_render_snapshot_rows_visited,
                    rows_visited);
            m_profile_stats.max_render_snapshot_cells_emitted =
                std::max(
                    m_profile_stats.max_render_snapshot_cells_emitted,
                    cells_emitted);
        }
#endif
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::primary_cursor");

        if (viewport.active_buffer == Terminal_buffer_id::PRIMARY) {
            const primary_backing_row_t cursor_backing_row =
                primary_backing_row_from_active(active_grid_row_t{m_cursor.row});
            const viewport_row_t cursor_viewport_row =
                viewport_row_from_primary_backing_unbounded(viewport, cursor_backing_row);
            snapshot.cursor.position.row = cursor_viewport_row.value;
            snapshot.cursor.visible =
                snapshot.cursor.visible                                &&
                snapshot.cursor.position.row >= 0                      &&
                snapshot.cursor.position.row < m_config.grid_size.rows;
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::selections");

        if (render_snapshot_visible_line_provenance_is_valid(snapshot)) {
            for (const Terminal_render_selection_request& selection_request : request.selections) {
                std::vector<int> selected_logical_rows;
                if (!render_selection_request_logical_rows(
                        selection_request,
                        viewport.active_buffer,
                        selected_logical_rows))
                {
                    continue;
                }

                const Terminal_selection_range& selection = selection_request.range;
                const terminal_grid_position_t start = normalized_selection_start(selection);
                const terminal_grid_position_t end   = normalized_selection_end(selection);

                for (std::size_t index = 0U; index < selected_logical_rows.size(); ++index) {
                    const int logical_row = selected_logical_rows[index];
                    int row = logical_row;
                    if (viewport.active_buffer == Terminal_buffer_id::PRIMARY) {
                        const std::optional<viewport_row_t> converted_row =
                            viewport_row_from_primary_backing(
                                viewport,
                                primary_backing_row_t{logical_row});
                        if (!converted_row.has_value()) {
                            continue;
                        }

                        row = converted_row->value;
                    }
                    if (row < 0 || row >= m_config.grid_size.rows) {
                        continue;
                    }

                    const int first_column = index == 0U ? start.column : 0;
                    const int end_column   =
                        index + 1U == selected_logical_rows.size()
                            ? end.column
                            : m_config.grid_size.columns;
                    if (first_column >= 0                          &&
                        first_column <= m_config.grid_size.columns &&
                        end_column   >= 0                          &&
                        end_column   <= m_config.grid_size.columns &&
                        end_column   >  first_column)
                    {
                        snapshot.selection_spans.push_back({
                            selection,
                            row,
                            first_column,
                            end_column - first_column,
                        });
                    }
                }
            }
        }
    }

    return snapshot;
}

std::optional<Terminal_retained_line_provenance>
Terminal_screen_model::viewport_row_provenance(
    const Terminal_viewport_state& viewport,
    int                            viewport_row) const
{
    const viewport_row_t row{viewport_row};
    if (!viewport_row_is_valid(row)) {
        return std::nullopt;
    }

    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* active_row =
            alternate_active_row(active_grid_row_t{row.value});
        return active_row != nullptr
            ? std::optional<Terminal_retained_line_provenance>(
                active_row->retained_line_provenance)
            : std::nullopt;
    }

    const std::optional<primary_backing_row_t> backing_row =
        primary_backing_row_from_viewport(viewport, row);
    if (!backing_row.has_value()) {
        return std::nullopt;
    }

    if (backing_row->value < scrollback_size()) {
        const std::optional<retained_row_record_t> retained_record =
            m_primary_backing.materialize_retained_history_record(
                static_cast<std::size_t>(backing_row->value));
        return retained_record.has_value()
            ? std::optional<Terminal_retained_line_provenance>(
                retained_record->row.retained_line_provenance)
            : std::nullopt;
    }

    const active_grid_row_t active_row{
        backing_row->value - primary_backing_active_grid_first_row(),
    };
    const Terminal_screen_row& active =
        primary_active_grid_rows()[static_cast<std::size_t>(active_row.value)];
    return active.retained_line_provenance;
}

Terminal_screen_model::snapshot_row_t
Terminal_screen_model::snapshot_row_from_viewport(viewport_row_t row) const
{
    return {row.value};
}

std::vector<int> Terminal_screen_model::viewport_dirty_rows(
    const Terminal_viewport_state& viewport,
    const std::vector<int>&        dirty_rows) const
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return dirty_rows;
    }

    std::vector<int> viewport_rows;
    for (int dirty_row : dirty_rows) {
        const primary_backing_row_t backing_row =
            primary_backing_row_from_active(active_grid_row_t{dirty_row});
        const std::optional<viewport_row_t> viewport_row =
            viewport_row_from_primary_backing(viewport, backing_row);
        if (viewport_row.has_value()) {
            viewport_rows.push_back(viewport_row->value);
        }
    }

    return viewport_rows;
}

void Terminal_screen_model::append_snapshot_cells_from_row(
    Terminal_render_snapshot&  snapshot,
    const std::vector<Cell>&   row,
    int                        snapshot_row,
    std::vector<std::uint64_t>& row_referenced_hyperlink_ids) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::append_snapshot_cells_from_row");

    std::vector<Cell> visual_projection;
    const std::vector<Cell>& visual_row = [&]() -> const std::vector<Cell>& {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::append_snapshot_cells_from_row::geometry_projection");

        return row_cells_for_current_geometry(row, visual_projection);
    }();

    const int column_count = m_config.grid_size.columns;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::append_snapshot_cells_from_row::scan_cells");

        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::append_snapshot_cells_from_row::scan_cells::loop");

#if VNM_TERMINAL_PROFILING_ENABLED
            const bool profile_enabled = m_profile_stats.enabled;
#endif
            for (int column = 0; column < column_count; ++column) {
                const Cell& cell = visual_row[static_cast<std::size_t>(column)];
                if (!cell.occupied) {
#if VNM_TERMINAL_PROFILING_ENABLED
                    if (profile_enabled) {
                        ++m_profile_stats.render_snapshot_unoccupied_cells_skipped;
                    }
#endif
                    continue;
                }

                Terminal_render_cell_text snapshot_text =
                    Terminal_render_cell_text::from_source_cell(
                        cell.text,
                        cell.display_width,
                        cell.wide_continuation);
                const Terminal_render_cell_text_category snapshot_text_category =
                    snapshot_text.category();

#if VNM_TERMINAL_PROFILING_ENABLED
                if (profile_enabled) {
                    switch (snapshot_text.storage()) {
                        case Terminal_render_cell_text_storage::EMPTY:
                            ++m_profile_stats.render_snapshot_compact_empty_text_cells;
                            break;
                        case Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII:
                            ++m_profile_stats.render_snapshot_compact_ascii_text_cells;
                            break;
                        case Terminal_render_cell_text_storage::INLINE_SINGLE_BMP:
                            ++m_profile_stats.render_snapshot_inline_single_bmp_text_cells;
                            break;
                        case Terminal_render_cell_text_storage::FALLBACK_QSTRING: {
                            const QString* fallback_text =
                                snapshot_text.fallback_qstring_or_null();
                            const QStringView text(*fallback_text);
                            const std::uint64_t code_units =
                                static_cast<std::uint64_t>(text.size());
                            ++m_profile_stats.render_snapshot_fallback_qstring_copies;
                            m_profile_stats.
                                render_snapshot_fallback_text_code_units_copied +=
                                    code_units;
                            m_profile_stats.
                                max_render_snapshot_fallback_text_units_per_cell =
                                    std::max(
                                        m_profile_stats.
                                            max_render_snapshot_fallback_text_units_per_cell,
                                        code_units);

                            if (snapshot_text_category ==
                                Terminal_render_cell_text_category::PRINTABLE_ASCII)
                            {
                                ++m_profile_stats.
                                    render_snapshot_fallback_printable_ascii_copies;
                            }
                            else
                            if (snapshot_text_category ==
                                Terminal_render_cell_text_category::OTHER_ASCII)
                            {
                                ++m_profile_stats.
                                    render_snapshot_fallback_other_ascii_copies;
                            }
                            else
                            if (is_single_unicode_scalar(text)) {
                                ++m_profile_stats.
                                    render_snapshot_fallback_single_non_ascii_copies;
                            }
                            else {
                                ++m_profile_stats.render_snapshot_fallback_multi_text_copies;
                            }
                            break;
                        }
                    }
                }
#endif

                if (cell.hyperlink_id != 0U) {
                    const auto insert_position = std::lower_bound(
                        row_referenced_hyperlink_ids.begin(),
                        row_referenced_hyperlink_ids.end(),
                        cell.hyperlink_id);
                    if (insert_position == row_referenced_hyperlink_ids.end() ||
                        *insert_position != cell.hyperlink_id)
                    {
                        row_referenced_hyperlink_ids.insert(insert_position, cell.hyperlink_id);
                    }
                }

                snapshot.cells.push_back({
                    { snapshot_row, column },
                    std::move(snapshot_text),
                    cell.hyperlink_id,
                    cell.display_width,
                    cell.wide_continuation,
                    cell.style_id,
                    snapshot_text_category,
                });
            }
        }
    }
}

std::optional<Terminal_screen_model::Viewport_row_cells>
Terminal_screen_model::viewport_row_cells(
    const Terminal_viewport_state& viewport,
    int                            viewport_row) const
{
    const viewport_row_t row{viewport_row};
    if (!viewport_row_is_valid(row)) {
        return std::nullopt;
    }

    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* active_row =
            alternate_active_row(active_grid_row_t{row.value});
        return active_row != nullptr
            ? std::optional<Viewport_row_cells>(Viewport_row_cells{&active_row->cells})
            : std::nullopt;
    }

    const std::optional<primary_backing_row_t> backing_row =
        primary_backing_row_from_viewport(viewport, row);
    if (!backing_row.has_value()) {
        return std::nullopt;
    }

    if (backing_row->value < scrollback_size()) {
        std::optional<retained_row_record_t> retained_record;
        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::viewport_row_cells::retained_history_materialize");
            retained_record = m_primary_backing.materialize_retained_history_record(
                static_cast<std::size_t>(backing_row->value));
        }
        if (!retained_record.has_value()) {
            return std::nullopt;
        }

        Viewport_row_cells result;
        result.owned_cells = std::move(retained_record->row.cells);
        result.owned_hyperlink_identity_keys =
            std::move(retained_record->hyperlink_identity_keys);
        return result;
    }

    const active_grid_row_t active_row{
        backing_row->value - primary_backing_active_grid_first_row(),
    };
    const Terminal_screen_row& active =
        primary_active_grid_rows()[static_cast<std::size_t>(active_row.value)];
    return Viewport_row_cells{&active.cells};
}

void Terminal_screen_model::append_hyperlink_metadata_for_ids(
    std::vector<Terminal_render_hyperlink_metadata>& metadata,
    std::span<const std::uint64_t>                   hyperlink_ids,
    const std::map<std::uint64_t, QByteArray>*       row_local_identity_keys) const
{
    for (std::uint64_t hyperlink_id : hyperlink_ids) {
        const bool already_materialized = std::any_of(
            metadata.begin(),
            metadata.end(),
            [hyperlink_id](const Terminal_render_hyperlink_metadata& candidate) {
                return candidate.hyperlink_id == hyperlink_id;
            });
        if (already_materialized) {
            continue;
        }

        const QByteArray* identity_key = nullptr;
        if (row_local_identity_keys != nullptr) {
            const auto row_local_found = row_local_identity_keys->find(hyperlink_id);
            if (row_local_found != row_local_identity_keys->end()) {
                identity_key = &row_local_found->second;
            }
        }
        else {
            identity_key = active_hyperlink_identity_key(hyperlink_id);
        }

        if (identity_key != nullptr) {
            metadata.push_back({
                hyperlink_id,
                *identity_key,
                uri_from_hyperlink_identity_key(*identity_key),
            });
        }
    }
}

}
