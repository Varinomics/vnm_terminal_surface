#include "vnm_terminal/internal/terminal_public_projection.h"

#include "vnm_terminal/internal/terminal_screen_model.h"
#include <QtGlobal>
#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace vnm_terminal::internal {

namespace {

int projection_safe_basis_scrollback_depth(const Terminal_render_snapshot& safe_basis)
{
    return safe_basis.viewport.active_buffer == Terminal_buffer_id::PRIMARY
        ? safe_basis.viewport.scrollback_rows
        : 0;
}

int first_safe_basis_public_row(const Terminal_render_snapshot& safe_basis)
{
    return safe_basis.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
        ? 0
        : safe_basis.viewport.scrollback_rows - safe_basis.viewport.offset_from_tail;
}

int full_primary_public_row_count(const Terminal_render_snapshot& safe_basis)
{
    if (safe_basis.viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
        return 0;
    }

    return std::max(0, safe_basis.viewport.scrollback_rows) +
        std::max(0, safe_basis.grid_size.rows);
}

int safe_basis_projection_row_count(const Terminal_render_snapshot& safe_basis)
{
    return std::min(
        safe_basis.grid_size.rows,
        static_cast<int>(safe_basis.visible_line_provenance.size()));
}

bool provenance_describes_same_retained_fragment_source(
    const Terminal_render_line_provenance& left,
    const Terminal_render_line_provenance& right)
{
    return
        left.retained_line_id   == right.retained_line_id   &&
        left.content_generation == right.content_generation;
}

terminal_history_handle_t projection_history_handle_from_provenance(
    const Terminal_render_line_provenance& provenance)
{
    return terminal_history_handle_from_retained_identity(
        provenance.retained_line_id,
        provenance.content_generation);
}

bool render_cells_match(
    const Terminal_render_cell& left,
    const Terminal_render_cell& right)
{
    return
        left.position.row      == right.position.row      &&
        left.position.column   == right.position.column   &&
        left.text              == right.text              &&
        left.hyperlink_id      == right.hyperlink_id      &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.style_id          == right.style_id;
}

bool projection_row_cells_match(
    const Terminal_public_projection_row& left,
    const Terminal_public_projection_row& right)
{
    if (left.public_row != right.public_row ||
        !(left.provenance == right.provenance) ||
        left.cells.size() != right.cells.size())
    {
        return false;
    }

    for (std::size_t index = 0U; index < left.cells.size(); ++index) {
        if (!render_cells_match(left.cells[index], right.cells[index])) {
            return false;
        }
    }

    return true;
}

int visual_fragment_index_for_snapshot_row(
    const Terminal_render_snapshot& snapshot,
    int                             snapshot_row)
{
    const Terminal_render_line_provenance& provenance =
        snapshot.visible_line_provenance[static_cast<std::size_t>(snapshot_row)];

    int fragment_index = 0;
    for (int row = 0; row < snapshot_row; ++row) {
        if (provenance_describes_same_retained_fragment_source(
                provenance,
                snapshot.visible_line_provenance[static_cast<std::size_t>(row)]))
        {
            ++fragment_index;
        }
    }
    return fragment_index;
}

bool row_snapshot_matches_safe_capture_basis(
    const Terminal_render_snapshot& row_snapshot,
    const Terminal_render_snapshot& safe_basis)
{
    return
        grid_sizes_match(row_snapshot.grid_size, safe_basis.grid_size) &&
        row_snapshot.viewport.active_buffer == Terminal_buffer_id::PRIMARY &&
        safe_basis.viewport.active_buffer   == Terminal_buffer_id::PRIMARY &&
        row_snapshot.viewport.scrollback_rows ==
            safe_basis.viewport.scrollback_rows &&
        row_snapshot.viewport.visible_rows == safe_basis.viewport.visible_rows;
}

Terminal_public_scroll_diagnostic_reason diagnostic_reason_for_disable_reason(
    Terminal_public_projection_disable_reason reason)
{
    switch (reason) {
        case Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED:
            return
                Terminal_public_scroll_diagnostic_reason::PUBLIC_PROJECTION_GEOMETRY_INVALIDATED;
        case Terminal_public_projection_disable_reason::MEMORY_PRESSURE:
            return Terminal_public_scroll_diagnostic_reason::
                PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED;
        case Terminal_public_projection_disable_reason::NONE:
        case Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED:
        case Terminal_public_projection_disable_reason::UNSUPPORTED_BUFFER:
            return Terminal_public_scroll_diagnostic_reason::NONE;
    }

    return Terminal_public_scroll_diagnostic_reason::NONE;
}

Terminal_public_projection_row copied_projection_row(
    const Terminal_render_snapshot& snapshot,
    int                             snapshot_row,
    int                             public_row,
    int                             visual_fragment_index,
    bool                            visual_fragment_index_is_exact)
{
    Q_ASSERT(snapshot_row >= 0);
    Q_ASSERT(snapshot_row < static_cast<int>(snapshot.visible_line_provenance.size()));

    Terminal_public_projection_row row;
    row.public_row            = public_row;
    row.provenance            =
        snapshot.visible_line_provenance[static_cast<std::size_t>(snapshot_row)];
    row.history_handle        =
        projection_history_handle_from_provenance(row.provenance);
    row.visual_fragment_index = visual_fragment_index;
    row.visual_fragment_index_is_exact = visual_fragment_index_is_exact;

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row != snapshot_row) {
            continue;
        }

