#pragma once

#include "vnm_terminal/internal/ime_contract.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_render_cell_text.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QByteArray>
#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <algorithm>
#include <memory>
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
    INVALID_CELL_TEXT_CATEGORY,
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
    INVALID_SNAPSHOT_BASIS_PURPOSE,
    INVALID_CELL_ORDER,
};

enum class Terminal_render_snapshot_basis
{
    LIVE_CONTENT,
    PUBLIC_PROJECTION,
};

enum class Terminal_render_snapshot_purpose
{
    CONTENT,
    SELECTION_DERIVED,
    GEOMETRY_DERIVED,
    SCROLL,
};

enum class Terminal_synchronized_output_scroll_policy
{
    DEFER_UNTIL_CONTENT_PUBLICATION,
    IMMEDIATE_PUBLIC_PROJECTION,
};

enum class Terminal_synchronized_output_policy_change_event
{
    NONE,
    CHANGED_MID_HOLD,
};

enum class Terminal_public_scroll_diagnostic_reason
{
    NONE,
    SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD,
    PUBLIC_PROJECTION_GEOMETRY_INVALIDATED,
    PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED,
    PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT,
    PUBLIC_PROJECTION_SCROLL_PUBLICATION_FAILED,
    DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED,
    DETACHED_ANCHOR_GEOMETRY_CHANGED,
    DETACHED_ANCHOR_NOT_RETAINED,
    SELECTION_PUBLIC_PROJECTION_UNSUPPORTED,
    SCREEN_BUFFER_EPOCH_CHANGED,
    BUFFER_TRANSITION_RELEASED,
};

enum class Terminal_release_reconciliation_result
{
    NONE,
    STICKY_TAIL,
    EXACT_ANCHOR,
    RETAINED_ID_BEST_EFFORT,
    NEAREST_SUCCESSOR,
    NEAREST_PREDECESSOR,
    OLDEST_AVAILABLE_LIVE,
    DEFERRED_OFFSET,
    INCOMPATIBLE_BUFFER,
};

enum class Terminal_hidden_row_eligibility
{
    NOT_EVALUATED,
    ELIGIBLE,
    INELIGIBLE,
};

enum class Terminal_hidden_row_clamp_reason
{
    NONE,
    PUBLIC_VIEWPORT_BOUNDARY,
    LIVE_VIEWPORT_BOUNDARY,
};

enum class Terminal_public_projection_disable_reason
{
    NONE,
    GEOMETRY_INVALIDATED,
    MEMORY_PRESSURE,
    PROJECTION_INVALIDATED,
    UNSUPPORTED_BUFFER,
};