        Terminal_render_cell copied_cell = cell;
        copied_cell.position.row         = 0;
        row.cells.push_back(std::move(copied_cell));
    }

    return row;
}

std::vector<Terminal_text_style> compact_styles_for_copied_rows(
    const std::vector<Terminal_text_style>&       source_styles,
    std::vector<Terminal_public_projection_row>&  rows)
{
    std::set<Terminal_style_id> referenced_style_ids;
    for (const Terminal_public_projection_row& row : rows) {
        for (const Terminal_render_cell& cell : row.cells) {
            referenced_style_ids.insert(cell.style_id);
        }
    }

    std::vector<std::optional<Terminal_style_id>> remapped_style_ids(source_styles.size());
    std::vector<Terminal_text_style> compact_styles;
    compact_styles.reserve(referenced_style_ids.size() + 1U);
    compact_styles.push_back(
        source_styles.empty()
            ? Terminal_text_style{}
            : source_styles[static_cast<std::size_t>(k_default_terminal_style_id)]);
    if (!remapped_style_ids.empty()) {
        remapped_style_ids[static_cast<std::size_t>(k_default_terminal_style_id)] =
            k_default_terminal_style_id;
    }

    for (Terminal_style_id source_style_id : referenced_style_ids) {
        if (source_style_id == k_default_terminal_style_id) {
            continue;
        }

        const std::size_t source_style_index =
            static_cast<std::size_t>(source_style_id);
        if (source_style_index >= source_styles.size()) {
            continue;
        }

        remapped_style_ids[source_style_index] =
            static_cast<Terminal_style_id>(compact_styles.size());
        compact_styles.push_back(source_styles[source_style_index]);
    }

    for (Terminal_public_projection_row& row : rows) {
        for (Terminal_render_cell& cell : row.cells) {
            const std::size_t source_style_index =
                static_cast<std::size_t>(cell.style_id);
            if (source_style_index < remapped_style_ids.size() &&
                remapped_style_ids[source_style_index].has_value())
            {
                cell.style_id = *remapped_style_ids[source_style_index];
            }
            else {
                cell.style_id = k_default_terminal_style_id;
            }
        }
    }

    return compact_styles;
}

std::vector<Terminal_render_hyperlink_metadata> compact_hyperlinks_for_copied_rows(
    const std::vector<Terminal_render_hyperlink_metadata>& source_hyperlinks,
    std::vector<Terminal_public_projection_row>&           rows)
{
    std::set<std::uint64_t> referenced_hyperlink_ids;
    for (const Terminal_public_projection_row& row : rows) {
        for (const Terminal_render_cell& cell : row.cells) {
            if (cell.hyperlink_id != 0U) {
                referenced_hyperlink_ids.insert(cell.hyperlink_id);
            }
        }
    }

    std::vector<Terminal_render_hyperlink_metadata> compact_hyperlinks;
    compact_hyperlinks.reserve(referenced_hyperlink_ids.size());
    std::set<std::uint64_t> compact_hyperlink_ids;
    for (const Terminal_render_hyperlink_metadata& hyperlink : source_hyperlinks) {
        if (referenced_hyperlink_ids.contains(hyperlink.hyperlink_id)) {
            compact_hyperlinks.push_back(hyperlink);
            compact_hyperlink_ids.insert(hyperlink.hyperlink_id);
        }
    }

    for (Terminal_public_projection_row& row : rows) {
        for (Terminal_render_cell& cell : row.cells) {
            if (cell.hyperlink_id != 0U &&
                !compact_hyperlink_ids.contains(cell.hyperlink_id))
            {
                cell.hyperlink_id = 0U;
            }
        }
    }

    return compact_hyperlinks;
}

void append_unique_hyperlink_metadata(
    std::vector<Terminal_render_hyperlink_metadata>&       target,
    const std::vector<Terminal_render_hyperlink_metadata>& source)
{
    for (const Terminal_render_hyperlink_metadata& hyperlink : source) {
        const auto duplicate = std::find_if(
            target.begin(),
            target.end(),
            [&hyperlink](const Terminal_render_hyperlink_metadata& candidate) {
                return candidate.hyperlink_id == hyperlink.hyperlink_id;
            });
        if (duplicate == target.end()) {
            target.push_back(hyperlink);
        }
    }
}

Terminal_render_snapshot_request public_projection_row_snapshot_request(
    const Terminal_render_snapshot& safe_basis,
    int                             offset_from_tail)
{
    Terminal_render_snapshot_request request;
    request.sequence                     = safe_basis.metadata.sequence;
    request.row_origin_generation        = safe_basis.metadata.row_origin_generation;
    request.basis                        = safe_basis.basis;
    request.purpose                      = safe_basis.purpose;
    request.viewport                     = safe_basis.viewport;
    request.viewport.offset_from_tail    = offset_from_tail;
    request.viewport_changed             = true;
    request.cursor_shape                 = safe_basis.cursor.shape;
    request.cursor_blink_enabled         = safe_basis.cursor.blink_enabled;
    request.ime_preedit                  = safe_basis.ime_preedit;
    request.backend_geometry_in_sync     = safe_basis.metadata.backend_geometry_in_sync;
    request.visual_bell_active           = safe_basis.metadata.visual_bell_active;
    request.mouse_reporting_mode_changed =
        safe_basis.metadata.mouse_reporting_mode_changed;
    return request;
}

int first_public_row_for_snapshot(const Terminal_render_snapshot& snapshot)
{
    return snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
        ? 0
        : snapshot.viewport.scrollback_rows - snapshot.viewport.offset_from_tail;
}

std::pair<std::uint64_t, std::uint64_t> fragment_count_key(
    const Terminal_render_line_provenance& provenance)
{
    return { provenance.retained_line_id, provenance.content_generation };
}