inline QString render_snapshot_basis_name(Terminal_render_snapshot_basis basis)
{
    switch (basis) {
        case Terminal_render_snapshot_basis::LIVE_CONTENT:
            return QStringLiteral("LIVE_CONTENT");
        case Terminal_render_snapshot_basis::PUBLIC_PROJECTION:
            return QStringLiteral("PUBLIC_PROJECTION");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString render_snapshot_purpose_name(Terminal_render_snapshot_purpose purpose)
{
    switch (purpose) {
        case Terminal_render_snapshot_purpose::CONTENT:
            return QStringLiteral("CONTENT");
        case Terminal_render_snapshot_purpose::SELECTION_DERIVED:
            return QStringLiteral("SELECTION_DERIVED");
        case Terminal_render_snapshot_purpose::GEOMETRY_DERIVED:
            return QStringLiteral("GEOMETRY_DERIVED");
        case Terminal_render_snapshot_purpose::SCROLL:
            return QStringLiteral("SCROLL");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString synchronized_output_scroll_policy_name(
    Terminal_synchronized_output_scroll_policy policy)
{
    switch (policy) {
        case Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION:
            return QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION");
        case Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION:
            return QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString synchronized_output_policy_change_event_name(
    Terminal_synchronized_output_policy_change_event event)
{
    switch (event) {
        case Terminal_synchronized_output_policy_change_event::NONE:
            return QStringLiteral("none");
        case Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD:
            return QStringLiteral("changed_mid_hold");
    }

    return QStringLiteral("unknown");
}

inline QString public_scroll_diagnostic_reason_name(
    Terminal_public_scroll_diagnostic_reason reason)
{
    switch (reason) {
        case Terminal_public_scroll_diagnostic_reason::NONE:
            return QStringLiteral("none");
        case Terminal_public_scroll_diagnostic_reason::
            SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD:
            return QStringLiteral("synchronized_output_scroll_policy_changed_mid_hold");
        case Terminal_public_scroll_diagnostic_reason::PUBLIC_PROJECTION_GEOMETRY_INVALIDATED:
            return QStringLiteral("public_projection_geometry_invalidated");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED:
            return QStringLiteral("public_projection_memory_pressure_invalidated");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT:
            return QStringLiteral("public_projection_invalidated_deferred_intent");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_SCROLL_PUBLICATION_FAILED:
            return QStringLiteral("public_projection_scroll_publication_failed");
        case Terminal_public_scroll_diagnostic_reason::
            DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED:
            return QStringLiteral("detached_anchor_content_generation_changed");
        case Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_GEOMETRY_CHANGED:
            return QStringLiteral("detached_anchor_geometry_changed");
        case Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_NOT_RETAINED:
            return QStringLiteral("detached_anchor_not_retained");
        case Terminal_public_scroll_diagnostic_reason::SELECTION_PUBLIC_PROJECTION_UNSUPPORTED:
            return QStringLiteral("selection_public_projection_unsupported");
        case Terminal_public_scroll_diagnostic_reason::SCREEN_BUFFER_EPOCH_CHANGED:
            return QStringLiteral("screen_buffer_epoch_changed");
        case Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED:
            return QStringLiteral("buffer_transition_released");
    }

    return QStringLiteral("unknown");
}

inline QString release_reconciliation_result_name(
    Terminal_release_reconciliation_result result)
{
    switch (result) {
        case Terminal_release_reconciliation_result::NONE:
            return QStringLiteral("none");
        case Terminal_release_reconciliation_result::STICKY_TAIL:
            return QStringLiteral("sticky_tail");
        case Terminal_release_reconciliation_result::EXACT_ANCHOR:
            return QStringLiteral("exact_anchor");
        case Terminal_release_reconciliation_result::RETAINED_ID_BEST_EFFORT:
            return QStringLiteral("retained_id_best_effort");
        case Terminal_release_reconciliation_result::NEAREST_SUCCESSOR:
            return QStringLiteral("nearest_successor");
        case Terminal_release_reconciliation_result::NEAREST_PREDECESSOR:
            return QStringLiteral("nearest_predecessor");
        case Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE:
            return QStringLiteral("oldest_available_live");
        case Terminal_release_reconciliation_result::DEFERRED_OFFSET:
            return QStringLiteral("deferred_offset");
        case Terminal_release_reconciliation_result::INCOMPATIBLE_BUFFER:
            return QStringLiteral("incompatible_buffer");
    }

    return QStringLiteral("unknown");
}

inline QString hidden_row_eligibility_name(Terminal_hidden_row_eligibility eligibility)
{
    switch (eligibility) {
        case Terminal_hidden_row_eligibility::NOT_EVALUATED:
            return QStringLiteral("not_evaluated");
        case Terminal_hidden_row_eligibility::ELIGIBLE:
            return QStringLiteral("eligible");
        case Terminal_hidden_row_eligibility::INELIGIBLE:
            return QStringLiteral("ineligible");
    }

    return QStringLiteral("unknown");
}

inline QString hidden_row_clamp_reason_name(Terminal_hidden_row_clamp_reason reason)
{
    switch (reason) {
        case Terminal_hidden_row_clamp_reason::NONE:
            return QStringLiteral("none");
        case Terminal_hidden_row_clamp_reason::PUBLIC_VIEWPORT_BOUNDARY:
            return QStringLiteral("public_viewport_boundary");
        case Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY:
            return QStringLiteral("live_viewport_boundary");
    }

    return QStringLiteral("unknown");
}

inline QString public_projection_disable_reason_name(
    Terminal_public_projection_disable_reason reason)
{
    switch (reason) {
        case Terminal_public_projection_disable_reason::NONE:
            return QStringLiteral("none");
        case Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED:
            return QStringLiteral("geometry_invalidated");
        case Terminal_public_projection_disable_reason::MEMORY_PRESSURE:
            return QStringLiteral("memory_pressure");
        case Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED:
            return QStringLiteral("projection_invalidated");
        case Terminal_public_projection_disable_reason::UNSUPPORTED_BUFFER:
            return QStringLiteral("unsupported_buffer");
    }

    return QStringLiteral("unknown");
}

struct Terminal_render_cell
{
    terminal_grid_position_t   position;
    Terminal_render_cell_text  text;
    std::uint64_t              hyperlink_id      = 0U;
    int                        display_width     = 1;
    bool                       wide_continuation = false;
    Terminal_style_id          style_id          = k_default_terminal_style_id;
    Terminal_render_cell_text_category
                               text_category     = Terminal_render_cell_text_category::UNKNOWN;
};

static_assert(sizeof(Terminal_render_cell) <= 48U);

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
    // Wall-clock time of the last content change (ms since epoch); zero means
    // the line was never written. Not part of row identity: the stamp only
    // moves when content_generation advances, so equality stays decided by the
    // generation and the field rides along for GUI-thread timestamp lookups.
    qint64                     content_stamp_ms   = 0;
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

struct Terminal_public_scroll_diagnostics
{
    Terminal_synchronized_output_scroll_policy effective_policy =
        Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    Terminal_synchronized_output_policy_change_event policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
    Terminal_public_scroll_diagnostic_reason diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::NONE;
    std::uint64_t public_projection_generation = 0U;
    Terminal_viewport_state public_viewport_before;
    Terminal_viewport_state public_viewport_after;
    Terminal_viewport_state live_viewport_before_on_release;
    Terminal_viewport_state live_viewport_after_on_release;
    bool visible_scroll_applied = false;
    bool live_content_publication_blocked = false;
    Terminal_release_reconciliation_result release_reconciliation_result =
        Terminal_release_reconciliation_result::NONE;
    Terminal_hidden_row_eligibility hidden_row_eligibility =
        Terminal_hidden_row_eligibility::NOT_EVALUATED;
    Terminal_hidden_row_clamp_reason hidden_row_clamp_reason =
        Terminal_hidden_row_clamp_reason::NONE;
    Terminal_public_projection_disable_reason public_projection_disable_reason =
        Terminal_public_projection_disable_reason::NONE;
};

struct Terminal_render_snapshot_request
{
    std::uint64_t                         sequence = 0U;
    std::uint64_t                         row_origin_generation = 0U;
    Terminal_render_snapshot_basis        basis = Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose      purpose = Terminal_render_snapshot_purpose::CONTENT;
    Terminal_public_scroll_diagnostics    public_scroll_diagnostics;
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
    Terminal_render_snapshot_basis                     basis = Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose                   purpose = Terminal_render_snapshot_purpose::CONTENT;
    Terminal_public_scroll_diagnostics                 public_scroll_diagnostics;
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

// Two snapshots coalesce their dirty rows by physical row index only when they
// describe the same physical row-identity space: same grid, same viewport
// mapping, and the same row-origin generation. A scrollback-eviction row-origin
// change shifts which logical line each physical index names, so coalescing
// across it would carry a stale or missing row; the row-origin guard forces a
// full repaint instead.
inline bool snapshots_share_row_identity_space(
    const Terminal_render_snapshot&    left,
    const Terminal_render_snapshot&    right)
{
    return
        grid_sizes_match(left.grid_size, right.grid_size)                           &&
        left.metadata.row_origin_generation == right.metadata.row_origin_generation &&
        viewport_mappings_match(left.viewport, right.viewport);
}

inline void append_dirty_range_rows(
    std::vector<int>&                                   rows,
    const std::vector<Terminal_render_dirty_row_range>& ranges)
{
    for (const Terminal_render_dirty_row_range& range : ranges) {
        for (int row = range.first_row; row < range.first_row + range.row_count; ++row) {
            rows.push_back(row);
        }
    }
}

// Merge the dirty rows of a previously published snapshot into a newer one when
// both share a physical row-identity space, returning the newer snapshot with a
// coalesced dirty-row range set. Diverging grid, viewport, or row-origin yields
// a full-viewport repaint range.
inline Terminal_render_snapshot snapshot_with_coalesced_dirty_rows(
    const Terminal_render_snapshot&        previous_snapshot,
    Terminal_render_snapshot               snapshot)
{
    if (!snapshots_share_row_identity_space(previous_snapshot, snapshot)) {
        snapshot.dirty_row_ranges =
            compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        return snapshot;
    }

    std::vector<int> dirty_rows;
    append_dirty_range_rows(dirty_rows, previous_snapshot.dirty_row_ranges);
    append_dirty_range_rows(dirty_rows, snapshot.dirty_row_ranges);
    snapshot.dirty_row_ranges =
        compact_dirty_row_ranges(std::move(dirty_rows), snapshot.grid_size.rows, false);
    return snapshot;
}

// Shared-pointer wrapper over snapshot_with_coalesced_dirty_rows for callers
// that publish the coalesced snapshot through a shared_ptr<const> channel.
inline std::shared_ptr<const Terminal_render_snapshot> coalesced_dirty_row_snapshot_handle(
    const Terminal_render_snapshot&    previous,
    const Terminal_render_snapshot&    current)
{
    Terminal_render_snapshot coalesced = snapshot_with_coalesced_dirty_rows(previous, current);
    return std::make_shared<const Terminal_render_snapshot>(std::move(coalesced));
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

        cell->text.append_to(text);
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

inline Terminal_render_cell_text_category render_cell_text_category_for_validation(
    const QString& text)
{
    if (text.isEmpty()) {
        return Terminal_render_cell_text_category::EMPTY;
    }

    constexpr unsigned int printable_ascii_first = 0x20U;
    constexpr unsigned int printable_ascii_last  = 0x7eU;

    unsigned int outside_printable_ascii = 0U;
    unsigned int non_ascii               = 0U;
    const qsizetype text_size            = text.size();
    const QChar* characters              = text.data();
    for (qsizetype index = 0; index < text_size; ++index) {
        const unsigned int code_unit = characters[index].unicode();
        outside_printable_ascii |= static_cast<unsigned int>(
            code_unit - printable_ascii_first >
                printable_ascii_last - printable_ascii_first);
        non_ascii |= code_unit;
    }

    if (outside_printable_ascii == 0U) {
        return Terminal_render_cell_text_category::PRINTABLE_ASCII;
    }

    return (non_ascii & ~0x7fU) != 0U
        ? Terminal_render_cell_text_category::NON_ASCII
        : Terminal_render_cell_text_category::OTHER_ASCII;
}

inline bool render_cell_text_category_is_valid(const Terminal_render_cell& cell)
{
    return
        cell.text.is_valid() &&
        (cell.text_category == Terminal_render_cell_text_category::UNKNOWN ||
            cell.text_category == cell.text.category());
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

    const bool public_projection_basis =
        snapshot.basis == Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    const bool scroll_purpose =
        snapshot.purpose == Terminal_render_snapshot_purpose::SCROLL;
    if (public_projection_basis != scroll_purpose) {
        return {Terminal_render_snapshot_status::INVALID_SNAPSHOT_BASIS_PURPOSE};
    }
    if (public_projection_basis &&
        snapshot.viewport.active_buffer != Terminal_buffer_id::PRIMARY)
    {
        return {Terminal_render_snapshot_status::INVALID_VIEWPORT};
    }
    if (public_projection_basis &&
        (snapshot.dirty_row_ranges.size() != 1U ||
            snapshot.dirty_row_ranges.front().first_row != 0 ||
            snapshot.dirty_row_ranges.front().row_count != snapshot.grid_size.rows))
    {
        return {Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE};
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

        // Snapshot cells are stored row-major with strictly ascending columns
        // within each row. Both producers (the live-content model row loop and
        // the public-projection scroll assembly) emit cells in this order, and
        // the frame builder iterates snapshot.cells directly relying on it.
        // Enforce the contract here so a violating snapshot fails loudly instead
        // of mis-rendering.
        if (i > 0) {
            const Terminal_render_cell& previous = snapshot.cells[i - 1];
            const bool strictly_after =
                cell.position.row > previous.position.row ||
                (cell.position.row    == previous.position.row &&
                 cell.position.column >  previous.position.column);
            if (!strictly_after) {
                return {Terminal_render_snapshot_status::INVALID_CELL_ORDER};
            }
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
            if (!cell.text.is_empty() || cell.display_width != 0 || cell.position.column == 0) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }
            if (!render_cell_text_category_is_valid(cell)) {
                return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
            }
            continue;
        }

        if (cell.display_width <= 0 ||
            cell.display_width >  snapshot.grid_size.columns - cell.position.column)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_WIDTH};
        }

        if (!render_cell_text_category_is_valid(cell)) {
            return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
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