std::vector<Terminal_public_projection_row> copied_primary_projection_rows(
    const Terminal_screen_model&                         safe_model,
    const Terminal_render_snapshot&                      safe_basis,
    std::vector<Terminal_render_hyperlink_metadata>&     source_hyperlinks,
    std::size_t&                                         row_capture_snapshot_count,
    bool&                                                safe_basis_matches_entry_boundary)
{
    const int safe_basis_scrollback_depth =
        projection_safe_basis_scrollback_depth(safe_basis);
    const int public_row_count = full_primary_public_row_count(safe_basis);

    row_capture_snapshot_count        = 0U;
    safe_basis_matches_entry_boundary = true;

    std::vector<Terminal_public_projection_row> copied_by_public_row(
        static_cast<std::size_t>(public_row_count));
    std::vector<bool> copied_public_rows(static_cast<std::size_t>(public_row_count), false);

    const int snapshot_stride = std::max(1, safe_basis.grid_size.rows);
    for (int offset_from_tail = safe_basis_scrollback_depth;;) {
        const Terminal_render_snapshot row_snapshot = safe_model.render_snapshot(
            public_projection_row_snapshot_request(
                safe_basis,
                offset_from_tail));
        ++row_capture_snapshot_count;

        if (!row_snapshot_matches_safe_capture_basis(row_snapshot, safe_basis)) {
            safe_basis_matches_entry_boundary = false;
            return {};
        }

        append_unique_hyperlink_metadata(source_hyperlinks, row_snapshot.hyperlinks);

        const int first_public_row = first_public_row_for_snapshot(row_snapshot);
        for (int snapshot_row = 0; snapshot_row < row_snapshot.grid_size.rows; ++snapshot_row) {
            const int public_row = first_public_row + snapshot_row;
            if (public_row < 0 ||
                public_row >= public_row_count ||
                copied_public_rows[static_cast<std::size_t>(public_row)] ||
                snapshot_row >= static_cast<int>(row_snapshot.visible_line_provenance.size()))
            {
                continue;
            }

            Terminal_public_projection_row copied_row =
                copied_projection_row(row_snapshot, snapshot_row, public_row, 0, false);
            const std::optional<terminal_history_handle_t> history_handle =
                safe_model.retained_history_handle_at_logical_row(
                    Terminal_buffer_id::PRIMARY,
                    public_row);
            if (!history_handle.has_value()) {
                safe_basis_matches_entry_boundary = false;
                return {};
            }

            copied_row.history_handle = *history_handle;
            copied_by_public_row[static_cast<std::size_t>(public_row)] =
                std::move(copied_row);
            copied_public_rows[static_cast<std::size_t>(public_row)] = true;
        }

        if (offset_from_tail == 0) {
            break;
        }

        offset_from_tail = std::max(0, offset_from_tail - snapshot_stride);
    }

    const int first_safe_basis_row = first_safe_basis_public_row(safe_basis);
    const int safe_basis_row_count = safe_basis_projection_row_count(safe_basis);
    for (int snapshot_row = 0; snapshot_row < safe_basis_row_count; ++snapshot_row) {
        const int public_row = first_safe_basis_row + snapshot_row;
        if (public_row < 0 || public_row >= public_row_count ||
            !copied_public_rows[static_cast<std::size_t>(public_row)])
        {
            safe_basis_matches_entry_boundary = false;
            return {};
        }

        const Terminal_public_projection_row expected =
            copied_projection_row(safe_basis, snapshot_row, public_row, 0, false);
        if (!projection_row_cells_match(
                copied_by_public_row[static_cast<std::size_t>(public_row)],
                expected))
        {
            safe_basis_matches_entry_boundary = false;
            return {};
        }
    }

    std::vector<Terminal_public_projection_row> rows;
    rows.reserve(static_cast<std::size_t>(public_row_count));
    std::map<std::pair<std::uint64_t, std::uint64_t>, int> next_fragment_indices;
    for (int public_row = 0; public_row < public_row_count; ++public_row) {
        if (!copied_public_rows[static_cast<std::size_t>(public_row)]) {
            safe_basis_matches_entry_boundary = false;
            return {};
        }

        Terminal_public_projection_row row =
            std::move(copied_by_public_row[static_cast<std::size_t>(public_row)]);
        const std::pair<std::uint64_t, std::uint64_t> key =
            fragment_count_key(row.provenance);
        row.visual_fragment_index = next_fragment_indices[key];
        row.visual_fragment_index_is_exact = true;
        ++next_fragment_indices[key];
        rows.push_back(
            std::move(row));
    }
    return rows;
}

}

Terminal_public_projection Terminal_public_projection::capture_from_safe_model(
    std::uint64_t                       generation,
    const Terminal_render_snapshot&     safe_basis,
    terminal_selection_content_basis_t  content_basis,
    std::uint64_t                       active_buffer_epoch)
{
    Terminal_public_projection projection;
    projection.m_generation                = generation;
    projection.m_content_basis             = content_basis;
    projection.m_active_buffer_epoch       = active_buffer_epoch;
    projection.m_source_basis              = safe_basis.basis;
    projection.m_source_purpose            = safe_basis.purpose;
    projection.m_basis_validation_status   = validate_render_snapshot(safe_basis).status;
    projection.m_grid_size                 = safe_basis.grid_size;
    projection.m_viewport                  = safe_basis.viewport;
    projection.m_color_state               = safe_basis.color_state;
    projection.m_cursor                    = safe_basis.cursor;
    projection.m_modes                     = safe_basis.modes;
    projection.m_metadata                  = safe_basis.metadata;
    projection.m_public_scroll_diagnostics = safe_basis.public_scroll_diagnostics;
    projection.m_public_scroll_diagnostics.public_projection_generation = generation;
    projection.m_safe_basis_viewport_dirty_row_ranges = safe_basis.dirty_row_ranges;
    projection.m_safe_basis_viewport_selection_spans   = safe_basis.selection_spans;
    projection.m_safe_basis_scrollback_depth = projection_safe_basis_scrollback_depth(safe_basis);
    projection.m_safe_basis_active_grid_rows = safe_basis.grid_size.rows;
    projection.m_first_copied_public_row   = first_safe_basis_public_row(safe_basis);
    projection.m_copied_row_bound          =
        static_cast<std::size_t>(safe_basis_projection_row_count(safe_basis));
    projection.m_row_capture_snapshot_count = 1U;

    projection.m_rows.reserve(projection.m_copied_row_bound);

    // Rows are copied from the safe-basis viewport only. Off-viewport public
    // scrollback requires a safe row store; this projection must not reconstruct
    // those rows from the live model.
    for (int snapshot_row = 0;
         snapshot_row < static_cast<int>(projection.m_copied_row_bound);
         ++snapshot_row)
    {
        const int visual_fragment_index =
            visual_fragment_index_for_snapshot_row(safe_basis, snapshot_row);
        projection.m_rows.push_back(
            copied_projection_row(
                safe_basis,
                snapshot_row,
                projection.m_first_copied_public_row + snapshot_row,
                visual_fragment_index,
                false));
    }

    projection.m_styles =
        compact_styles_for_copied_rows(safe_basis.styles, projection.m_rows);
    projection.m_hyperlinks =
        compact_hyperlinks_for_copied_rows(safe_basis.hyperlinks, projection.m_rows);
    projection.m_ime_preedit = safe_basis.ime_preedit;

    return projection;
}

Terminal_public_projection
Terminal_public_projection::capture_primary_full_rows_from_safe_model(
    std::uint64_t                       generation,
    const Terminal_render_snapshot&     safe_basis,
    terminal_selection_content_basis_t  content_basis,
    std::uint64_t                       active_buffer_epoch,
    const Terminal_screen_model&        safe_model)
{
    Terminal_public_projection projection =
        capture_from_safe_model(
            generation,
            safe_basis,
            content_basis,
            active_buffer_epoch);
    if (safe_basis.viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
        return projection;
    }

    std::vector<Terminal_render_hyperlink_metadata> source_hyperlinks =
        safe_basis.hyperlinks;
    std::size_t row_capture_snapshot_count = 0U;
    bool        safe_basis_matches_entry_boundary = false;
    std::vector<Terminal_public_projection_row> rows =
        copied_primary_projection_rows(
            safe_model,
            safe_basis,
            source_hyperlinks,
            row_capture_snapshot_count,
            safe_basis_matches_entry_boundary);
    if (!safe_basis_matches_entry_boundary) {
        return projection;
    }

    projection.m_rows = std::move(rows);
    projection.m_first_copied_public_row = 0;
    projection.m_copied_row_bound        = projection.m_rows.size();
    projection.m_row_capture_snapshot_count = row_capture_snapshot_count;
    projection.m_styles =
        compact_styles_for_copied_rows(safe_basis.styles, projection.m_rows);
    projection.m_hyperlinks =
        compact_hyperlinks_for_copied_rows(source_hyperlinks, projection.m_rows);
    projection.m_rows_are_safe_basis_viewport_only = false;
    return projection;
}

Terminal_public_projection Terminal_public_projection::with_copied_rows_for_testing(
    const Terminal_public_projection&       source,
    int                                     first_copied_public_row,
    std::vector<Terminal_public_projection_row>
                                            rows)
{
    Terminal_public_projection projection = source;
    projection.m_first_copied_public_row = first_copied_public_row;
    projection.m_copied_row_bound        = rows.size();
    projection.m_rows                    = std::move(rows);
    projection.m_row_capture_snapshot_count = 0U;
    projection.m_rows_are_safe_basis_viewport_only = true;
    return projection;
}

void Terminal_public_viewport_controller::reset()
{
    m_content_basis = {};
    m_active_buffer_epoch = 0U;
    m_first_copied_public_row = 0;
    m_copied_row_metadata.clear();
    m_release_intent = {};
}

void Terminal_public_viewport_controller::initialize_from_projection(
    const Terminal_public_projection& projection)
{
    std::vector<Terminal_public_projection_row_metadata> copied_row_metadata;
    copied_row_metadata.reserve(projection.rows().size());
    for (const Terminal_public_projection_row& row : projection.rows()) {
        copied_row_metadata.push_back({
            row.provenance,
            row.history_handle,
            row.visual_fragment_index,
            row.visual_fragment_index_is_exact,
        });
    }

    initialize_from_copied_rows(
        projection.viewport(),
        projection.generation(),
        projection.content_basis(),
        projection.active_buffer_epoch(),
        projection.first_copied_public_row(),
        std::move(copied_row_metadata));
}

void Terminal_public_viewport_controller::initialize_from_copied_rows(
    Terminal_viewport_state                             viewport,
    std::uint64_t                                       public_projection_generation,
    terminal_selection_content_basis_t                  content_basis,
    std::uint64_t                                       active_buffer_epoch,
    int                                                 first_copied_public_row,
    std::vector<Terminal_public_projection_row_metadata> copied_row_metadata)
{
    m_content_basis            = content_basis;
    m_active_buffer_epoch      = active_buffer_epoch;
    m_first_copied_public_row  = first_copied_public_row;
    m_copied_row_metadata      = std::move(copied_row_metadata);
    m_release_intent           = {};

    viewport.scrollback_rows =
        std::max(0, viewport.scrollback_rows);
    viewport.offset_from_tail =
        std::clamp(viewport.offset_from_tail, 0, viewport.scrollback_rows);
    viewport.follow_tail = viewport.offset_from_tail == 0;

    m_release_intent.has_public_viewport            = true;
    m_release_intent.public_projection_valid        = true;
    m_release_intent.public_projection_generation   = public_projection_generation;
    m_release_intent.public_viewport                = viewport;
    m_release_intent.active_buffer_epoch            = active_buffer_epoch;
    m_release_intent.sticky_tail                    = viewport.follow_tail;
    refresh_detached_anchor();
}

int Terminal_public_viewport_controller::deferred_scroll_base_offset_from_tail() const
{
    return !m_release_intent.public_projection_valid &&
            m_release_intent.deferred_offset_from_tail.has_value()
        ? *m_release_intent.deferred_offset_from_tail
        : m_release_intent.public_viewport.offset_from_tail;
}

int Terminal_public_viewport_controller::deferred_line_delta_from_public_viewport(
    int offset_from_tail) const
{
    return offset_from_tail - m_release_intent.public_viewport.offset_from_tail;
}

Terminal_public_viewport_scroll_result Terminal_public_viewport_controller::scroll_lines(
    int line_delta)
{
    if (!m_release_intent.has_public_viewport || line_delta == 0) {
        return {};
    }

    const int previous_offset = deferred_scroll_base_offset_from_tail();
    const long long requested_offset =
        static_cast<long long>(previous_offset) + static_cast<long long>(line_delta);
    const int target_offset = static_cast<int>(
        std::clamp(
            requested_offset,
            0LL,
            static_cast<long long>(max_offset_from_tail())));

    if (!m_release_intent.public_projection_valid) {
        return record_deferred_offset_intent(
            target_offset,
            deferred_line_delta_from_public_viewport(target_offset),
            false);
    }

    return apply_offset(target_offset, false, target_offset != previous_offset);
}

Terminal_public_viewport_scroll_result
Terminal_public_viewport_controller::scroll_to_offset_from_tail(int offset_from_tail)
{
    if (!m_release_intent.has_public_viewport) {
        return {};
    }

    const int target_offset =
        std::clamp(offset_from_tail, 0, max_offset_from_tail());
    const bool sticky_tail_after_request = offset_from_tail == 0;

    if (!m_release_intent.public_projection_valid) {
        return record_deferred_offset_intent(
            target_offset,
            deferred_line_delta_from_public_viewport(target_offset),
            sticky_tail_after_request);
    }

    return apply_offset(
        target_offset,
        sticky_tail_after_request,
        !sticky_tail_after_request);
}

Terminal_public_viewport_scroll_result Terminal_public_viewport_controller::scroll_to_tail()
{
    return scroll_to_offset_from_tail(0);
}

void Terminal_public_viewport_controller::invalidate(
    Terminal_public_projection_disable_reason reason,
    Terminal_public_scroll_diagnostic_reason  diagnostic_reason)
{
    if (!m_release_intent.has_public_viewport ||
        !m_release_intent.public_projection_valid)
    {
        return;
    }

    m_release_intent.public_projection_valid = false;
    m_release_intent.public_projection_disable_reason = reason;
    m_release_intent.diagnostic_reason =
        diagnostic_reason != Terminal_public_scroll_diagnostic_reason::NONE
            ? diagnostic_reason
            : diagnostic_reason_for_disable_reason(reason);
}

void Terminal_public_viewport_controller::record_selection_mutation_unsupported()
{
    if (!m_release_intent.has_public_viewport) {
        return;
    }

    // Diagnostic precedence is deferred release intent first, explicit
    // invalidation diagnostics second, and selection-unsupported last.
    if (m_release_intent.diagnostic_reason != Terminal_public_scroll_diagnostic_reason::NONE) {
        return;
    }

    m_release_intent.diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::SELECTION_PUBLIC_PROJECTION_UNSUPPORTED;
}

int Terminal_public_viewport_controller::max_offset_from_tail() const
{
    return m_release_intent.has_public_viewport
        ? std::max(0, m_release_intent.public_viewport.scrollback_rows)
        : 0;
}

int Terminal_public_viewport_controller::first_public_row_for_offset(
    int offset_from_tail) const
{
    const Terminal_viewport_state& viewport = m_release_intent.public_viewport;
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return 0;
    }

    return viewport.scrollback_rows - offset_from_tail;
}

bool Terminal_public_viewport_controller::viewport_rows_are_copied_at_offset(
    int offset_from_tail) const
{
    if (!m_release_intent.has_public_viewport ||
        m_copied_row_metadata.empty())
    {
        return false;
    }

    const Terminal_viewport_state& viewport = m_release_intent.public_viewport;
    const int first_public_row = first_public_row_for_offset(offset_from_tail);
    const int last_public_row  = first_public_row + viewport.visible_rows - 1;
    const int last_copied_public_row =
        m_first_copied_public_row + static_cast<int>(m_copied_row_metadata.size()) - 1;

    return
        first_public_row >= m_first_copied_public_row &&
        last_public_row  <= last_copied_public_row;
}

void Terminal_public_viewport_controller::refresh_detached_anchor()
{
    if (!m_release_intent.has_public_viewport ||
        m_release_intent.sticky_tail)
    {
        m_release_intent.detached_anchor.reset();
        return;
    }

    if (!viewport_rows_are_copied_at_offset(
            m_release_intent.public_viewport.offset_from_tail))
    {
        m_release_intent.detached_anchor.reset();
        return;
    }

    const int first_public_row =
        first_public_row_for_offset(m_release_intent.public_viewport.offset_from_tail);
    const std::size_t copied_index =
        static_cast<std::size_t>(first_public_row - m_first_copied_public_row);
    const Terminal_public_projection_row_metadata& metadata =
        m_copied_row_metadata[copied_index];

    Terminal_public_viewport_anchor anchor;
    anchor.public_projection_generation = m_release_intent.public_projection_generation;
    anchor.active_buffer                 = m_release_intent.public_viewport.active_buffer;
    anchor.active_buffer_epoch           = m_active_buffer_epoch;
    anchor.geometry_generation           = m_content_basis.grid_reflow_generation;
    anchor.history_handle                = metadata.history_handle;
    anchor.visual_fragment_index = metadata.visual_fragment_index;
    anchor.visual_fragment_index_is_exact =
        metadata.visual_fragment_index_is_exact;
    anchor.viewport_row          = 0;
    anchor.sticky_tail           = m_release_intent.sticky_tail;

    m_release_intent.detached_anchor = anchor;
}

void Terminal_public_viewport_controller::set_sticky_tail(bool sticky_tail)
{
    m_release_intent.sticky_tail = sticky_tail;
    refresh_detached_anchor();
}

Terminal_public_viewport_scroll_result Terminal_public_viewport_controller::apply_offset(
    int   target_offset_from_tail,
    bool  sticky_tail_after_request,
    bool  request_records_detached_intent)
{
    const int previous_offset = m_release_intent.public_viewport.offset_from_tail;

    if (target_offset_from_tail != previous_offset &&
        !viewport_rows_are_copied_at_offset(target_offset_from_tail))
    {
        invalidate(Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED);
        return record_deferred_offset_intent(
            target_offset_from_tail,
            target_offset_from_tail - previous_offset,
            sticky_tail_after_request);
    }

    if (request_records_detached_intent || sticky_tail_after_request) {
        set_sticky_tail(sticky_tail_after_request);
    }

    if (target_offset_from_tail == previous_offset) {
        return {};
    }

    m_release_intent.public_viewport.offset_from_tail = target_offset_from_tail;
    m_release_intent.public_viewport.follow_tail      = target_offset_from_tail == 0;
    refresh_detached_anchor();

    Terminal_public_viewport_scroll_result result;
    result.viewport_result = {
        Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        target_offset_from_tail - previous_offset,
    };
    return result;
}

Terminal_public_viewport_scroll_result
Terminal_public_viewport_controller::record_deferred_offset_intent(
    int   offset_from_tail,
    int   line_delta,
    bool  sticky_tail_after_request)
{
    set_sticky_tail(sticky_tail_after_request);
    m_release_intent.deferred_intent_recorded = true;
    m_release_intent.deferred_offset_from_tail = offset_from_tail;
    m_release_intent.deferred_line_delta = line_delta;
    m_release_intent.detached_anchor.reset();
    // A deferred local scroll describes the release-visible action, so it
    // supersedes earlier selection-unsupported diagnostics.
    m_release_intent.diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT;
    m_release_intent.hidden_row_eligibility =
        Terminal_hidden_row_eligibility::INELIGIBLE;
    m_release_intent.hidden_row_clamp_reason =
        Terminal_hidden_row_clamp_reason::PUBLIC_VIEWPORT_BOUNDARY;

    Terminal_public_viewport_scroll_result result;
    result.deferred_release_intent_recorded = true;
    result.invalidated_public_projection =
        m_release_intent.public_projection_disable_reason !=
            Terminal_public_projection_disable_reason::NONE;
    return result;
}

}
