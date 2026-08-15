#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/backend_output_capture_writer.h"

#include "vnm_terminal/internal/interaction_trace.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include <QKeyEvent>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace vnm_terminal::internal {

namespace {

constexpr std::size_t k_pending_notification_limit = 4096U;
constexpr std::size_t k_selection_trace_span_limit = 4U;
constexpr qsizetype   k_backend_output_drain_slice_bytes = 4096;
constexpr QByteArrayView k_focus_in_report("\x1b[I", 3);
constexpr QByteArrayView k_focus_out_report("\x1b[O", 3);

using Backend_callback_drain_deadline =
    std::optional<std::chrono::steady_clock::time_point>;

Terminal_backend_error make_backend_error(
    Terminal_backend_error_code    code,
    QString                        message)
{
    return {code, std::move(message)};
}

bool model_result_warrants_render_snapshot(const Terminal_screen_model_result& result)
{
    return
        !result.dirty_rows.empty() ||
        result.viewport_changed    ||
        result.mode_state_changed  ||
        result.mouse_reporting_mode_changed;
}

void apply_trailing_changes(
    Terminal_screen_model_result&                         result,
    const terminal_screen_model_trailing_changes_t& trailing_changes)
{
    result.terminal_content_changed = trailing_changes.terminal_content_changed;
    result.active_buffer_changed = trailing_changes.active_buffer_changed;
    result.grid_reflow_changed = trailing_changes.grid_reflow_changed;
    result.viewport_changed = trailing_changes.viewport_changed;
    result.mode_state_changed = trailing_changes.mode_state_changed;
    result.mouse_reporting_mode_changed =
        trailing_changes.mouse_reporting_mode_changed;
    result.alternate_scroll_mode_changed =
        trailing_changes.alternate_scroll_mode_changed;
}

void coalesce_dirty_rows(std::vector<int>& dirty_rows, const std::vector<int>& added_rows)
{
    dirty_rows.insert(dirty_rows.end(), added_rows.begin(), added_rows.end());
    std::sort(dirty_rows.begin(), dirty_rows.end());
    dirty_rows.erase(
        std::unique(dirty_rows.begin(), dirty_rows.end()),
        dirty_rows.end());
}

void coalesce_backend_content_model_result(
    Terminal_screen_model_result&       target,
    const Terminal_screen_model_result& update)
{
    coalesce_dirty_rows(target.dirty_rows, update.dirty_rows);
    target.dirty_rows_have_stable_mutation_identity =
        target.dirty_rows_have_stable_mutation_identity &&
        update.dirty_rows_have_stable_mutation_identity;
    target.terminal_content_changed =
        target.terminal_content_changed || update.terminal_content_changed;
    target.active_buffer_changed =
        target.active_buffer_changed || update.active_buffer_changed;
    target.grid_reflow_changed =
        target.grid_reflow_changed || update.grid_reflow_changed;
    target.viewport_changed =
        target.viewport_changed || update.viewport_changed;
    target.mode_state_changed =
        target.mode_state_changed || update.mode_state_changed;
    target.mouse_reporting_mode_changed =
        target.mouse_reporting_mode_changed || update.mouse_reporting_mode_changed;
    target.alternate_scroll_mode_changed =
        target.alternate_scroll_mode_changed || update.alternate_scroll_mode_changed;
    target.scrollback_rows          = update.scrollback_rows;
    target.evicted_scrollback_rows += update.evicted_scrollback_rows;
}

bool backend_callback_drain_deadline_reached(
    const Backend_callback_drain_deadline& deadline)
{
    return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

bool model_allows_render_snapshot(const Terminal_screen_model& model)
{
    return !model.mode_state().synchronized_output;
}

bool render_snapshot_can_advance_latest_content_snapshot(
    const Terminal_render_snapshot& snapshot)
{
    return
        snapshot.basis   == Terminal_render_snapshot_basis::LIVE_CONTENT &&
        snapshot.purpose == Terminal_render_snapshot_purpose::CONTENT;
}

template<typename T>
std::uint64_t vector_payload_bytes(const std::vector<T>& values)
{
    return
        static_cast<std::uint64_t>(values.capacity()) *
        static_cast<std::uint64_t>(sizeof(T));
}

std::uint64_t byte_array_payload_bytes(const QByteArray& value)
{
    return static_cast<std::uint64_t>(value.capacity());
}

std::uint64_t string_payload_bytes(const QString& value)
{
    return
        static_cast<std::uint64_t>(sizeof(QString)) +
        static_cast<std::uint64_t>(value.capacity()) *
            static_cast<std::uint64_t>(sizeof(QChar));
}

std::uint64_t render_cell_text_payload_bytes(const Terminal_render_cell_text& text)
{
    const QString* fallback = text.fallback_qstring_or_null();
    return fallback != nullptr ? string_payload_bytes(*fallback) : 0U;
}

std::uint64_t render_snapshot_payload_bytes(const Terminal_render_snapshot& snapshot)
{
    std::uint64_t bytes =
        vector_payload_bytes(snapshot.styles)                  +
        vector_payload_bytes(snapshot.cells)                   +
        vector_payload_bytes(snapshot.visible_line_provenance) +
        vector_payload_bytes(snapshot.dirty_row_ranges)        +
        vector_payload_bytes(snapshot.hyperlinks)              +
        vector_payload_bytes(snapshot.selection_spans)         +
        vector_payload_bytes(snapshot.search_match_spans);
    for (const Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        bytes += byte_array_payload_bytes(hyperlink.identity_key);
        bytes += byte_array_payload_bytes(hyperlink.uri);
    }
    const Terminal_render_snapshot_row_content_view rows(snapshot);
    for (const Terminal_render_snapshot_row_content row : rows) {
        for (const Terminal_render_cell& cell : row) {
            bytes += render_cell_text_payload_bytes(cell.text);
        }
    }
    return bytes;
}

struct retained_render_snapshot_stats_t
{
    std::uint64_t payload_bytes    = 0U;
    std::uint64_t generation_count = 0U;
};

retained_render_snapshot_stats_t retained_render_snapshot_stats(
    const std::shared_ptr<const Terminal_render_snapshot>& latest_render,
    const std::shared_ptr<const Terminal_render_snapshot>& latest_content)
{
    retained_render_snapshot_stats_t stats;
    if (latest_render != nullptr) {
        stats.payload_bytes += render_snapshot_payload_bytes(*latest_render);
        ++stats.generation_count;
    }
    if (latest_content != nullptr && latest_content.get() != latest_render.get()) {
        stats.payload_bytes += render_snapshot_payload_bytes(*latest_content);
        ++stats.generation_count;
    }
    return stats;
}

void update_retained_render_snapshot_profile_stats(
    Terminal_session_profile_stats& profile_stats,
    const std::shared_ptr<const Terminal_render_snapshot>& latest_render,
    const std::shared_ptr<const Terminal_render_snapshot>& latest_content)
{
    const retained_render_snapshot_stats_t retained =
        retained_render_snapshot_stats(latest_render, latest_content);
    profile_stats.retained_snapshot_payload_bytes    = retained.payload_bytes;
    profile_stats.retained_snapshot_generation_count = retained.generation_count;
    profile_stats.max_retained_snapshot_payload_bytes =
        std::max(
            profile_stats.max_retained_snapshot_payload_bytes,
            retained.payload_bytes);
    profile_stats.max_retained_snapshot_generation_count =
        std::max(
            profile_stats.max_retained_snapshot_generation_count,
            retained.generation_count);
}

void record_consumer_materialization(
    Terminal_session_profile_stats*                 profile_stats,
    Terminal_render_snapshot_materialization_reason reason,
    std::uint64_t                                   rows,
    std::uint64_t                                   cells)
{
    if (profile_stats == nullptr || !profile_stats->enabled) {
        return;
    }

    switch (reason) {
        case Terminal_render_snapshot_materialization_reason::GEOMETRY_DERIVED_SNAPSHOT:
            ++profile_stats->geometry_derived_materialization_calls;
            profile_stats->geometry_derived_materialization_rows  += rows;
            profile_stats->geometry_derived_materialization_cells += cells;
            break;
    }
}

bool snapshot_is_public_projection_scroll(const Terminal_render_snapshot& snapshot)
{
    return
        snapshot.basis   == Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
        snapshot.purpose == Terminal_render_snapshot_purpose::SCROLL;
}

int first_public_row_for_viewport(const Terminal_viewport_state& viewport)
{
    return viewport.active_buffer == Terminal_buffer_id::ALTERNATE
        ? 0
        : viewport.scrollback_rows - viewport.offset_from_tail;
}

bool selection_ranges_match(
    const Terminal_selection_range& left,
    const Terminal_selection_range& right)
{
    return
        left.start.row    == right.start.row    &&
        left.start.column == right.start.column &&
        left.end.row      == right.end.row      &&
        left.end.column   == right.end.column   &&
        left.mode         == right.mode;
}

bool selection_range_was_materialized(
    const std::vector<Terminal_selection_range>& materialized_ranges,
    const Terminal_selection_range&              range)
{
    return std::any_of(
        materialized_ranges.begin(),
        materialized_ranges.end(),
        [&](const Terminal_selection_range& materialized_range) {
            return selection_ranges_match(materialized_range, range);
        });
}

std::optional<Terminal_render_selection_span>
materialize_public_projection_selection_span(
    const Terminal_selection_range& source_range,
    int                             public_row,
    int                             column_count)
{
    if (source_range.mode == Terminal_selection_mode::NONE) {
        return std::nullopt;
    }

    const terminal_grid_position_t start = normalized_selection_start(source_range);
    const terminal_grid_position_t end   = normalized_selection_end(source_range);
    if (public_row < start.row || public_row > end.row) {
        return std::nullopt;
    }

    const int first_column = public_row == start.row ? start.column : 0;
    const int end_column =
        public_row == end.row ? end.column : column_count;
    if (first_column < 0       || first_column > column_count ||
        end_column   < 0       || end_column   > column_count ||
        end_column   <= first_column)
    {
        return std::nullopt;
    }

    return Terminal_render_selection_span{
        source_range,
        0,
        first_column,
        end_column - first_column,
    };
}

void append_public_projection_selection_span_if_visible(
    Terminal_render_snapshot&          snapshot,
    const Terminal_public_projection&  projection,
    int                                first_public_row,
    int                                public_row,
    Terminal_render_selection_span     span)
{
    const int snapshot_row = public_row - first_public_row;
    if (snapshot_row < 0 || snapshot_row >= snapshot.grid_size.rows) {
        return;
    }

    const int copied_index = public_row - projection.first_copied_public_row();
    if (copied_index < 0 ||
        copied_index >= static_cast<int>(projection.rows().size()))
    {
        return;
    }

    const Terminal_public_projection_row& projection_row =
        projection.rows()[static_cast<std::size_t>(copied_index)];
    if (projection_row.public_row != public_row ||
        !(projection_row.provenance ==
            snapshot.visible_line_provenance[static_cast<std::size_t>(snapshot_row)]))
    {
        return;
    }

    span.row = snapshot_row;
    snapshot.selection_spans.push_back(std::move(span));
}

void append_public_projection_selection_spans(
    Terminal_render_snapshot&           snapshot,
    const Terminal_public_projection&   projection,
    int                                 first_public_row)
{
    const std::vector<Terminal_render_selection_span>& safe_basis_spans =
        projection.safe_basis_viewport_selection_spans();
    std::vector<Terminal_selection_range> materialized_ranges;
    materialized_ranges.reserve(safe_basis_spans.size());

    for (const Terminal_render_selection_span& safe_basis_span : safe_basis_spans) {
        if (selection_range_was_materialized(
                materialized_ranges,
                safe_basis_span.source_range))
        {
            continue;
        }
        materialized_ranges.push_back(safe_basis_span.source_range);

        const terminal_grid_position_t start =
            normalized_selection_start(safe_basis_span.source_range);
        const terminal_grid_position_t end =
            normalized_selection_end(safe_basis_span.source_range);
        const int first_visible_selected_public_row =
            std::max(first_public_row, start.row);
        const int last_visible_selected_public_row =
            std::min(first_public_row + snapshot.grid_size.rows - 1, end.row);

        for (int public_row = first_visible_selected_public_row;
             public_row <= last_visible_selected_public_row;
             ++public_row)
        {
            std::optional<Terminal_render_selection_span> span =
                materialize_public_projection_selection_span(
                    safe_basis_span.source_range,
                    public_row,
                    snapshot.grid_size.columns);
            if (!span.has_value()) {
                continue;
            }

            append_public_projection_selection_span_if_visible(
                snapshot,
                projection,
                first_public_row,
                public_row,
                std::move(*span));
        }
    }
}

std::optional<Terminal_render_snapshot> public_projection_scroll_snapshot_from_projection(
    const Terminal_public_projection&    projection,
    Terminal_viewport_state              viewport,
    std::uint64_t                        sequence,
    Terminal_public_scroll_diagnostics   diagnostics)
{
    const terminal_grid_size_t grid_size = projection.grid_size();
    if (viewport.active_buffer != projection.active_buffer() ||
        viewport.visible_rows  != grid_size.rows)
    {
        return std::nullopt;
    }

    const int first_public_row = first_public_row_for_viewport(viewport);
    const int first_copied_index =
        first_public_row - projection.first_copied_public_row();
    if (first_copied_index < 0 ||
        first_copied_index + viewport.visible_rows >
            static_cast<int>(projection.rows().size()))
    {
        return std::nullopt;
    }

    Terminal_render_snapshot snapshot;
    snapshot.basis                     = Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    snapshot.purpose                   = Terminal_render_snapshot_purpose::SCROLL;
    snapshot.public_scroll_diagnostics = diagnostics;
    snapshot.grid_size                 = grid_size;
    snapshot.viewport                  = viewport;
    snapshot.color_state               = projection.color_state();
    snapshot.styles                    = projection.styles();
    snapshot.hyperlinks                = projection.hyperlinks();
    snapshot.cursor                    = projection.cursor();
    snapshot.ime_preedit               = projection.ime_preedit();
    snapshot.metadata                  = projection.metadata();
    snapshot.metadata.sequence         = sequence;
    snapshot.modes                     = projection.modes();
    snapshot.dirty_row_ranges          = {{0, viewport.visible_rows}};

    snapshot.visible_line_provenance.reserve(static_cast<std::size_t>(viewport.visible_rows));
    std::size_t projected_cell_count = 0U;
    for (int viewport_row = 0; viewport_row < viewport.visible_rows; ++viewport_row) {
        const Terminal_public_projection_row& projection_row =
            projection.rows()[static_cast<std::size_t>(first_copied_index + viewport_row)];
        projected_cell_count += projection_row.cells.size();
    }
    snapshot.cells.reserve(projected_cell_count);
    for (int viewport_row = 0; viewport_row < viewport.visible_rows; ++viewport_row) {
        const Terminal_public_projection_row& projection_row =
            projection.rows()[static_cast<std::size_t>(first_copied_index + viewport_row)];
        snapshot.visible_line_provenance.push_back(projection_row.provenance);

        for (Terminal_render_cell cell : projection_row.cells) {
            cell.position.row = viewport_row;
            snapshot.cells.push_back(std::move(cell));
        }
    }

    if (snapshot.cursor.visible) {
        const int cursor_public_row =
            first_public_row_for_viewport(projection.viewport()) + snapshot.cursor.position.row;
        snapshot.cursor.position.row = cursor_public_row - first_public_row;
        snapshot.cursor.visible =
            snapshot.cursor.position.row >= 0 &&
            snapshot.cursor.position.row <  snapshot.grid_size.rows;
    }

    append_public_projection_selection_spans(snapshot, projection, first_public_row);

    if (validate_render_snapshot(snapshot).status != Terminal_render_snapshot_status::OK) {
        return std::nullopt;
    }

    return snapshot;
}

bool viewport_states_match(
    const Terminal_viewport_state& left,
    const Terminal_viewport_state& right)
{
    return
        viewport_mappings_match(left, right)                                      &&
        left.follow_tail                      == right.follow_tail                &&
        left.alternate_screen_scroll_policy   == right.alternate_screen_scroll_policy;
}

bool selection_snapshot_matches_safe_content_basis(
    const Terminal_render_snapshot& selection_snapshot,
    const Terminal_render_snapshot& safe_content)
{
    return
        selection_snapshot.basis == Terminal_render_snapshot_basis::LIVE_CONTENT &&
        safe_content.basis       == Terminal_render_snapshot_basis::LIVE_CONTENT &&
        grid_sizes_match(selection_snapshot.grid_size, safe_content.grid_size)   &&
        viewport_mappings_match(selection_snapshot.viewport, safe_content.viewport) &&
        selection_snapshot.metadata.row_origin_generation ==
            safe_content.metadata.row_origin_generation &&
        render_snapshot_visible_line_provenance_is_valid(selection_snapshot) &&
        render_snapshot_visible_line_provenance_is_valid(safe_content)       &&
        selection_snapshot.visible_line_provenance ==
            safe_content.visible_line_provenance;
}

Terminal_selection_anchor_domain selection_anchor_domain_for_buffer(
    Terminal_buffer_id buffer_id)
{
    return buffer_id == Terminal_buffer_id::ALTERNATE
        ? Terminal_selection_anchor_domain::ALTERNATE_ACTIVE_GRID
        : Terminal_selection_anchor_domain::PRIMARY_BACKING;
}

bool selection_source_identities_match(
    const terminal_selection_source_identity_t& left,
    const terminal_selection_source_identity_t& right)
{
    return
        left.source_content_basis == right.source_content_basis      &&
        left.anchor_domain        == right.anchor_domain             &&
        left.session_epoch        == right.session_epoch             &&
        left.buffer_id            == right.buffer_id                 &&
        left.grid_reflow_basis    == right.grid_reflow_basis         &&
        left.row_origin_generation == right.row_origin_generation    &&
        grid_sizes_match(left.grid_size, right.grid_size)            &&
        viewport_mappings_match(left.viewport_mapping, right.viewport_mapping);
}

bool selection_sources_have_compatible_content(
    const terminal_selection_source_identity_t& left,
    const terminal_selection_source_identity_t& right)
{
    return
        left.source_content_basis == right.source_content_basis &&
        left.anchor_domain        == right.anchor_domain        &&
        left.session_epoch        == right.session_epoch        &&
        left.buffer_id            == right.buffer_id            &&
        left.grid_reflow_basis    == right.grid_reflow_basis    &&
        left.row_origin_generation == right.row_origin_generation &&
        grid_sizes_match(left.grid_size, right.grid_size);
}

bool selection_lease_matches_source(
    const terminal_selection_visual_lease_t&    lease,
    const terminal_selection_source_identity_t& source)
{
    return
        lease.source_content_basis == source.source_content_basis &&
        lease.anchor_domain        == source.anchor_domain        &&
        lease.session_epoch        == source.session_epoch        &&
        lease.buffer_id            == source.buffer_id            &&
        lease.grid_reflow_basis    == source.grid_reflow_basis    &&
        lease.row_origin_generation == source.row_origin_generation &&
        grid_sizes_match(lease.grid_size, source.grid_size)       &&
        viewport_mappings_match(lease.viewport_mapping, source.viewport_mapping);
}

bool selection_lease_has_compatible_visual_source(
    const terminal_selection_visual_lease_t&    lease,
    const terminal_selection_source_identity_t& source,
    bool                                        allow_viewport_projection)
{
    return
        lease.source_content_basis == source.source_content_basis &&
        lease.anchor_domain        == source.anchor_domain        &&
        lease.session_epoch        == source.session_epoch        &&
        lease.buffer_id            == source.buffer_id            &&
        lease.grid_reflow_basis    == source.grid_reflow_basis    &&
        lease.row_origin_generation == source.row_origin_generation &&
        grid_sizes_match(lease.grid_size, source.grid_size)       &&
        (allow_viewport_projection ||
            viewport_mappings_match(lease.viewport_mapping, source.viewport_mapping));
}

bool selection_visual_leases_match(
    const terminal_selection_visual_lease_t& left,
    const terminal_selection_visual_lease_t& right)
{
    return
        left.source_content_basis         == right.source_content_basis         &&
        left.anchor_domain                == right.anchor_domain                &&
        left.session_epoch                == right.session_epoch                &&
        left.buffer_id                    == right.buffer_id                    &&
        left.grid_reflow_basis            == right.grid_reflow_basis            &&
        left.row_origin_generation        == right.row_origin_generation        &&
        grid_sizes_match(left.grid_size, right.grid_size)                       &&
        viewport_mappings_match(left.viewport_mapping, right.viewport_mapping)  &&
        left.selected_range                == right.selected_range                &&
        left.anchor                        == right.anchor                        &&
        left.extent                        == right.extent                        &&
        left.durable_payload_identity      == right.durable_payload_identity      &&
        left.provisional_payload_identity  == right.provisional_payload_identity  &&
        left.selected_lines                == right.selected_lines;
}

bool selection_trace_requested(bool stderr_enabled)
{
    return stderr_enabled || interaction_trace_enabled();
}

void write_selection_trace(bool enabled, const QString& message)
{
    record_interaction_trace("selection", "state", message);
    if (!enabled) {
        return;
    }

    std::fprintf(stderr, "[vnm-terminal-selection] %s\n", qPrintable(message));
}

QString selection_trace_bool(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

QString selection_trace_grid_position(terminal_grid_position_t position)
{
    return QStringLiteral("%1:%2").arg(position.row).arg(position.column);
}

QString selection_trace_range(const Terminal_selection_range& range)
{
    return QStringLiteral("%1-%2,mode=%3")
        .arg(selection_trace_grid_position(range.start))
        .arg(selection_trace_grid_position(range.end))
        .arg(static_cast<int>(range.mode));
}

template <typename ranges_t>
QString selection_trace_ranges(const ranges_t& ranges)
{
    if (ranges.empty()) {
        return QStringLiteral("none");
    }

    QString text;
    for (const Terminal_selection_range& range : ranges) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += selection_trace_range(range);
    }
    return text;
}

QString selection_trace_selection_requests(
    const std::vector<Terminal_render_selection_request>& requests)
{
    if (requests.empty()) {
        return QStringLiteral("none");
    }

    QString text;
    for (const Terminal_render_selection_request& request : requests) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += selection_trace_range(request.range);
    }
    return text;
}

QString selection_trace_selection_spans(
    const std::vector<Terminal_render_selection_span>& spans)
{
    if (spans.empty()) {
        return QStringLiteral("none");
    }

    QString text;
    const std::size_t span_count =
        std::min(spans.size(), k_selection_trace_span_limit);
    for (std::size_t index = 0U; index < span_count; ++index) {
        const Terminal_render_selection_span& span = spans[index];
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += QStringLiteral("row=%1,cols=%2+%3,source=%4")
            .arg(span.row)
            .arg(span.first_column)
            .arg(span.column_count)
            .arg(selection_trace_range(span.source_range));
    }
    if (spans.size() > span_count) {
        text += QStringLiteral(";...");
    }
    return text;
}

QString selection_trace_grid_size(terminal_grid_size_t grid_size)
{
    return QStringLiteral("%1x%2").arg(grid_size.rows).arg(grid_size.columns);
}

QString selection_trace_content_basis(terminal_selection_content_basis_t content_basis)
{
    return QStringLiteral("content=%1,reflow=%2")
        .arg(static_cast<qulonglong>(content_basis.content_generation))
        .arg(static_cast<qulonglong>(content_basis.grid_reflow_generation));
}

QString selection_trace_anchor_domain(Terminal_selection_anchor_domain domain)
{
    return QString::number(static_cast<int>(domain));
}

QString selection_trace_viewport(const Terminal_viewport_state& viewport)
{
    return QStringLiteral("buffer=%1,visible=%2,scrollback=%3,offset=%4")
        .arg(static_cast<int>(viewport.active_buffer))
        .arg(viewport.visible_rows)
        .arg(viewport.scrollback_rows)
        .arg(viewport.offset_from_tail);
}

QString selection_trace_source_identity(
    const terminal_selection_source_identity_t& source)
{
    return QStringLiteral(
        "source{basis={%1},domain=%2,epoch=%3,buffer=%4,grid_reflow=%5,"
        "row_origin=%6,grid=%7,viewport={%8}}")
        .arg(selection_trace_content_basis(source.source_content_basis))
        .arg(selection_trace_anchor_domain(source.anchor_domain))
        .arg(static_cast<qulonglong>(source.session_epoch))
        .arg(static_cast<int>(source.buffer_id))
        .arg(static_cast<qulonglong>(source.grid_reflow_basis))
        .arg(static_cast<qulonglong>(source.row_origin_generation))
        .arg(selection_trace_grid_size(source.grid_size))
        .arg(selection_trace_viewport(source.viewport_mapping));
}

QString selection_trace_source_identity(
    const std::optional<terminal_selection_source_identity_t>& source)
{
    return source.has_value()
        ? selection_trace_source_identity(*source)
        : QStringLiteral("source{none}");
}

QString selection_trace_visual_lease(
    const terminal_selection_visual_lease_t& lease)
{
    QString line_leases = QStringLiteral("none");
    if (!lease.selected_lines.empty()) {
        line_leases.clear();
        const std::size_t line_count = std::min(
            lease.selected_lines.size(),
            k_selection_trace_span_limit);
        for (std::size_t index = 0U; index < line_count; ++index) {
            const terminal_selection_line_lease_t& line = lease.selected_lines[index];
            if (!line_leases.isEmpty()) {
                line_leases += QLatin1Char(';');
            }
            line_leases += QStringLiteral("offset=%1,epoch=%2,row=%3,generation=%4")
                .arg(line.row_offset)
                .arg(static_cast<qulonglong>(line.history_handle.epoch))
                .arg(static_cast<qulonglong>(line.history_handle.row_sequence))
                .arg(static_cast<qulonglong>(line.history_handle.content_generation));
        }
        if (lease.selected_lines.size() > line_count) {
            line_leases += QStringLiteral(";...");
        }
    }

    return QStringLiteral(
        "lease{basis={%1},domain=%2,epoch=%3,buffer=%4,grid_reflow=%5,"
        "row_origin=%6,grid=%7,viewport={%8},range=%9,anchor=%10,extent=%11,"
        "durable=%12,provisional=%13,line_lease_count=%14,line_leases={%15}}")
        .arg(selection_trace_content_basis(lease.source_content_basis))
        .arg(selection_trace_anchor_domain(lease.anchor_domain))
        .arg(static_cast<qulonglong>(lease.session_epoch))
        .arg(static_cast<int>(lease.buffer_id))
        .arg(static_cast<qulonglong>(lease.grid_reflow_basis))
        .arg(static_cast<qulonglong>(lease.row_origin_generation))
        .arg(selection_trace_grid_size(lease.grid_size))
        .arg(selection_trace_viewport(lease.viewport_mapping))
        .arg(selection_trace_range(lease.selected_range))
        .arg(selection_trace_grid_position(lease.anchor))
        .arg(selection_trace_grid_position(lease.extent))
        .arg(static_cast<qulonglong>(lease.durable_payload_identity))
        .arg(static_cast<qulonglong>(lease.provisional_payload_identity))
        .arg(static_cast<qulonglong>(lease.selected_lines.size()))
        .arg(line_leases);
}

QString selection_trace_visual_lease(
    const std::optional<terminal_selection_visual_lease_t>& lease)
{
    return lease.has_value()
        ? selection_trace_visual_lease(*lease)
        : QStringLiteral("lease{none}");
}

QString selection_trace_history_handle(const terminal_history_handle_t& handle)
{
    return QStringLiteral("epoch=%1,byte=%2,row=%3,bytes=%4,generation=%5")
        .arg(static_cast<qulonglong>(handle.epoch))
        .arg(static_cast<qulonglong>(handle.byte_sequence))
        .arg(static_cast<qulonglong>(handle.row_sequence))
        .arg(handle.record_bytes)
        .arg(static_cast<qulonglong>(handle.content_generation));
}

QString selection_trace_history_handles(
    const std::vector<terminal_history_handle_t>& handles)
{
    if (handles.empty()) {
        return QStringLiteral("none");
    }
    QString text;
    const std::size_t count = std::min(handles.size(), k_selection_trace_span_limit);
    for (std::size_t index = 0U; index < count; ++index) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += QStringLiteral("{%1}").arg(selection_trace_history_handle(handles[index]));
    }
    if (handles.size() > count) {
        text += QStringLiteral(";...");
    }
    return text;
}

QString selection_trace_int_values(const std::vector<int>& values)
{
    if (values.empty()) {
        return QStringLiteral("none");
    }

    QString text;
    const std::size_t count = std::min(values.size(), k_selection_trace_span_limit);
    for (std::size_t index = 0U; index < count; ++index) {
        if (!text.isEmpty()) {
            text += QLatin1Char(',');
        }
        text += QString::number(values[index]);
    }
    if (values.size() > count) {
        text += QStringLiteral(",...");
    }
    return text;
}

QString selection_trace_continuity(
    const std::optional<terminal_selection_continuity_capability_t>& continuity)
{
    if (!continuity.has_value()) {
        return QStringLiteral("continuity{absent}");
    }

    QString successors;
    std::size_t successor_count = 0U;
    for (const auto& [retained_line_id, candidates] :
        continuity->successors_by_old_retained_line_id)
    {
        for (const terminal_selection_line_successor_t& successor : candidates) {
            if (successor_count >= k_selection_trace_span_limit) {
                successors += QStringLiteral(";...");
                break;
            }
            if (!successors.isEmpty()) {
                successors += QLatin1Char(';');
            }
            successors += QStringLiteral(
                "key=%1,old={%2},final={%3},old_row=%4,final_row=%5,delta=%6")
                .arg(static_cast<qulonglong>(retained_line_id))
                .arg(selection_trace_history_handle(successor.old_handle))
                .arg(selection_trace_history_handle(successor.final_handle))
                .arg(successor.old_logical_row)
                .arg(successor.final_logical_row)
                .arg(successor.row_delta);
            ++successor_count;
        }
        if (successor_count >= k_selection_trace_span_limit) {
            break;
        }
    }
    if (successors.isEmpty()) {
        successors = QStringLiteral("none");
    }

    QString proofs;
    const std::size_t proof_count = std::min(
        continuity->survivor_proofs.size(),
        k_selection_trace_span_limit);
    for (std::size_t index = 0U; index < proof_count; ++index) {
        const terminal_selection_survivor_proof_t& proof =
            continuity->survivor_proofs[index];
        if (!proofs.isEmpty()) {
            proofs += QLatin1Char(';');
        }
        proofs += QStringLiteral(
            "old={%1},predecessor={%2},final={%3},result=%4,old_row=%5,final_row=%6")
            .arg(selection_trace_history_handle(proof.old_handle))
            .arg(selection_trace_history_handle(proof.candidate_predecessor_handle))
            .arg(selection_trace_history_handle(proof.final_handle))
            .arg(static_cast<int>(proof.result))
            .arg(proof.old_logical_row)
            .arg(proof.final_logical_row);
    }
    if (continuity->survivor_proofs.size() > proof_count) {
        proofs += QStringLiteral(";...");
    }
    if (proofs.isEmpty()) {
        proofs = QStringLiteral("none");
    }

    return QStringLiteral("continuity{version=%1,successors={%2},proofs={%3}}")
        .arg(continuity->version)
        .arg(successors)
        .arg(proofs);
}

QString selection_trace_recovery_proposals(
    const std::vector<terminal_recovery_proposal_t>& proposals)
{
    if (proposals.empty()) {
        return QStringLiteral("none");
    }

    QString text;
    for (const terminal_recovery_proposal_t& proposal : proposals) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += QStringLiteral(
            "reason=%1,status=%2,source=%3,candidate_rows=%4,recovered_rows=%5,"
            "matched_prefix=%6,unmatched_tail=%7,ambiguous=%8")
            .arg(static_cast<int>(proposal.reason))
            .arg(static_cast<int>(proposal.status))
            .arg(static_cast<int>(proposal.provenance_source))
            .arg(proposal.candidate_visible_rows)
            .arg(proposal.recovered_row_count)
            .arg(proposal.matched_prefix_rows)
            .arg(proposal.unmatched_tail_rows)
            .arg(selection_trace_bool(proposal.visible_row_identity_ambiguous));
    }
    return text;
}

QString selection_trace_recovery_attempts(
    const std::vector<terminal_recovery_attempt_t>& attempts,
    std::uint64_t                                   dropped_attempts)
{
    QString text = attempts.empty() ? QStringLiteral("none") : QString();
    for (const terminal_recovery_attempt_t& attempt : attempts) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += QStringLiteral("status=%1,reason=%2,candidate_rows=%3,recovered_rows=%4")
            .arg(QString::fromLatin1(
                terminal_recovery_attempt_status_token(attempt.status)))
            .arg(QString::fromLatin1(
                terminal_recovery_attempt_reason_token(attempt.reason)))
            .arg(attempt.candidate_visible_rows)
            .arg(attempt.recovered_row_count);
    }
    if (dropped_attempts > 0U) {
        if (!text.isEmpty()) {
            text += QLatin1Char(';');
        }
        text += QStringLiteral("dropped=%1")
            .arg(static_cast<qulonglong>(dropped_attempts));
    }
    return text;
}

void append_selection_trace_reason(QString& reasons, const QString& reason)
{
    if (!reasons.isEmpty()) {
        reasons += QLatin1Char(',');
    }
    reasons += reason;
}

QString selection_trace_source_mismatch_reason(
    const terminal_selection_source_identity_t& left,
    const terminal_selection_source_identity_t& right)
{
    QString reasons;
    if (left.source_content_basis != right.source_content_basis) {
        append_selection_trace_reason(reasons, QStringLiteral("content-basis"));
    }
    if (left.anchor_domain != right.anchor_domain) {
        append_selection_trace_reason(reasons, QStringLiteral("anchor-domain"));
    }
    if (left.session_epoch != right.session_epoch) {
        append_selection_trace_reason(reasons, QStringLiteral("epoch"));
    }
    if (left.buffer_id != right.buffer_id) {
        append_selection_trace_reason(reasons, QStringLiteral("buffer"));
    }
    if (left.grid_reflow_basis != right.grid_reflow_basis) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-reflow"));
    }
    if (left.row_origin_generation != right.row_origin_generation) {
        append_selection_trace_reason(reasons, QStringLiteral("row-origin"));
    }
    if (!grid_sizes_match(left.grid_size, right.grid_size)) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-size"));
    }
    if (!viewport_mappings_match(left.viewport_mapping, right.viewport_mapping)) {
        append_selection_trace_reason(reasons, QStringLiteral("viewport-mapping"));
    }
    return reasons.isEmpty() ? QStringLiteral("none") : reasons;
}

bool selection_state_allows_span_emission(Terminal_selection_internal_state state)
{
    return
        state == Terminal_selection_internal_state::DRAG_PREVIEW ||
        state == Terminal_selection_internal_state::ATTACHED;
}

std::vector<terminal_selection_line_lease_t> selection_line_leases_from_snapshot(
    const Terminal_render_snapshot& snapshot,
    const Terminal_selection_range& range)
{
    std::vector<terminal_selection_line_lease_t> lines;
    if (!render_snapshot_visible_line_provenance_is_valid(snapshot)) {
        return lines;
    }

    const terminal_grid_position_t start = normalized_selection_start(range);
    const terminal_grid_position_t end   = normalized_selection_end(range);
    const int first_visible_logical_row  = render_snapshot_first_visible_logical_row(snapshot);
    const int last_visible_logical_row   = first_visible_logical_row + snapshot.grid_size.rows - 1;
    if (start.row < first_visible_logical_row || end.row > last_visible_logical_row) {
        return lines;
    }

    lines.reserve(static_cast<std::size_t>(end.row - start.row + 1));
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int viewport_row = logical_row - first_visible_logical_row;
        const Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(viewport_row)];
        lines.push_back(terminal_selection_line_lease_from_retained_identity(
            logical_row - start.row,
            provenance.retained_line_id,
            provenance.content_generation));
    }
    return lines;
}

bool active_model_matches_published_selection_source(
    const Terminal_screen_model&                model,
    const terminal_selection_source_identity_t& source,
    terminal_selection_content_basis_t          content_basis,
    std::uint64_t                               session_epoch,
    std::uint64_t                               row_origin_generation,
    const Terminal_viewport_state&              viewport)
{
    return
        model_allows_render_snapshot(model)                         &&
        source.source_content_basis == content_basis                 &&
        source.anchor_domain == selection_anchor_domain_for_buffer(
            model.active_buffer_id())                                &&
        source.session_epoch        == session_epoch                 &&
        source.buffer_id            == model.active_buffer_id()      &&
        source.grid_reflow_basis    == content_basis.grid_reflow_generation  &&
        source.row_origin_generation == row_origin_generation        &&
        grid_sizes_match(source.grid_size, model.grid_size())        &&
        viewport_mappings_match(source.viewport_mapping, viewport);
}

bool model_should_publish_render_snapshot(
    const Terminal_screen_model&           model,
    const Terminal_screen_model_result&    result)
{
    return
        model_result_warrants_render_snapshot(result) &&
        model_allows_render_snapshot(model);
}

std::optional<Terminal_buffer_id> active_buffer_after_from_mode_transition_delta(
    const Terminal_screen_model_result& result)
{
    for (const terminal_backing_delta_t& delta : result.backing_deltas) {
        if (delta.kind == Terminal_backing_delta_kind::MODE_TRANSITIONED) {
            return delta.active_buffer_after;
        }
    }

    return std::nullopt;
}

void sync_viewport_controller_to_snapshot(
    Terminal_viewport_controller&     controller,
    const Terminal_viewport_state&    viewport)
{
    controller.set_visible_rows(viewport.visible_rows);
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        controller.enter_alternate_screen();
        return;
    }

    controller.leave_alternate_screen();
    controller.set_scrollback_rows(viewport.scrollback_rows);
    const int offset_delta =
        viewport.offset_from_tail - controller.state().offset_from_tail;
    if (offset_delta != 0) {
        (void)controller.scroll_lines(offset_delta);
    }
}

int bounded_string_size(const QString& text)
{
    return static_cast<int>(
        std::min<qsizetype>(text.size(), std::numeric_limits<int>::max()));
}

bool cell_position_fits_grid(
    const Terminal_render_cell&    cell,
    terminal_grid_size_t           grid_size)
{
    return
        cell.position.row    >= 0              &&
        cell.position.row    <  grid_size.rows &&
        cell.position.column >= 0              &&
        cell.position.column <  grid_size.columns;
}

bool continuation_matches_base(
    const Terminal_render_cell&    continuation,
    const Terminal_render_cell&    base)
{
    return
        continuation.wide_continuation         &&
        continuation.style_id == base.style_id &&
        continuation.hyperlink_id == base.hyperlink_id;
}

std::vector<Terminal_render_cell> cells_adapted_to_grid(
    const Terminal_render_snapshot_row_content_view& rows,
    terminal_grid_size_t                            grid_size)
{
    std::vector<Terminal_render_cell> adapted_cells;
    adapted_cells.reserve(rows.cell_count());
    for (const Terminal_render_snapshot_row_content row : rows) {
        for (auto cell_it = row.begin(); cell_it != row.end(); ++cell_it) {
            const Terminal_render_cell& cell = *cell_it;
            if (!cell_position_fits_grid(cell, grid_size) || cell.wide_continuation) {
                continue;
            }

            if (cell.display_width <= 0 ||
                cell.display_width >  grid_size.columns - cell.position.column)
            {
                continue;
            }

            auto continuation_it = cell_it;
            bool complete_cell_span = true;
            for (int column_delta = 1; column_delta < cell.display_width; ++column_delta) {
                ++continuation_it;
                if (continuation_it == row.end() ||
                    continuation_it->position.column != cell.position.column + column_delta ||
                    !continuation_matches_base(*continuation_it, cell))
                {
                    complete_cell_span = false;
                    break;
                }
            }

            if (!complete_cell_span) {
                continue;
            }

            adapted_cells.push_back(cell);
            continuation_it = cell_it;
            for (int column_delta = 1; column_delta < cell.display_width; ++column_delta) {
                ++continuation_it;
                adapted_cells.push_back(*continuation_it);
            }
        }
    }

    return adapted_cells;
}

bool snapshot_cells_reference_hyperlink(
    const std::vector<Terminal_render_cell>&           cells,
    Terminal_hyperlink_id                              hyperlink_id)
{
    return std::any_of(
        cells.begin(),
        cells.end(),
        [hyperlink_id](const Terminal_render_cell& cell) {
            return cell.hyperlink_id == hyperlink_id;
        });
}

std::vector<Terminal_render_hyperlink_metadata> hyperlinks_referenced_by_cells(
    const std::vector<Terminal_render_hyperlink_metadata>& hyperlinks,
    const std::vector<Terminal_render_cell>&               cells)
{
    std::vector<Terminal_render_hyperlink_metadata> referenced_hyperlinks;
    referenced_hyperlinks.reserve(hyperlinks.size());
    for (const Terminal_render_hyperlink_metadata& hyperlink : hyperlinks) {
        if (snapshot_cells_reference_hyperlink(cells, hyperlink.hyperlink_id)) {
            referenced_hyperlinks.push_back(hyperlink);
        }
    }

    return referenced_hyperlinks;
}

Terminal_viewport_state viewport_adapted_to_grid(
    Terminal_viewport_state            viewport,
    terminal_grid_size_t               grid_size)
{
    viewport.visible_rows = grid_size.rows;
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        viewport.scrollback_rows  = 0;
        viewport.offset_from_tail = 0;
        viewport.follow_tail      = true;
        return viewport;
    }

    viewport.scrollback_rows = std::max(0, viewport.scrollback_rows);
    viewport.offset_from_tail =
        std::clamp(viewport.offset_from_tail, 0, viewport.scrollback_rows);
    viewport.follow_tail = viewport.offset_from_tail == 0;
    return viewport;
}

Terminal_render_snapshot geometry_snapshot_from_public_snapshot(
    const Terminal_render_snapshot&    public_snapshot,
    terminal_grid_size_t               grid_size,
    std::uint64_t                      sequence,
    bool                               backend_geometry_in_sync,
    Terminal_session_profile_stats*    profile_stats)
{
    const bool content_preserved = grid_sizes_match(public_snapshot.grid_size, grid_size);

    Terminal_render_snapshot snapshot;
    snapshot.basis                   = Terminal_render_snapshot_basis::LIVE_CONTENT;
    snapshot.purpose                 = Terminal_render_snapshot_purpose::GEOMETRY_DERIVED;
    snapshot.grid_size               = grid_size;
    snapshot.viewport                = viewport_adapted_to_grid(public_snapshot.viewport, grid_size);
    snapshot.color_state             = public_snapshot.color_state;
    snapshot.styles                  = public_snapshot.styles;
    snapshot.visible_line_provenance = public_snapshot.visible_line_provenance;
    snapshot.hyperlinks              = public_snapshot.hyperlinks;
    snapshot.cursor                  = public_snapshot.cursor;
    snapshot.ime_preedit             = public_snapshot.ime_preedit;
    snapshot.metadata                = public_snapshot.metadata;
    snapshot.modes                   = public_snapshot.modes;
    const Terminal_render_snapshot_row_content_view public_rows(public_snapshot);
    snapshot.cells = cells_adapted_to_grid(public_rows, grid_size);
    record_consumer_materialization(
        profile_stats,
        Terminal_render_snapshot_materialization_reason::GEOMETRY_DERIVED_SNAPSHOT,
        static_cast<std::uint64_t>(snapshot.grid_size.rows),
        static_cast<std::uint64_t>(snapshot.cells.size()));
    snapshot.hyperlinks = hyperlinks_referenced_by_cells(snapshot.hyperlinks, snapshot.cells);
    if (!content_preserved || !render_snapshot_visible_line_provenance_is_valid(snapshot)) {
        snapshot.visible_line_provenance.clear();
    }
    snapshot.selection_spans.clear();
    snapshot.search_match_spans.clear();
    snapshot.dirty_row_ranges                      = compact_dirty_row_ranges({}, grid_size.rows, true);
    snapshot.metadata.sequence                     = sequence;
    snapshot.metadata.backend_geometry_in_sync     = backend_geometry_in_sync;
    snapshot.metadata.visual_bell_active           = false;
    snapshot.metadata.mouse_reporting_mode_changed = false;
    if (snapshot.cursor.visible) {
        snapshot.cursor.position.row =
            std::clamp(snapshot.cursor.position.row, 0, grid_size.rows - 1);
        snapshot.cursor.position.column =
            std::clamp(snapshot.cursor.position.column, 0, grid_size.columns - 1);
    }
    return snapshot;
}

struct Sync_parameter_location
{
    bool       found       = false;
    qsizetype  group_begin = -1;
    qsizetype  prefix_end  = -1;
};

struct Sync_set_sequence
{
    qsizetype  start           = -1;
    qsizetype  end             = -1;
    qsizetype  parameter_begin = -1;
    qsizetype  parameter_end   = -1;
    qsizetype  sync_begin      = -1;
    qsizetype  prefix_end      = -1;
};

bool is_csi_parameter_byte(unsigned char byte)
{
    return byte >= 0x30U && byte <= 0x3fU;
}

bool is_csi_intermediate_byte(unsigned char byte)
{
    return byte >= 0x20U && byte <= 0x2fU;
}

bool is_csi_final_byte(unsigned char byte)
{
    return byte >= 0x40U && byte <= 0x7eU;
}

Sync_parameter_location find_sync_parameter_location(QByteArrayView parameter_bytes)
{
    if (parameter_bytes.empty() || parameter_bytes[0] != '?') {
        return {};
    }

    bool        saw_sync_parameter = false;
    qsizetype   sync_group_begin   = -1;
    qsizetype   sync_prefix_end    = -1;
    std::size_t group_count        = 0U;
    qsizetype   offset             = 1;
    for (;;) {
        if (group_count >= k_csi_parameter_group_limit) {
            return {};
        }

        const qsizetype group_begin = offset;
        while (offset               <  parameter_bytes.size() &&
            parameter_bytes[offset] >= '0'                    &&
            parameter_bytes[offset] <= '9')
        {
            ++offset;
        }

        const qsizetype length = offset - group_begin;
        if (length                           <= 0 ||
            static_cast<std::size_t>(length) >  k_csi_parameter_digit_limit)
        {
            return {};
        }
        ++group_count;

        if (length                           == 4   &&
            parameter_bytes[group_begin]     == '2' &&
            parameter_bytes[group_begin + 1] == '0' &&
            parameter_bytes[group_begin + 2] == '2' &&
            parameter_bytes[group_begin + 3] == '6')
        {
            saw_sync_parameter = true;
            sync_group_begin   = group_begin;
            sync_prefix_end    = group_begin;
            if (sync_prefix_end > 1 && parameter_bytes[sync_prefix_end - 1] == ';') {
                --sync_prefix_end;
            }
        }

        if (offset >= parameter_bytes.size()) {
            break;
        }

        if (parameter_bytes[offset] != ';') {
            return {};
        }
        ++offset;
    }

    return {
        saw_sync_parameter,
        sync_group_begin,
        sync_prefix_end,
    };
}

Sync_set_sequence next_synchronized_output_mode_sequence(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state,
    char                       mode_final_byte)
{
    Terminal_utf8_scan_state utf8_scan_state = initial_utf8_scan_state;
    for (qsizetype offset = 0; offset < bytes.size(); ++offset) {
        if (utf8_scan_consumes_byte(static_cast<unsigned char>(bytes[offset]), utf8_scan_state)) {
            continue;
        }

        qsizetype parameter_begin = -1;
        if (static_cast<unsigned char>(bytes[offset]) == 0x9bU) {
            parameter_begin = offset + 1;
        }
        else
        if (static_cast<unsigned char>(bytes[offset]) == 0x1bU        &&
            offset + 1                                <  bytes.size() &&
            bytes[offset + 1]                         == '[')
        {
            parameter_begin = offset + 2;
        }
        else {
            continue;
        }

        qsizetype cursor = parameter_begin;
        while (cursor < bytes.size() &&
            is_csi_parameter_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        const qsizetype parameter_end = cursor;
        while (cursor < bytes.size() &&
            is_csi_intermediate_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }
        const bool has_intermediates = cursor > parameter_end;

        if (cursor >= bytes.size()) {
            return {};
        }

        const unsigned char final_byte = static_cast<unsigned char>(bytes[cursor]);
        if (!is_csi_final_byte(final_byte)) {
            continue;
        }

        const QByteArrayView parameters(
            bytes.data() + parameter_begin,
            parameter_end - parameter_begin);
        const Sync_parameter_location sync_location =
            (final_byte == static_cast<unsigned char>(mode_final_byte) &&
                !has_intermediates)
                ? find_sync_parameter_location(parameters)
                : Sync_parameter_location{};
        if (sync_location.found)
        {
            return {
                offset,
                cursor + 1,
                parameter_begin,
                parameter_end,
                parameter_begin + sync_location.group_begin,
                parameter_begin + sync_location.prefix_end,
            };
        }

        offset = cursor;
    }

    return {};
}

Sync_set_sequence next_synchronized_output_set_sequence(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state)
{
    return next_synchronized_output_mode_sequence(bytes, initial_utf8_scan_state, 'h');
}

Sync_set_sequence next_synchronized_output_reset_sequence(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state)
{
    return next_synchronized_output_mode_sequence(bytes, initial_utf8_scan_state, 'l');
}

QByteArray csi_private_modes(QByteArrayView parameter_bytes, char mode_final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(parameter_bytes.data(), parameter_bytes.size());
    bytes.append(mode_final_byte);
    return bytes;
}

QByteArray sync_sequence_prefix(
    QByteArrayView             bytes,
    const Sync_set_sequence&   sequence,
    char                       mode_final_byte)
{
    if (sequence.prefix_end <= sequence.parameter_begin + 1) {
        return {};
    }

    return
        csi_private_modes(
            QByteArrayView(
                bytes.data() + sequence.parameter_begin,
                sequence.prefix_end - sequence.parameter_begin),
            mode_final_byte);
}

QByteArray sync_sequence_suffix_and_tail(QByteArrayView bytes, const Sync_set_sequence& sequence)
{
    QByteArray suffix = QByteArrayLiteral("\x1b[?");
    suffix.append(
        bytes.data() + sequence.sync_begin,
        sequence.parameter_end - sequence.sync_begin);
    suffix.append('h');
    suffix.append(bytes.data() + sequence.end, bytes.size() - sequence.end);
    return suffix;
}

QByteArray sync_sequence_only(char mode_final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[?2026");
    bytes.append(mode_final_byte);
    return bytes;
}

QByteArray sync_sequence_post_parameter_suffix_and_tail(
    QByteArrayView             bytes,
    const Sync_set_sequence&   sequence,
    char                       mode_final_byte)
{
    QByteArray suffix;
    const qsizetype sync_group_end = sequence.sync_begin + 4;
    if (sync_group_end < sequence.parameter_end) {
        const qsizetype suffix_begin = sync_group_end + 1;
        if (suffix_begin < sequence.parameter_end) {
            suffix = QByteArrayLiteral("\x1b[?");
            suffix.append(
                bytes.data() + suffix_begin,
                sequence.parameter_end - suffix_begin);
            suffix.append(mode_final_byte);
        }
    }

    suffix.append(bytes.data() + sequence.end, bytes.size() - sequence.end);
    return suffix;
}

QByteArray sync_sequence_from_sync_parameter_and_tail(
    QByteArrayView             bytes,
    const Sync_set_sequence&   sequence,
    char                       mode_final_byte)
{
    QByteArray rewritten = sync_sequence_only(mode_final_byte);
    rewritten.append(sync_sequence_post_parameter_suffix_and_tail(
        bytes,
        sequence,
        mode_final_byte));
    return rewritten;
}

qsizetype trailing_incomplete_csi_start(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state)
{
    Terminal_utf8_scan_state utf8_scan_state = initial_utf8_scan_state;
    for (qsizetype offset = 0; offset < bytes.size(); ++offset) {
        if (utf8_scan_consumes_byte(static_cast<unsigned char>(bytes[offset]), utf8_scan_state)) {
            continue;
        }

        qsizetype parameter_begin = -1;
        if (static_cast<unsigned char>(bytes[offset]) == 0x9bU) {
            parameter_begin = offset + 1;
        }
        else
        if (static_cast<unsigned char>(bytes[offset]) == 0x1bU) {
            if (offset + 1 >= bytes.size()) {
                return offset;
            }

            if (bytes[offset + 1] == '[') {
                parameter_begin = offset + 2;
            }
            else {
                continue;
            }
        }
        else {
            continue;
        }

        qsizetype cursor = parameter_begin;
        while (cursor < bytes.size() &&
            is_csi_parameter_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        while (cursor < bytes.size() &&
            is_csi_intermediate_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        if (cursor >= bytes.size()) {
            return offset;
        }

        if (is_csi_final_byte(static_cast<unsigned char>(bytes[cursor]))) {
            offset = cursor;
        }
    }

    return -1;
}

bool utf8_scan_state_is_reset(const Terminal_utf8_scan_state& state)
{
    return
        state.continuation_remaining == 0     &&
        state.next_minimum           == 0x80U &&
        state.next_maximum           == 0xbfU;
}

bool backend_output_is_plain_ascii_without_prescan_intro(QByteArrayView bytes)
{
    for (const char byte : bytes) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value == 0x1bU || value == 0x9bU || value >= 0x80U) {
            return false;
        }
    }

    return true;
}

bool uses_deferred_backend_callbacks(const Terminal_session_config& config)
{
    return
        static_cast<bool>(config.backend_event_notifier) ||
        static_cast<bool>(config.backend_event_epoch_notifier);
}

class Backend_callback_invocation
{
public:
    explicit Backend_callback_invocation(
        const std::shared_ptr<Terminal_session_callback_lifetime>& lifetime);

    ~Backend_callback_invocation();

    Backend_callback_invocation(const Backend_callback_invocation&)            = delete;
    Backend_callback_invocation& operator=(const Backend_callback_invocation&) = delete;

    Terminal_session* session() const { return m_session; }
    Terminal_queue_result enqueue(
        Terminal_session_command command);

private:
    std::shared_ptr<Terminal_session_callback_lifetime> m_lifetime;
    Terminal_session* m_session = nullptr;
};

}

class Terminal_session_callback_lifetime
{
public:
    // Backend threads enqueue commands through this shared lifetime object.
    // close() stops new callbacks and waits for in-flight callbacks before
    // Terminal_session members can be destroyed. Callback command count and
    // output bytes are bounded here because notifier-driven owners may not
    // drain while the GUI thread is busy.
    explicit Terminal_session_callback_lifetime(
        Terminal_session*      session,
        Terminal_queue_limits  output_queue_limits,
        bool                   coalesce_output_callbacks)
    :
        m_session(session),
        m_callback_queue_limits(output_queue_limits),
        m_pending_callback_queue(output_queue_limits),
        m_coalesce_output_callbacks(coalesce_output_callbacks)
    {}

    Terminal_session* begin_callback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting_callbacks || m_session == nullptr) {
            return nullptr;
        }

        ++m_active_callbacks;
        return m_session;
    }

    void end_callback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_active_callbacks;
        if (m_active_callbacks == 0U) {
            m_idle.notify_all();
        }
    }

    Terminal_queue_result enqueue(Terminal_session_command command)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_backend_callbacks_stopped) {
            if (command.kind == Terminal_session_command_kind::BACKEND_EXIT &&
                !m_backend_exit_after_stop_pending)
            {
                m_backend_exit_after_stop_pending = true;
                command.backend_callback_epoch = next_backend_callback_epoch_locked();
                m_pending_commands.push_back(std::move(command));
            }
            return {Terminal_queue_result_code::ACCEPTED, false};
        }

        const bool output_command =
            command.kind == Terminal_session_command_kind::BACKEND_OUTPUT;
        if (output_command) {
            if (m_backend_output_stopped) {
                return {Terminal_queue_result_code::ACCEPTED, false};
            }
        }

        const std::size_t byte_count =
            output_command
                ? static_cast<std::size_t>(command.bytes.size())
                : 0U;
        const bool append_to_previous_output =
            m_coalesce_output_callbacks                                                     &&
            output_command                                                                  &&
            !m_pending_commands.empty()                                                     &&
            m_pending_commands.back().kind == Terminal_session_command_kind::BACKEND_OUTPUT;
        const std::size_t command_count = append_to_previous_output ? 0U : 1U;
        const Terminal_queue_result result =
            m_pending_callback_queue.reserve(byte_count, command_count);
        if (result.code == Terminal_queue_result_code::HARD_LIMIT_REACHED) {
            drop_pending_output_locked();
            if (!m_backend_callback_overflow_report_pending) {
                Terminal_session_command error_command = make_backend_error_command(
                    0U,
                    make_backend_error(
                        Terminal_backend_error_code::OUTPUT_OVERFLOW,
                        QStringLiteral("pending backend callback hard limit reached")));
                error_command.backend_callback_epoch = next_backend_callback_epoch_locked();
                m_pending_commands.push_back(std::move(error_command));
                m_backend_callback_overflow_report_pending = true;
            }
            if (!output_command) {
                m_backend_callbacks_stopped = true;
            }
            m_backend_output_stopped = true;
            return result;
        }

        if (append_to_previous_output) {
            command.backend_callback_epoch = next_backend_callback_epoch_locked();
            m_pending_commands.back().backend_callback_epoch =
                command.backend_callback_epoch;
            m_pending_commands.back().bytes += command.bytes;
            return result;
        }

        command.backend_callback_epoch = next_backend_callback_epoch_locked();
        m_pending_commands.push_back(std::move(command));
        return result;
    }

    void enqueue_backend_output_capture_failure(Terminal_backend_error error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_backend_output_capture_failure_reported ||
            m_pending_backend_output_capture_failure.has_value())
        {
            return;
        }

        Terminal_session_command command = make_backend_error_command(0U, std::move(error));
        command.backend_callback_epoch = next_backend_callback_epoch_locked();
        m_pending_backend_output_capture_failure = std::move(command);
        m_backend_output_capture_failure_reported = true;
    }

    std::deque<Terminal_session_command> take_pending_commands(
        std::vector<std::uint64_t>& backend_output_capture_failure_epochs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::deque<Terminal_session_command> commands;
        commands.swap(m_pending_commands);
        backend_output_capture_failure_epochs.clear();
        if (m_pending_backend_output_capture_failure.has_value()) {
            backend_output_capture_failure_epochs.push_back(
                m_pending_backend_output_capture_failure->backend_callback_epoch);
            const auto position = std::lower_bound(
                commands.begin(),
                commands.end(),
                m_pending_backend_output_capture_failure->backend_callback_epoch,
                [](const Terminal_session_command& command, std::uint64_t epoch) {
                    return command.backend_callback_epoch < epoch;
                });
            commands.insert(
                position,
                std::move(*m_pending_backend_output_capture_failure));
            m_pending_backend_output_capture_failure.reset();
        }
        m_pending_callback_queue = Bounded_terminal_command_queue(m_callback_queue_limits);
        m_backend_callback_overflow_report_pending = false;
        return commands;
    }

    bool high_water_reached()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return m_pending_callback_queue.high_water_reached();
    }

    bool has_pending_or_active_callbacks()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return !m_pending_commands.empty() ||
            m_pending_backend_output_capture_failure.has_value() ||
            m_active_callbacks != 0U;
    }

    std::size_t pending_command_count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return m_pending_commands.size() +
            (m_pending_backend_output_capture_failure.has_value() ? 1U : 0U);
    }

    std::uint64_t last_enqueued_backend_callback_epoch() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return m_last_enqueued_backend_callback_epoch;
    }

    void stop_backend_output()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backend_output_stopped = true;

        drop_pending_output_locked();
    }

    void close()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_accepting_callbacks = false;
        m_session = nullptr;
        m_pending_commands.clear();
        m_pending_backend_output_capture_failure.reset();
        m_idle.wait(lock, [this] { return m_active_callbacks == 0U; });
        m_pending_commands.clear();
        m_pending_backend_output_capture_failure.reset();
    }

private:
    std::uint64_t next_backend_callback_epoch_locked()
    {
        const std::uint64_t epoch = m_next_backend_callback_epoch++;
        if (m_next_backend_callback_epoch == 0U) {
            m_next_backend_callback_epoch = 1U;
        }
        m_last_enqueued_backend_callback_epoch = epoch;
        return epoch;
    }

    void drop_pending_output_locked()
    {
        std::deque<Terminal_session_command> retained_commands;
        while (!m_pending_commands.empty()) {
            Terminal_session_command command = std::move(m_pending_commands.front());
            m_pending_commands.pop_front();
            if (command.kind != Terminal_session_command_kind::BACKEND_OUTPUT) {
                retained_commands.push_back(std::move(command));
            }
        }
        m_pending_commands.swap(retained_commands);
        m_pending_callback_queue = Bounded_terminal_command_queue(m_callback_queue_limits);
        for (const Terminal_session_command& command : m_pending_commands) {
            const std::size_t byte_count =
                command.kind == Terminal_session_command_kind::BACKEND_OUTPUT
                    ? static_cast<std::size_t>(command.bytes.size())
                    : 0U;
            (void)m_pending_callback_queue.reserve(byte_count);
        }
    }

    mutable std::mutex                   m_mutex;
    std::condition_variable              m_idle;
    Terminal_session*                    m_session = nullptr;
    std::deque<Terminal_session_command> m_pending_commands;
    std::optional<Terminal_session_command>
                                         m_pending_backend_output_capture_failure;
    Terminal_queue_limits                m_callback_queue_limits;
    Bounded_terminal_command_queue       m_pending_callback_queue;
    std::size_t                          m_active_callbacks = 0U;
    bool                                 m_accepting_callbacks = true;
    bool                                 m_backend_callbacks_stopped = false;
    bool                                 m_backend_exit_after_stop_pending = false;
    bool                                 m_backend_output_stopped = false;
    bool                                 m_backend_callback_overflow_report_pending = false;
    bool                                 m_backend_output_capture_failure_reported = false;
    bool                                 m_coalesce_output_callbacks = false;
    std::uint64_t                        m_next_backend_callback_epoch = 1U;
    std::uint64_t                        m_last_enqueued_backend_callback_epoch = 0U;
};

Backend_callback_invocation::Backend_callback_invocation(
    const std::shared_ptr<Terminal_session_callback_lifetime>& lifetime)
:
    m_lifetime(lifetime)
{
    if (m_lifetime != nullptr) {
        m_session = m_lifetime->begin_callback();
    }
}

Backend_callback_invocation::~Backend_callback_invocation()
{
    if (m_session != nullptr) {
        m_lifetime->end_callback();
    }
}

Terminal_queue_result Backend_callback_invocation::enqueue(
    Terminal_session_command command)
{
    if (m_session != nullptr) {
        return m_lifetime->enqueue(std::move(command));
    }

    return {Terminal_queue_result_code::ACCEPTED, false};
}

Terminal_session::Terminal_session(
    std::unique_ptr<Terminal_backend>  backend,
    Terminal_session_config            config)
:
    m_callback_lifetime(
        std::make_shared<Terminal_session_callback_lifetime>(
            this,
            config.output_queue_limits,
            uses_deferred_backend_callbacks(config))),
    m_backend(std::move(backend)),
    m_config(config),
    m_output_queue(m_config.output_queue_limits),
    m_write_queue(m_config.write_queue_limits)
{
    m_config.scrollback_limit = std::max(0, m_config.scrollback_limit);
    m_bell_state.policy = m_config.bell_policy;
    if (m_config.backend_output_capture_config.has_value()) {
        m_backend_output_capture_writer =
            std::make_unique<Backend_output_capture_writer>(
                *m_config.backend_output_capture_config);
    }
}

Terminal_session::~Terminal_session()
{
    m_callback_lifetime->close();
    m_backend.reset();
}

Terminal_session_result Terminal_session::start(Terminal_launch_config launch_config)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    launch_config.output_delivery_limits =
        Terminal_backend_output_delivery_limits{
            m_config.output_queue_limits.high_water_bytes,
            m_config.output_queue_limits.hard_limit_bytes,
        };

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(
        make_start_command(sequence, std::move(launch_config)));
}

Terminal_session_result Terminal_session::write_user_bytes(QByteArray bytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    return write_user_bytes_locked(
        std::move(bytes),
        User_write_viewport_policy::RETURN_TO_TAIL,
        Backend_callback_drain_policy::DRAIN_CALLBACKS);
}

Terminal_session_result Terminal_session::write_user_bytes_without_backend_drain(
    QByteArray bytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return write_user_bytes_locked(
        std::move(bytes),
        User_write_viewport_policy::RETURN_TO_TAIL,
        Backend_callback_drain_policy::KEEP_CALLBACKS_QUEUED);
}

std::optional<Terminal_session_result>
Terminal_session::try_write_user_bytes_without_backend_drain_if_callbacks_empty(
    QByteArray bytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_pending_commands.empty() ||
        m_callback_lifetime->has_pending_or_active_callbacks())
    {
        return std::nullopt;
    }

    return write_user_bytes_locked(
        std::move(bytes),
        User_write_viewport_policy::RETURN_TO_TAIL,
        Backend_callback_drain_policy::KEEP_CALLBACKS_QUEUED);
}

Terminal_session_result Terminal_session::write_user_bytes_locked(
    QByteArray                         bytes,
    User_write_viewport_policy         viewport_policy,
    Backend_callback_drain_policy      drain_policy,
    std::uint64_t                      interaction_trace_id)
{
    const std::uint64_t sequence = next_sequence();
    if (!is_session_writable()) {
        return make_rejected_result(
            sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session write requires a running backend")));
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes), interaction_trace_id),
        drain_policy);
    return finalize_accepted_text_input_result(
        result,
        sequence,
        viewport_policy);
}

Terminal_key_event_result Terminal_session::write_key_event(
    const QKeyEvent& event,
    std::uint64_t    interaction_trace_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    return write_key_event_locked(
        event,
        Backend_callback_drain_policy::DRAIN_CALLBACKS,
        interaction_trace_id);
}

Terminal_key_event_result Terminal_session::write_key_event_locked(
    const QKeyEvent&                   event,
    Backend_callback_drain_policy      drain_policy,
    std::uint64_t                      interaction_trace_id)
{
    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_key_event(event, modes);
    if (bytes.isEmpty()) {
        return {};
    }

    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (interaction_trace_enabled() && interaction_trace_id == 0U) {
        interaction_trace_id = next_interaction_trace_correlation_id();
    }
    if (interaction_trace_enabled()) {
        record_interaction_trace(
            "session",
            "key-encoded",
            interaction_trace_key_summary(event) + QLatin1Char(' ') +
                interaction_trace_byte_summary(bytes),
            interaction_trace_id);
    }
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes), interaction_trace_id),
        drain_policy);
    return {
        true,
        finalize_accepted_text_input_result(
            result,
            sequence,
            User_write_viewport_policy::RETURN_TO_TAIL),
    };
}

std::optional<Terminal_mouse_event_result>
Terminal_session::try_write_mouse_event_without_backend_drain_if_callbacks_empty(
    Terminal_mouse_event event)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_pending_commands.empty() ||
        m_callback_lifetime->has_pending_or_active_callbacks())
    {
        return std::nullopt;
    }

    return write_mouse_event_locked(
        event,
        Backend_callback_drain_policy::KEEP_CALLBACKS_QUEUED);
}

Terminal_mouse_event_result Terminal_session::write_mouse_event_locked(
    Terminal_mouse_event               event,
    Backend_callback_drain_policy      drain_policy)
{
    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_mouse_event(event, modes);
    if (bytes.isEmpty()) {
        return {};
    }

    if (!is_session_writable()) {
        return {};
    }

    const User_write_viewport_policy viewport_policy =
        event.kind == Terminal_mouse_event_kind::MOVE
            ? User_write_viewport_policy::PRESERVE_VIEWPORT
            : User_write_viewport_policy::RETURN_TO_TAIL;
    return {
        true,
        write_user_bytes_locked(
            std::move(bytes),
            viewport_policy,
            drain_policy),
    };
}

Terminal_ime_commit_result Terminal_session::write_ime_commit(
    QString       text,
    std::uint64_t interaction_trace_id)
{
    if (text.isEmpty()) {
        return {};
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    QByteArray bytes = text.toUtf8();
    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (interaction_trace_enabled() && interaction_trace_id == 0U) {
        interaction_trace_id = next_interaction_trace_correlation_id();
    }
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes), interaction_trace_id));

    Terminal_session_result final_result =
        finalize_accepted_text_input_result(
            result,
            sequence,
            User_write_viewport_policy::RETURN_TO_TAIL);

    if (final_result.code == Terminal_session_result_code::ACCEPTED &&
        ime_preedit_has_content(m_ime_preedit))
    {
        m_ime_preedit = {};
        advance_ime_preedit_generation();
    }

    return {true, final_result};
}

Terminal_paste_text_result Terminal_session::write_paste_text(
    QString                        text,
    Terminal_paste_framing_policy  policy,
    std::uint64_t                  interaction_trace_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_paste_text(std::move(text), modes, policy);
    if (bytes.isEmpty()) {
        return {};
    }

    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (interaction_trace_enabled() && interaction_trace_id == 0U) {
        interaction_trace_id = next_interaction_trace_correlation_id();
    }
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_paste_command(sequence, std::move(bytes), interaction_trace_id));
    return {
        true,
        finalize_accepted_text_input_result(
            result,
            sequence,
            User_write_viewport_policy::RETURN_TO_TAIL),
    };
}

Terminal_paste_text_result Terminal_session::write_submitted_text(
    QString                        text,
    Terminal_paste_framing_policy  policy,
    std::uint64_t                  interaction_trace_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_paste_text(std::move(text), modes, policy);
    if (bytes.isEmpty()) {
        return {};
    }
    bytes.append('\r');
    if (bytes.size() > vnm_terminal::k_terminal_message_utf8_hard_limit_bytes) {
        const std::uint64_t sequence = next_sequence();
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_ARGUMENT,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral(
                        "encoded message exceeds terminal message hard limit"))),
        };
    }

    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        const std::uint64_t sequence = next_sequence();
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral(
                        "message submission requires a running backend"))),
        };
    }

    const std::uint64_t sequence = next_sequence();
    if (interaction_trace_enabled() && interaction_trace_id == 0U) {
        interaction_trace_id = next_interaction_trace_correlation_id();
    }
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral(
                        "message submission requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_message_command(sequence, std::move(bytes), interaction_trace_id));
    return {
        true,
        finalize_accepted_text_input_result(
            result,
            sequence,
            User_write_viewport_policy::RETURN_TO_TAIL),
    };
}

Terminal_focus_event_result Terminal_session::write_focus_event(bool focused)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() || !m_screen_model->mode_state().focus_reporting) {
        return {};
    }

    // Focus reports are terminal-mode side effects. After exit, drop them silently
    // instead of surfacing a public write error on ordinary focus transitions.
    if (!is_session_writable()) {
        return {};
    }

    const QByteArrayView report = focused ? k_focus_in_report : k_focus_out_report;
    return {
        true,
        write_user_bytes_locked(
            QByteArray(report.data(), report.size()),
            User_write_viewport_policy::PRESERVE_VIEWPORT),
    };
}

void Terminal_session::set_ime_preedit(QString text, int cursor_position)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Ime_preedit_state state;
    state.text            = std::move(text);
    state.cursor_position = std::clamp(cursor_position, 0, bounded_string_size(state.text));
    state.active          = !state.text.isEmpty();

    if (same_ime_preedit_state(m_ime_preedit, state)) {
        return;
    }

    m_ime_preedit = std::move(state);
    advance_ime_preedit_generation();
}

void Terminal_session::cancel_ime_preedit()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!ime_preedit_has_content(m_ime_preedit)) {
        return;
    }

    m_ime_preedit = {};
    advance_ime_preedit_generation();
}

Terminal_session_result Terminal_session::write_terminal_reply(const Terminal_reply& reply)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const Terminal_session_command command =
        make_terminal_reply_command(next_sequence(), reply);
    if (!is_session_writable()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session write requires a running backend")));
    }

    return enqueue_and_process_synchronous_command(command);
}

Terminal_session_result Terminal_session::resize(
    QSizeF                 source_geometry,
    terminal_grid_size_t   grid_size)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();

    Terminal_resize_transaction resize;
    resize.id                       = next_resize_id();
    resize.source_geometry          = source_geometry;
    resize.target_grid_size         = grid_size;
    resize.snapshot_geometry        = source_geometry;
    resize.snapshot_grid_size       = grid_size;
    resize.backend_geometry_in_sync = m_backend_geometry_in_sync;

    return enqueue_and_process_synchronous_command(make_resize_command(sequence, resize));
}

Terminal_viewport_scroll_result Terminal_session::scroll_viewport_lines(int line_delta)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    return scroll_viewport_lines_locked(line_delta);
}

Terminal_viewport_scroll_result Terminal_session::scroll_viewport_lines_from_published_state(
    int                       line_delta,
    Terminal_viewport_state   published_viewport)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_screen_model.has_value() || line_delta == 0) {
        return {};
    }
    const bool render_publication_blocked =
        !model_allows_render_snapshot(*m_screen_model);
    if (published_viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
        return {};
    }

    if (render_publication_blocked &&
        immediate_public_projection_policy_enabled() &&
        public_projection_hold_active())
    {
        const Terminal_viewport_state public_viewport_before =
            m_public_viewport_controller.viewport();
        if (!viewport_states_match(public_viewport_before, published_viewport)) {
            return {};
        }

        const Terminal_public_viewport_controller public_viewport_controller_before =
            m_public_viewport_controller;
        Terminal_public_viewport_scroll_result public_scroll_result =
            m_public_viewport_controller.scroll_lines(line_delta);
        return finish_public_projection_scroll(
            std::move(public_scroll_result),
            public_viewport_controller_before,
            public_viewport_before,
            QStringLiteral("public projection viewport scrolled"));
    }

    if (!render_publication_blocked) {
        const Terminal_viewport_state viewport_before = m_viewport_controller.state();
        if (!viewport_states_match(viewport_before, published_viewport)) {
            return {};
        }
    }

    Terminal_viewport_scroll_result scroll_result;
    if (render_publication_blocked) {
        const Terminal_viewport_state live_viewport = m_viewport_controller.state();
        if (live_viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
            return {};
        }

        const int bounded_public_max_offset =
            std::max(0, published_viewport.scrollback_rows);
        const long long requested_offset =
            static_cast<long long>(live_viewport.offset_from_tail) +
            static_cast<long long>(line_delta);
        const int target_offset = static_cast<int>(
            std::clamp(
                requested_offset,
                0LL,
                static_cast<long long>(bounded_public_max_offset)));
        const int bounded_line_delta =
            target_offset - live_viewport.offset_from_tail;
        if (bounded_line_delta == 0) {
            return {Terminal_viewport_scroll_action::AT_BOUNDARY, 0};
        }

        scroll_result = m_viewport_controller.scroll_lines(bounded_line_delta);
    }
    else {
        scroll_result = m_viewport_controller.scroll_lines(line_delta);
    }
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

Terminal_viewport_scroll_result Terminal_session::scroll_viewport_lines_locked(int line_delta)
{
    if (!m_screen_model.has_value() || line_delta == 0) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

// Internal wheel/page scrolling may intentionally defer publication while
// synchronized output is active; public chrome needs the stricter variant below.
Terminal_viewport_scroll_result Terminal_session::scroll_published_viewport_lines(
    int line_delta)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() || line_delta == 0) {
        return {};
    }
    const bool render_publication_blocked =
        !model_allows_render_snapshot(*m_screen_model);
    if (render_publication_blocked &&
        immediate_public_projection_policy_enabled() &&
        public_projection_hold_active())
    {
        const Terminal_viewport_state public_viewport_before =
            m_public_viewport_controller.viewport();
        if (public_viewport_before.active_buffer != Terminal_buffer_id::PRIMARY) {
            return {};
        }

        const Terminal_public_viewport_controller public_viewport_controller_before =
            m_public_viewport_controller;
        Terminal_public_viewport_scroll_result public_scroll_result =
            m_public_viewport_controller.scroll_lines(line_delta);
        return finish_public_projection_scroll(
            std::move(public_scroll_result),
            public_viewport_controller_before,
            public_viewport_before,
            QStringLiteral("public projection viewport scrolled"));
    }
    if (render_publication_blocked) {
        return {};
    }
    if (m_viewport_controller.state().active_buffer != Terminal_buffer_id::PRIMARY) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

Terminal_viewport_scroll_result Terminal_session::scroll_published_viewport_to_offset_from_tail(
    int offset_from_tail)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value()) {
        return {};
    }
    const bool render_publication_blocked =
        !model_allows_render_snapshot(*m_screen_model);
    if (render_publication_blocked &&
        immediate_public_projection_policy_enabled() &&
        public_projection_hold_active())
    {
        const Terminal_viewport_state public_viewport_before =
            m_public_viewport_controller.viewport();
        if (public_viewport_before.active_buffer != Terminal_buffer_id::PRIMARY) {
            return {};
        }

        const Terminal_public_viewport_controller public_viewport_controller_before =
            m_public_viewport_controller;
        Terminal_public_viewport_scroll_result public_scroll_result =
            m_public_viewport_controller.scroll_to_offset_from_tail(offset_from_tail);
        return finish_public_projection_scroll(
            std::move(public_scroll_result),
            public_viewport_controller_before,
            public_viewport_before,
            QStringLiteral("public projection viewport scrolled"));
    }
    if (render_publication_blocked) {
        return {};
    }
    if (m_viewport_controller.state().active_buffer != Terminal_buffer_id::PRIMARY) {
        return {};
    }

    const Terminal_viewport_state viewport = m_viewport_controller.state();
    const int target_offset =
        std::clamp(offset_from_tail, 0, std::max(0, viewport.scrollback_rows));
    const int line_delta = target_offset - viewport.offset_from_tail;
    if (line_delta == 0) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

Terminal_viewport_scroll_result Terminal_session::finish_public_projection_scroll(
    Terminal_public_viewport_scroll_result scroll_result,
    Terminal_public_viewport_controller    public_viewport_controller_before,
    Terminal_viewport_state                public_viewport_before,
    QString                                message)
{
    if (scroll_result.invalidated_public_projection) {
        m_public_projection.reset();
    }
    if (scroll_result.viewport_result.action !=
        Terminal_viewport_scroll_action::VIEWPORT_MOVED)
    {
        if (scroll_result.deferred_release_intent_recorded) {
            return {
                Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED,
                0,
            };
        }
        return scroll_result.viewport_result;
    }

    if (!publish_public_projection_scroll_snapshot(
            next_sequence(),
            std::move(message),
            public_viewport_before,
            m_public_viewport_controller.viewport()))
    {
        m_public_viewport_controller = std::move(public_viewport_controller_before);
        invalidate_public_projection(
            Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED,
            Terminal_public_scroll_diagnostic_reason::
                PUBLIC_PROJECTION_SCROLL_PUBLICATION_FAILED);
        return {};
    }

    return scroll_result.viewport_result;
}

void Terminal_session::set_selection_range(Terminal_selection_range range)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    set_selection_range_from_published_source_locked(
        range,
        published_selection_source_identity_unlocked());
}

void Terminal_session::set_selection_range_from_published_source(
    Terminal_selection_range             range,
    terminal_selection_source_identity_t source)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    set_selection_range_from_published_source_locked(range, source);
}

void Terminal_session::set_selection_range_from_drained_published_source(
    Terminal_selection_range             range,
    terminal_selection_source_identity_t source)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    set_selection_range_from_published_source_locked(range, source);
}

void Terminal_session::detach_selection_visual_attachment()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (public_projection_hold_active()) {
        m_public_viewport_controller.record_selection_mutation_unsupported();
        return;
    }

    const bool had_internal_selection = m_selection.has_internal_selection();
    const bool had_visual_lease       = m_selection.visual_lease().has_value();
    if (!had_internal_selection) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral("session visual-detach ignored reason=no-selection"));
        }
        return;
    }
    const bool publication_blocked =
        m_screen_model.has_value() && !model_allows_render_snapshot(*m_screen_model);

    m_selection.detach_visual_attachment();
    reset_synchronized_selection_continuity_hold();
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session visual-detach action=detach had_lease=%1 "
                "publication_blocked=%2 state=%3")
                .arg(selection_trace_bool(had_visual_lease))
                .arg(selection_trace_bool(publication_blocked))
                .arg(static_cast<int>(m_selection.internal_state())));
    }
    if (had_visual_lease || !m_selection.has_internal_selection()) {
        const bool allow_blocked_selection_only_snapshot =
            publication_blocked && had_visual_lease;
        publish_selection_snapshot(
            next_sequence(),
            QStringLiteral("selection visual detached"),
            allow_blocked_selection_only_snapshot);
    }
}

void Terminal_session::clear_selection()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (public_projection_hold_active()) {
        m_public_viewport_controller.record_selection_mutation_unsupported();
        return;
    }

    if (!m_selection.has_internal_selection()) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled, QStringLiteral("session clear-selection reason=no-selection"));
        }
        return;
    }

    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral("session clear-selection range=%1 buffer=%2 basis={%3}")
                .arg(selection_trace_range(m_selection.range()))
                .arg(static_cast<int>(m_selection_buffer_id))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    m_selection.clear();
    reset_synchronized_selection_continuity_hold();
    if (m_screen_model.has_value()) {
        m_selection_buffer_id = m_screen_model->active_buffer_id();
        publish_selection_snapshot(next_sequence(), QStringLiteral("selection cleared"));
    }
}

void Terminal_session::set_scrollback_limit(int limit)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    m_config.scrollback_limit = std::max(0, limit);
    if (!m_screen_model.has_value()) {
        return;
    }

    const std::optional<Live_primary_viewport_anchor> detached_viewport_anchor =
        capture_live_primary_detached_viewport_anchor();
    const Terminal_screen_model_result model_result =
        m_screen_model->set_scrollback_limit(m_config.scrollback_limit);
    const Terminal_viewport_state previous_viewport  = m_viewport_controller.state();
    const terminal_grid_size_t    previous_grid_size = m_grid_size;
    m_render_snapshot_model_result = model_result;
    sync_viewport_from_model_result(model_result, detached_viewport_anchor);

    const bool render_snapshot_available = model_allows_render_snapshot(*m_screen_model);
    if (!render_snapshot_available) {
        accumulate_synchronized_selection_continuity(model_result);
        record_blocked_synchronized_row_origin_change(model_result);
    }
    if ((model_result_warrants_render_snapshot(model_result) || m_visual_bell_active) &&
        render_snapshot_available)
    {
        const Terminal_screen_model_result selection_basis_result =
            model_result_with_deferred_synchronized_row_origins(model_result);
        advance_selection_content_basis_for_model_result(
            selection_basis_result,
            previous_viewport,
            previous_grid_size);
        publish_render_snapshot(next_sequence(), QStringLiteral("scrollback limit changed"));
    }
}

void Terminal_session::set_color_state(Terminal_color_state state)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    // Remember the requested color state so it survives a screen-model
    // (re)creation. At startup this runs before the model exists (the model is
    // created lazily on the first resize), so without remembering it the chosen
    // color scheme would be lost and the model would keep its defaults.
    m_color_state = state;

    if (!m_screen_model.has_value()) {
        return;
    }

    const std::optional<Live_primary_viewport_anchor> detached_viewport_anchor =
        capture_live_primary_detached_viewport_anchor();
    const Terminal_screen_model_result model_result =
        m_screen_model->set_color_state(std::move(state));
    const Terminal_viewport_state previous_viewport  = m_viewport_controller.state();
    const terminal_grid_size_t    previous_grid_size = m_grid_size;
    m_render_snapshot_model_result = model_result;
    sync_viewport_from_model_result(model_result, detached_viewport_anchor);

    const bool render_snapshot_available = model_allows_render_snapshot(*m_screen_model);
    if (!render_snapshot_available) {
        accumulate_synchronized_selection_continuity(model_result);
        record_blocked_synchronized_row_origin_change(model_result);
    }
    if ((model_result_warrants_render_snapshot(model_result) || m_visual_bell_active) &&
        render_snapshot_available)
    {
        const Terminal_screen_model_result selection_basis_result =
            model_result_with_deferred_synchronized_row_origins(model_result);
        advance_selection_content_basis_for_model_result(
            selection_basis_result,
            previous_viewport,
            previous_grid_size);
        publish_render_snapshot(next_sequence(), QStringLiteral("color state changed"));
    }
}

void Terminal_session::set_text_area_resize_policy(
    Terminal_text_area_resize_policy policy)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_config.text_area_resize_policy == policy) {
        return;
    }

    // Assigned before the drain, unlike the settings either side of this one.
    // The policy answers "can the host move its window right now", so bytes
    // that arrived before the host lost its geometry but have not been parsed
    // yet must still be parsed under the new policy. Draining first would honor
    // a queued request against a window that can no longer move.
    m_config.text_area_resize_policy = policy;
    if (m_screen_model.has_value()) {
        m_screen_model->set_text_area_resize_policy(policy);
    }

    drain_backend_callback_commands();
    process_pending_commands();
}

void Terminal_session::set_primary_repaint_recovery_enabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (m_config.recover_scrollback_from_primary_repaints == enabled) {
        return;
    }

    m_config.recover_scrollback_from_primary_repaints = enabled;
    if (m_screen_model.has_value()) {
        m_screen_model->set_primary_repaint_recovery_enabled(enabled);
    }
}

Terminal_session_result Terminal_session::interrupt()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(make_interrupt_command(sequence));
}

Terminal_session_result Terminal_session::terminate()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(make_terminate_command(sequence));
}

Terminal_session_result Terminal_session::force_release_synchronized_output()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(
        make_force_release_synchronized_output_command(sequence));
}

Terminal_session_result
Terminal_session::force_release_synchronized_output_without_backend_drain()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const std::uint64_t sequence =
        m_last_processed_sequence == 0U
            ? next_sequence()
            : m_last_processed_sequence;
    Terminal_session_result result = force_release_synchronized_output_locked(sequence);
    record_result(result);
    return result;
}

Terminal_process_state Terminal_session::process_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_process_state;
}

bool Terminal_session::backend_ready() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_backend_ready;
}

bool Terminal_session::backend_geometry_in_sync() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_backend_geometry_in_sync;
}

bool Terminal_session::output_backpressure_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_output_backpressure_active;
}

bool Terminal_session::render_publication_blocked() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value() && !model_allows_render_snapshot(*m_screen_model);
}

bool Terminal_session::has_pending_backend_callback_events() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return !m_pending_commands.empty() ||
        m_callback_lifetime->has_pending_or_active_callbacks();
}

std::size_t Terminal_session::pending_backend_callback_event_count() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::size_t count = 0U;
    for (const Terminal_session_command& command : m_pending_commands) {
        if (command.backend_callback_epoch != 0U) {
            ++count;
        }
    }
    return count + m_callback_lifetime->pending_command_count();
}

std::uint64_t Terminal_session::backend_callback_enqueue_epoch() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_callback_lifetime->last_enqueued_backend_callback_epoch();
}

std::uint64_t Terminal_session::backend_callback_processed_epoch() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_last_processed_backend_callback_epoch;
}

bool Terminal_session::mouse_reporting_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_screen_model.has_value()) {
        return false;
    }

    const Terminal_input_mode_state modes = m_screen_model->input_mode_state();
    return
        modes.sgr_mouse_encoding                                         &&
        modes.mouse_tracking != Terminal_input_mouse_tracking_mode::NONE &&
        modes.mouse_tracking != Terminal_input_mouse_tracking_mode::X10;
}

bool Terminal_session::alternate_scroll_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return
        m_screen_model.has_value() &&
        m_screen_model->mode_state().alternate_scroll;
}

std::uint64_t Terminal_session::alternate_scroll_mode_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_alternate_scroll_mode_generation;
}

Terminal_viewport_state Terminal_session::viewport_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_viewport_controller.state();
}

terminal_grid_size_t Terminal_session::grid_size() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_grid_size;
}

std::uint64_t Terminal_session::last_processed_sequence() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_last_processed_sequence;
}

bool Terminal_session::has_selection() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_selection.has_selection();
}

Terminal_selection_result Terminal_session::selected_text() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_selection.has_selection()) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled, QStringLiteral("session selected-text result=no-selection"));
        }
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    if (!m_screen_model.has_value()) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral("session selected-text result=invalid-range reason=no-screen-model range=%1")
                    .arg(selection_trace_range(m_selection.range())));
        }
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const Terminal_selection_result result = m_selection.selected_text();
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral("session selected-text result=%1 range=%2 size=%3")
                .arg(static_cast<int>(result.code))
                .arg(selection_trace_range(m_selection.range()))
                .arg(result.text.size()));
    }
    return result;
}

std::optional<terminal_selection_visual_lease_t> Terminal_session::selection_visual_lease() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_selection.visual_lease();
}

std::optional<Terminal_selection_attachment_resolution_status>
Terminal_session::last_selection_attachment_resolution_status() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_last_selection_attachment_resolution_status;
}

Terminal_selection_anchor_domain Terminal_session::selection_anchor_domain() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_selection.anchor_domain();
}

std::optional<terminal_selection_source_identity_t>
Terminal_session::published_selection_source_identity() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return published_selection_source_identity_unlocked();
}

std::optional<terminal_selection_drag_press_provenance_t>
Terminal_session::begin_selection_drag_provenance(
    terminal_grid_position_t             original_position,
    terminal_selection_source_identity_t source)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_selection_drag_continuity.reset();
    if (!m_screen_model.has_value()) {
        return std::nullopt;
    }

    const std::optional<terminal_selection_source_identity_t> current_source =
        published_selection_source_identity_unlocked();
    if (!current_source.has_value() ||
        !selection_source_identities_match(*current_source, source))
    {
        return std::nullopt;
    }

    const std::optional<terminal_history_handle_t> handle =
        m_screen_model->retained_history_handle_at_logical_row(
            source.buffer_id,
            original_position.row);
    if (!handle.has_value()) {
        return std::nullopt;
    }

    terminal_selection_visual_lease_t anchor_lease =
        make_selection_visual_lease(
            {original_position, original_position, Terminal_selection_mode::NORMAL},
            source);
    anchor_lease.selected_lines.push_back({0, *handle});
    const Terminal_selection_attachment_resolution proof =
        m_screen_model->resolve_selection_attachment(
            anchor_lease,
            source,
            Terminal_screen_model_result{});
    if (proof.status != Terminal_selection_attachment_resolution_status::TRANSLATED ||
        !proof.translated_lease.has_value())
    {
        return std::nullopt;
    }

    terminal_selection_drag_press_provenance_t provenance;
    provenance.original_position    = original_position;
    provenance.retained_line_handle = *handle;
    provenance.source               = source;

    Selection_drag_continuity continuity;
    continuity.provenance   = provenance;
    continuity.anchor_lease = *proof.translated_lease;
    m_selection_drag_continuity = std::move(continuity);
    return provenance;
}

Terminal_selection_drag_resolution
Terminal_session::resolve_selection_drag_provenance(
    const terminal_selection_drag_press_provenance_t& provenance,
    bool                                               require_established_selection)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Terminal_selection_drag_resolution result;
    if (!m_screen_model.has_value() ||
        !m_selection_drag_continuity.has_value() ||
        m_selection_drag_continuity->provenance.original_position !=
            provenance.original_position ||
        m_selection_drag_continuity->provenance.retained_line_handle !=
            provenance.retained_line_handle ||
        !selection_source_identities_match(
            m_selection_drag_continuity->provenance.source,
            provenance.source))
    {
        return result;
    }

    Selection_drag_continuity& continuity = *m_selection_drag_continuity;
    if (continuity.failure.has_value()) {
        result.status = *continuity.failure;
        return result;
    }

    const std::optional<terminal_selection_source_identity_t> target_source =
        published_selection_source_identity_unlocked();
    if (!target_source.has_value()) {
        result.status = Terminal_selection_attachment_resolution_status::SOURCE_MISMATCH;
        return result;
    }

    if (continuity.cached_success.has_value()) {
        const Selection_drag_continuity::Cached_success& success =
            *continuity.cached_success;
        const std::optional<terminal_selection_visual_lease_t>& selection_lease =
            m_selection.visual_lease();
        const bool cache_has_compatible_selection = continuity.established
            ? success.proven_selection_lease.has_value() &&
                m_selection.internal_state() == Terminal_selection_internal_state::ATTACHED &&
                selection_lease.has_value() &&
                continuity.durable_payload_identity ==
                    success.proven_selection_lease->durable_payload_identity &&
                continuity.provisional_payload_identity ==
                    success.proven_selection_lease->provisional_payload_identity &&
                m_selection.range() == success.proven_selection_lease->selected_range &&
                selection_visual_leases_match(
                    *selection_lease,
                    *success.proven_selection_lease) &&
                selection_lease_has_compatible_visual_source(
                    *selection_lease,
                    *target_source,
                    true)
            : !success.proven_selection_lease.has_value();
        const bool cached_success_matches =
            selection_source_identities_match(success.source, *target_source) &&
            selection_visual_leases_match(success.anchor_lease, continuity.anchor_lease) &&
            cache_has_compatible_selection;
        if (cached_success_matches &&
            (!require_established_selection || continuity.established))
        {
            result.status            = Terminal_selection_attachment_resolution_status::TRANSLATED;
            result.resolved_position = success.anchor_lease.anchor;
            result.source            = success.source;
            if (require_established_selection) {
                result.reconciled_proven_range =
                    success.proven_selection_lease->selected_range;
            }
            return result;
        }
        continuity.cached_success.reset();
    }

    const Terminal_selection_attachment_resolution anchor_resolution =
        m_screen_model->resolve_selection_attachment(
            continuity.anchor_lease,
            *target_source,
            Terminal_screen_model_result{});
    result.status = anchor_resolution.status;
    if (anchor_resolution.status !=
            Terminal_selection_attachment_resolution_status::TRANSLATED ||
        !anchor_resolution.translated_lease.has_value())
    {
        continuity.failure = anchor_resolution.status;
        continuity.cached_success.reset();
        return result;
    }
    continuity.anchor_lease = *anchor_resolution.translated_lease;

    if (require_established_selection) {
        const std::optional<terminal_selection_visual_lease_t>& lease =
            m_selection.visual_lease();
        if (!continuity.established ||
            m_selection.internal_state() != Terminal_selection_internal_state::ATTACHED ||
            !lease.has_value() ||
            lease->durable_payload_identity != continuity.durable_payload_identity ||
            lease->provisional_payload_identity != continuity.provisional_payload_identity ||
            !selection_lease_has_compatible_visual_source(
                *lease,
                *target_source,
                true))
        {
            result.status = m_last_selection_attachment_resolution_status.value_or(
                Terminal_selection_attachment_resolution_status::INVALID_LEASE);
            continuity.failure = result.status;
            continuity.cached_success.reset();
            return result;
        }
        result.reconciled_proven_range = lease->selected_range;
    }

    result.resolved_position = continuity.anchor_lease.anchor;
    result.source            = *target_source;
    return result;
}

void Terminal_session::record_selection_drag_proven_range()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_selection_drag_continuity.has_value()) {
        return;
    }

    const std::optional<terminal_selection_visual_lease_t>& lease =
        m_selection.visual_lease();
    if (m_selection.internal_state() != Terminal_selection_internal_state::ATTACHED ||
        !lease.has_value())
    {
        m_selection_drag_continuity->failure =
            Terminal_selection_attachment_resolution_status::INVALID_LEASE;
        m_selection_drag_continuity->cached_success.reset();
        return;
    }

    m_selection_drag_continuity->durable_payload_identity =
        lease->durable_payload_identity;
    m_selection_drag_continuity->provisional_payload_identity =
        lease->provisional_payload_identity;
    m_selection_drag_continuity->established = true;
    const std::optional<terminal_selection_source_identity_t> target_source =
        published_selection_source_identity_unlocked();
    if (target_source.has_value()) {
        refresh_selection_drag_cached_success_range(*target_source);
    }
    else {
        m_selection_drag_continuity->cached_success.reset();
    }
}

void Terminal_session::clear_selection_drag_provenance()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_selection_drag_continuity.reset();
}

std::optional<terminal_selection_source_identity_t>
Terminal_session::published_selection_source_identity_unlocked() const
{
    if (m_latest_render_snapshot == nullptr) {
        return std::nullopt;
    }

    terminal_selection_source_identity_t source;
    source.source_content_basis = m_selection_content_basis;
    source.anchor_domain        =
        selection_anchor_domain_for_buffer(m_latest_render_snapshot->viewport.active_buffer);
    source.session_epoch        = m_selection_session_epoch;
    source.buffer_id            = m_latest_render_snapshot->viewport.active_buffer;
    source.grid_reflow_basis    = m_selection_content_basis.grid_reflow_generation;
    source.row_origin_generation = m_latest_render_snapshot->metadata.row_origin_generation;
    source.grid_size            = m_latest_render_snapshot->grid_size;
    source.viewport_mapping     = m_latest_render_snapshot->viewport;
    return source;
}

void Terminal_session::set_search_query(QString query)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (query.isEmpty()) {
        clear_search();
        return;
    }

    if (query != m_search.query()) {
        m_search.set_source_unavailable(std::move(query));
    }
    if (!refresh_search_from_current_public_source()) {
        (void)publish_search_derived_snapshot(
            QStringLiteral("terminal search source unavailable"));
        return;
    }

    if (m_search.current_match() != nullptr) {
        (void)reveal_current_search_match(QStringLiteral("terminal search query changed"));
    }
    else {
        (void)publish_search_derived_snapshot(
            QStringLiteral("terminal search query changed"));
    }
}

void Terminal_session::clear_search()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_search.query().isEmpty()) {
        return;
    }

    m_search.clear();
    m_search_projection.reset();
    (void)publish_search_derived_snapshot(QStringLiteral("terminal search cleared"));
}

bool Terminal_session::search_next()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_search.select_next()) {
        return false;
    }
    return reveal_current_search_match(QStringLiteral("terminal search next match"));
}

bool Terminal_session::search_previous()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_search.select_previous()) {
        return false;
    }
    return reveal_current_search_match(QStringLiteral("terminal search previous match"));
}

QString Terminal_session::search_query() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_search.query();
}

terminal_search_result_state_t Terminal_session::search_result_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_search.result_state();
}

std::vector<Terminal_session_command> Terminal_session::processed_commands() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_processed_commands;
}

std::vector<Terminal_session_notification> Terminal_session::notifications() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_notifications;
}

std::vector<Terminal_session_notification> Terminal_session::take_pending_notifications()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::vector<Terminal_session_notification> notifications;
    notifications.swap(m_pending_notifications);
    return notifications;
}

std::vector<Terminal_resize_transaction> Terminal_session::resize_transactions() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_resize_transactions;
}

std::vector<QByteArray> Terminal_session::output_chunks() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_output_chunks;
}

std::optional<Terminal_render_snapshot> Terminal_session::latest_render_snapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_latest_render_snapshot == nullptr) {
        return std::nullopt;
    }

    return *m_latest_render_snapshot;
}

std::shared_ptr<const Terminal_render_snapshot>
Terminal_session::latest_render_snapshot_handle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_latest_render_snapshot;
}

std::optional<Terminal_render_snapshot>
Terminal_session::latest_content_render_snapshot_for_testing() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_latest_content_render_snapshot == nullptr) {
        return std::nullopt;
    }

    return *m_latest_content_render_snapshot;
}

std::optional<Terminal_public_projection>
Terminal_session::capture_public_projection_for_testing()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_screen_model.has_value()                   ||
        m_latest_content_render_snapshot == nullptr   ||
        !model_allows_render_snapshot(*m_screen_model))
    {
        return std::nullopt;
    }

    m_public_projection =
        Terminal_public_projection::capture_from_safe_model(
            m_next_public_projection_generation,
            *m_latest_content_render_snapshot,
            m_latest_content_render_snapshot_content_basis,
            m_active_buffer_epoch);
    m_public_viewport_controller.initialize_from_projection(*m_public_projection);
    ++m_next_public_projection_generation;
    if (m_next_public_projection_generation == 0U) {
        m_next_public_projection_generation = 1U;
    }

    return m_public_projection;
}

std::optional<Terminal_public_projection>
Terminal_session::public_projection_for_testing() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_public_projection;
}

void Terminal_session::install_public_projection_for_testing(
    Terminal_public_projection projection)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_public_projection = std::move(projection);
    m_public_viewport_controller.initialize_from_projection(*m_public_projection);
}

std::optional<Terminal_viewport_state> Terminal_session::public_viewport_for_testing() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_public_viewport_controller.has_public_viewport()) {
        return std::nullopt;
    }

    return m_public_viewport_controller.viewport();
}

std::optional<Terminal_public_release_intent>
Terminal_session::public_release_intent_for_testing() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_public_viewport_controller.has_public_viewport()) {
        return std::nullopt;
    }

    return m_public_viewport_controller.release_intent();
}

Terminal_public_viewport_scroll_result
Terminal_session::scroll_public_projection_viewport_lines_for_testing(int line_delta)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Terminal_public_viewport_scroll_result result =
        m_public_viewport_controller.scroll_lines(line_delta);
    if (result.invalidated_public_projection) {
        m_public_projection.reset();
    }
    return result;
}

Terminal_public_viewport_scroll_result
Terminal_session::scroll_public_projection_viewport_to_offset_from_tail_for_testing(
    int offset_from_tail)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Terminal_public_viewport_scroll_result result =
        m_public_viewport_controller.scroll_to_offset_from_tail(offset_from_tail);
    if (result.invalidated_public_projection) {
        m_public_projection.reset();
    }
    return result;
}

Terminal_public_viewport_scroll_result
Terminal_session::scroll_public_projection_viewport_to_tail_for_testing()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Terminal_public_viewport_scroll_result result =
        m_public_viewport_controller.scroll_to_tail();
    if (result.invalidated_public_projection) {
        m_public_projection.reset();
    }
    return result;
}

void Terminal_session::invalidate_public_projection_for_testing(
    Terminal_public_projection_disable_reason reason)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    invalidate_public_projection(reason);
}

void Terminal_session::set_bell_policy(Terminal_bell_policy policy)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_config.bell_policy          = policy;
    m_bell_state.policy           = policy;
    m_bell_state.window_start_ms  = 0U;
    m_bell_state.events_in_window = 0U;
}

void Terminal_session::set_synchronized_output_scroll_policy(
    Terminal_synchronized_output_scroll_policy policy)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_config.synchronized_output_scroll_policy == policy) {
        return;
    }

    if (m_synchronized_output_hold_policy.has_value() &&
        *m_synchronized_output_hold_policy != policy)
    {
        m_synchronized_output_policy_change_event =
            Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD;
    }
    m_config.synchronized_output_scroll_policy = policy;
}

void Terminal_session::set_synchronized_output_scroll_policy_for_testing(
    Terminal_synchronized_output_scroll_policy policy)
{
    set_synchronized_output_scroll_policy(policy);
}

std::uint64_t Terminal_session::render_snapshot_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_render_snapshot_generation;
}

std::uint64_t Terminal_session::installed_render_snapshot_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_render_snapshot_installed_generation;
}

std::uint64_t Terminal_session::rendered_render_snapshot_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_render_snapshot_rendered_generation;
}

void Terminal_session::mark_render_snapshot_installed(std::uint64_t generation)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (generation <= m_render_snapshot_generation) {
        const std::uint64_t previous_installed_generation =
            m_render_snapshot_installed_generation;
        m_render_snapshot_installed_generation =
            std::max(m_render_snapshot_installed_generation, generation);
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            if (m_render_snapshot_installed_generation > previous_installed_generation) {
                ++m_profile_stats.snapshots_consumed_by_bridge;
            }
        }
#endif
    }
}

void Terminal_session::mark_render_publication_rendered(std::uint64_t generation)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (generation > m_render_snapshot_generation) {
        return;
    }

    const std::uint64_t previous_rendered_generation =
        m_render_snapshot_rendered_generation;
    m_render_snapshot_rendered_generation =
        std::max(m_render_snapshot_rendered_generation, generation);
    if (m_render_snapshot_rendered_generation >= m_render_snapshot_generation) {
        m_unrendered_render_snapshot_dirty_basis.reset();
    }
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled &&
        m_render_snapshot_rendered_generation > previous_rendered_generation)
    {
        ++m_profile_stats.snapshots_marked_rendered;
    }
#endif
}

Ime_preedit_state Terminal_session::ime_preedit_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_ime_preedit;
}

std::uint64_t Terminal_session::ime_preedit_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_ime_preedit_generation;
}

std::optional<Terminal_screen_model_result> Terminal_session::last_model_ingest_result() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_last_model_ingest_result;
}

void Terminal_session::set_dirty_row_stats_enabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_config.capture_dirty_row_stats = enabled;
#if VNM_TERMINAL_PROFILING_ENABLED
    m_profile_stats = {};
    m_profile_stats.enabled = enabled;
    if (enabled) {
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#endif
    if (m_screen_model.has_value()) {
        m_screen_model->set_dirty_row_stats_enabled(enabled);
        m_screen_model->set_profile_stats_enabled(enabled);
    }
}

Terminal_screen_model_dirty_row_stats Terminal_session::dirty_row_stats() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->dirty_row_stats()
        : Terminal_screen_model_dirty_row_stats{};
}

Terminal_screen_model_dirty_row_timeline Terminal_session::dirty_row_timeline() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->dirty_row_timeline()
        : Terminal_screen_model_dirty_row_timeline{};
}

void Terminal_session::set_profile_stats_enabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

#if VNM_TERMINAL_PROFILING_ENABLED
    m_profile_stats = {};
    m_profile_stats.enabled = enabled;
    if (m_screen_model.has_value()) {
        m_screen_model->set_profile_stats_enabled(enabled);
    }
    if (enabled) {
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#else
    Q_UNUSED(enabled);
#endif
}

Terminal_screen_model_profile_stats Terminal_session::model_profile_stats() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->profile_stats()
        : Terminal_screen_model_profile_stats{};
}

terminal_retained_history_diagnostics_t Terminal_session::retained_history_diagnostics() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->retained_history_diagnostics()
        : terminal_retained_history_diagnostics_t{};
}

void Terminal_session::set_selection_reconciliation_counters_enabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_screen_model.has_value()) {
        m_screen_model->set_selection_reconciliation_counters_enabled(enabled);
    }
}

void Terminal_session::reset_selection_reconciliation_counters()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_screen_model.has_value()) {
        m_screen_model->reset_selection_reconciliation_counters();
    }
}

terminal_selection_reconciliation_counters_t
Terminal_session::selection_reconciliation_counters() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_screen_model.has_value()
        ? m_screen_model->selection_reconciliation_counters()
        : terminal_selection_reconciliation_counters_t{};
}

Terminal_session_profile_stats Terminal_session::profile_stats() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_profile_stats;
}

std::optional<Terminal_backend_exit> Terminal_session::exit_status() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_exit_status;
}

Terminal_session_result Terminal_session::enqueue_and_process_synchronous_command(
    Terminal_session_command           command,
    Backend_callback_drain_policy      drain_policy)
{
    const std::uint64_t sequence = command.sequence;
    const Terminal_session_result enqueue_result = enqueue_command(std::move(command));
    if (enqueue_result.code != Terminal_session_result_code::ACCEPTED) {
        return enqueue_result;
    }

    begin_result_capture(sequence);
    process_pending_commands(drain_policy);
    const Terminal_session_result result = result_after_processing(sequence, enqueue_result);
    end_result_capture();
    return result;
}

Terminal_session_result Terminal_session::enqueue_command(Terminal_session_command command)
{
    const Queue_category category   = queue_category_for(command.kind);
    const std::size_t    byte_count = static_cast<std::size_t>(command.bytes.size());
    if (category == Queue_category::OUTPUT &&
        should_ignore_backend_output_after_stop(command.sequence))
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output ignored after terminal stop request")));
    }

    const Terminal_queue_result queue_result =
        would_accept_command(category, byte_count, 1U);

    if (queue_result.code == Terminal_queue_result_code::HARD_LIMIT_REACHED) {
        Terminal_backend_error error;
        Terminal_session_result_code code = Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED;
        if (category == Queue_category::OUTPUT) {
            return handle_output_overflow(
                command.sequence,
                QStringLiteral("backend output queue hard limit reached"));
        }
        else
        if (command.kind == Terminal_session_command_kind::USER_PASTE) {
            error = make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("paste text exceeds session write queue hard limit"));
        }
        else
        if (command.kind == Terminal_session_command_kind::USER_MESSAGE) {
            error = make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("message exceeds session write queue hard limit"));
        }
        else {
            error = make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session command queue hard limit reached"));
        }

        return make_rejected_result(command.sequence, code, std::move(error));
    }

    add_to_queue_state(category, byte_count);
    const std::uint64_t sequence = command.sequence;
    m_pending_commands.push_back(std::move(command));

    if (category == Queue_category::OUTPUT) {
        set_output_backpressure_active(queue_result.high_water_reached, sequence);
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        sequence,
        queue_result.high_water_reached,
        std::nullopt,
    };
}

Backend_callback_drain_stop Terminal_session::process_pending_commands(
    Backend_callback_drain_policy          drain_policy,
    Backend_callback_drain_deadline        deadline,
    std::optional<std::uint64_t>           target_backend_callback_epoch)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::process_pending_commands");

    if (m_processing_commands) {
        return Backend_callback_drain_stop::UNSETTLED;
    }

    m_processing_commands = true;
    m_drain_synchronized_release_publication_generation.reset();
    m_drain_latest_live_content_publication_generation.reset();
    const bool previous_backend_content_snapshot_deferral =
        m_backend_content_snapshot_deferral_active;
    m_backend_content_snapshot_deferral_active = deadline.has_value();

    Backend_callback_drain_stop stop = Backend_callback_drain_stop::COMPLETE;
    for (;;) {
        if (drain_policy == Backend_callback_drain_policy::DRAIN_CALLBACKS) {
            drain_backend_callback_commands();
        }
        if (m_pending_commands.empty()) {
            break;
        }

        Terminal_session_command command = std::move(m_pending_commands.front());
        m_pending_commands.pop_front();
        const std::uint64_t command_backend_callback_epoch =
            command.backend_callback_epoch;

        const bool continuing_budgeted_output =
            command.kind     == Terminal_session_command_kind::BACKEND_OUTPUT &&
            command.sequence == m_budgeted_backend_output_sequence;
        if (!continuing_budgeted_output) {
            record_processed_command(command);
        }

        const bool slice_backend_output =
            deadline.has_value()                                          &&
            command.kind == Terminal_session_command_kind::BACKEND_OUTPUT &&
            command.bytes.size() > k_backend_output_drain_slice_bytes     &&
            m_screen_model.has_value()                                    &&
            !should_ignore_backend_output_after_stop(command.sequence);
        if (slice_backend_output) {
            // A sliced BACKEND_OUTPUT remains one logical queued command. Bytes
            // are released per slice, but command-count/backpressure accounting
            // stays held until the final continuation completes.
            Q_ASSERT(m_result_capture_sequence == 0U);
            Terminal_session_command remainder = command;
            remainder.bytes = command.bytes.sliced(k_backend_output_drain_slice_bytes);
            command.bytes.truncate(k_backend_output_drain_slice_bytes);
            m_pending_commands.push_front(std::move(remainder));
            m_budgeted_backend_output_sequence = command.sequence;
        }
        else
        if (continuing_budgeted_output) {
            m_budgeted_backend_output_sequence = 0U;
        }

        const Queue_category category      = queue_category_for(command.kind);
        const std::size_t    byte_count    = static_cast<std::size_t>(command.bytes.size());
        const std::size_t    command_count = slice_backend_output ? 0U : 1U;
        const bool completes_backend_callback =
            command_backend_callback_epoch != 0U && !slice_backend_output;
        const Terminal_session_command_kind command_kind = command.kind;
        m_last_processed_sequence = command.sequence;

        if (command.kind != Terminal_session_command_kind::BACKEND_OUTPUT ||
            should_ignore_backend_output_after_stop(command.sequence))
        {
            flush_deferred_backend_content_snapshot();
        }

        m_backend_error_queued_during_command = false;
        m_processing_backend_callback_epoch =
            completes_backend_callback ? command_backend_callback_epoch : 0U;
        Terminal_session_result result = process_command(std::move(command));
        m_processing_backend_callback_epoch = 0U;
        if (!slice_backend_output) {
            record_result(std::move(result));
        }
        remove_from_queue_state(category, byte_count, command_count);
        if (category == Queue_category::OUTPUT) {
            set_output_backpressure_active(
                queue_high_water_reached(category),
                m_last_processed_sequence);
        }
        if (completes_backend_callback &&
            command_kind != Terminal_session_command_kind::BACKEND_OUTPUT)
        {
            advance_processed_backend_callback_epoch(command_backend_callback_epoch);
        }

        bool synchronized_output_active = false;
        if (m_screen_model.has_value()) {
            synchronized_output_active =
                m_screen_model->mode_state().synchronized_output;
        }

        if (target_backend_callback_epoch.has_value() &&
            m_last_processed_backend_callback_epoch >= *target_backend_callback_epoch)
        {
            break;
        }

        if (!backend_callback_drain_deadline_reached(deadline)) {
            continue;
        }
        const bool drain_work_remaining =
            !m_pending_commands.empty() ||
            (drain_policy == Backend_callback_drain_policy::DRAIN_CALLBACKS &&
                m_callback_lifetime->has_pending_or_active_callbacks());
        if (!drain_work_remaining) {
            continue;
        }

        // The primary budget expired at a command/slice boundary with work
        // remaining. A synchronized-output release that remains the latest
        // live-content publication is an explicit whole-frame boundary. An
        // active hold is HELD only while the installed publication is unchanged.
        const bool deferred_content_publication_pending =
            m_deferred_backend_content_snapshot.has_value();
        const bool release_is_latest_publication =
            m_drain_synchronized_release_publication_generation.has_value() &&
            (!m_drain_latest_live_content_publication_generation.has_value() ||
                *m_drain_latest_live_content_publication_generation <=
                    *m_drain_synchronized_release_publication_generation) &&
            !deferred_content_publication_pending;
        if (release_is_latest_publication) {
            stop = Backend_callback_drain_stop::SYNCHRONIZED_OUTPUT_RELEASED;
            break;
        }
        if (synchronized_output_active) {
            const bool hold_publication_unchanged =
                !m_drain_latest_live_content_publication_generation.has_value() &&
                !m_drain_synchronized_release_publication_generation.has_value() &&
                !deferred_content_publication_pending;
            stop = hold_publication_unchanged
                ? Backend_callback_drain_stop::HELD
                : Backend_callback_drain_stop::UNSETTLED;
            break;
        }
        stop = Backend_callback_drain_stop::UNSETTLED;
        break;
    }
    flush_deferred_backend_content_snapshot();
    m_backend_content_snapshot_deferral_active =
        previous_backend_content_snapshot_deferral;
    m_processing_commands = false;
    // Applies only to the empty-queue exit (callbacks still in flight with the
    // target epoch unreached); an explicit loop-tail stop kind is never
    // overwritten.
    if (stop == Backend_callback_drain_stop::COMPLETE     &&
        target_backend_callback_epoch.has_value()         &&
        m_last_processed_backend_callback_epoch < *target_backend_callback_epoch)
    {
        stop = Backend_callback_drain_stop::UNSETTLED;
    }
    if (stop == Backend_callback_drain_stop::COMPLETE &&
        drain_policy == Backend_callback_drain_policy::DRAIN_CALLBACKS &&
        m_callback_lifetime->has_pending_or_active_callbacks())
    {
        stop = Backend_callback_drain_stop::UNSETTLED;
    }
    m_drain_synchronized_release_publication_generation.reset();
    m_drain_latest_live_content_publication_generation.reset();
    return stop;
}

Terminal_session_result Terminal_session::process_command(Terminal_session_command command)
{
    switch (command.kind) {
        case Terminal_session_command_kind::START:
            return process_start_command(command);
        case Terminal_session_command_kind::USER_WRITE:
        case Terminal_session_command_kind::USER_PASTE:
        case Terminal_session_command_kind::USER_MESSAGE:
        case Terminal_session_command_kind::TERMINAL_REPLY:
            return process_write_command(command);
        case Terminal_session_command_kind::RESIZE:
            return process_resize_command(std::move(command));
        case Terminal_session_command_kind::INTERRUPT:
            return process_interrupt_command(command);
        case Terminal_session_command_kind::TERMINATE:
            return process_terminate_command(command);
        case Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT:
            return process_force_release_synchronized_output_command(command);
        case Terminal_session_command_kind::BACKEND_OUTPUT:
            return process_backend_output_command(command);
        case Terminal_session_command_kind::BACKEND_EXIT:
            return process_backend_exit_command(command);
        case Terminal_session_command_kind::BACKEND_ERROR:
            return process_backend_error_command(command);
    }

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_start_command(
    const Terminal_session_command& command)
{
    if (m_backend == nullptr || !command.launch_config.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("session start requires a backend and launch config")));
    }

    if (m_process_state != Terminal_process_state::NOT_STARTED) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("session is already running")));
    }

    const Terminal_backend_result config_result =
        validate_launch_config(*command.launch_config);
    if (is_backend_rejection(config_result)) {
        m_process_state = Terminal_process_state::FAILED;
        m_backend_ready = false;
        record_backend_error(command.sequence, *config_result.error);
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            *config_result.error);
    }

    if (!is_terminal_screen_model_grid_size_supported(*command.launch_config->initial_grid_size)) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
            QStringLiteral("initial terminal size exceeds screen model limits"));
        m_process_state = Terminal_process_state::FAILED;
        m_backend_ready = false;
        record_backend_error(command.sequence, error);
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            std::move(error));
    }

    m_grid_size = *command.launch_config->initial_grid_size;
    initialize_screen_model(m_grid_size);

    m_process_state = Terminal_process_state::STARTING;
    m_backend_geometry_in_sync = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_session_start(
            command.sequence,
            *command.launch_config,
            m_config);
    }
#endif
    const Terminal_backend_result backend_result =
        m_backend->start(*command.launch_config, make_backend_callbacks());
    drain_backend_callback_commands();

    if (is_backend_rejection(backend_result)) {
        m_process_state            = Terminal_process_state::FAILED;
        m_backend_ready            = false;
        m_backend_geometry_in_sync = false;
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return make_backend_rejected_result(command.sequence, backend_result.error);
    }

    if (m_stop_requested) {
        m_process_state            = Terminal_process_state::RUNNING;
        m_backend_ready            = false;
        m_backend_geometry_in_sync = false;
        return make_backend_rejected_result(
            command.sequence,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output overflowed during start")));
    }

    m_process_state            = Terminal_process_state::RUNNING;
    m_backend_ready            = true;
    m_backend_geometry_in_sync = true;
    record_notification({
        Terminal_session_notification_kind::PROCESS_STARTED,
        command.sequence,
        QStringLiteral("process started"),
    });

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_write_command(
    const Terminal_session_command& command)
{
    std::uint64_t interaction_trace_id = command.interaction_trace_id;
    if (interaction_trace_id == 0U && interaction_trace_enabled()) {
        interaction_trace_id = next_interaction_trace_correlation_id();
    }

    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::WRITE_FAILED,
            QStringLiteral("session write requires a running backend"));
        if (command.kind == Terminal_session_command_kind::TERMINAL_REPLY) {
            record_backend_error(command.sequence, error);
        }
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            std::move(error));
    }

    if (command.bytes.isEmpty()) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::WRITE_FAILED,
            QStringLiteral("session write requires bytes"));
        if (command.kind == Terminal_session_command_kind::TERMINAL_REPLY) {
            record_backend_error(command.sequence, error);
        }
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            std::move(error));
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        const QString source =
            command.kind == Terminal_session_command_kind::TERMINAL_REPLY
                ? QStringLiteral("terminal_reply")
                : command.kind == Terminal_session_command_kind::USER_PASTE
                    ? QStringLiteral("paste")
                    : command.kind == Terminal_session_command_kind::USER_MESSAGE
                        ? QStringLiteral("message")
                        : QStringLiteral("user");
        (void)m_config.transcript_recorder->record_host_write(
            command.sequence,
            source,
            command.bytes);
    }
#endif

    if (interaction_trace_enabled()) {
        record_interaction_trace(
            "session",
            "write-dispatch",
            interaction_trace_byte_summary(command.bytes),
            interaction_trace_id);
    }
    const Interaction_trace_scope trace_scope(interaction_trace_id);
    const Terminal_backend_result backend_result = m_backend->write(command.bytes);
    if (interaction_trace_enabled()) {
        record_interaction_trace(
            "session",
            is_backend_rejection(backend_result) ? "write-rejected" : "write-accepted",
            interaction_trace_byte_summary(command.bytes),
            interaction_trace_id);
    }
    if (is_backend_rejection(backend_result)) {
        record_backend_error(command.sequence, *backend_result.error);
        return make_backend_rejected_result(command.sequence, backend_result.error);
    }

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_resize_command(
    Terminal_session_command command)
{
    if (!command.resize.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize command requires a transaction")));
    }

    Terminal_resize_transaction resize = *command.resize;
    if (m_screen_model.has_value()) {
        resize.active_buffer = m_screen_model->active_buffer_id();
    }

    if (!is_terminal_screen_model_grid_size_supported(resize.target_grid_size)) {
        resize.model_result             = Terminal_model_resize_result::INVALID_GRID_SIZE;
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.snapshot_grid_size       = m_grid_size;
        resize.backend_geometry_in_sync = m_backend_geometry_in_sync;
        finalize_resize_transaction(
            resize,
            command.sequence,
            QStringLiteral("resize rejected"));

        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                is_valid_grid_size(resize.target_grid_size)
                    ? QStringLiteral("resize exceeds screen model limits")
                    : QStringLiteral("resize requires a positive grid")));
    }

    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        resize.model_result             = Terminal_model_resize_result::NOT_APPLIED;
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.snapshot_grid_size       = m_grid_size;
        resize.backend_geometry_in_sync = m_backend_geometry_in_sync;
        finalize_resize_transaction(
            resize,
            command.sequence,
            QStringLiteral("resize requires a running backend"));

        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize requires a running backend")));
    }

    if (!m_screen_model.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize requires an initialized screen model")));
    }

    const terminal_grid_size_t previous_grid_size = m_grid_size;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_session_resize_request(command.sequence, resize);
    }
#endif

    Resize_transition_context resize_context;
    resize_context.session     = this;
    resize_context.host_resize = &resize;
    resize_context.sequence    = command.sequence;
    const terminal_screen_model_resize_transition_sink_t resize_transition_sink{
        &resize_context,
        &Terminal_session::consume_screen_model_resize_transition_callback,
        command.sequence,
    };
    const Terminal_screen_model_result model_result = m_screen_model->resize(
        resize.target_grid_size,
        &resize_transition_sink);
    if (!resize_context.transition_consumed) {
        resize.snapshot_grid_size = m_grid_size;
        resize.model_result       = Terminal_model_resize_result::APPLIED;
        const bool backend_geometry_was_in_sync = m_backend_geometry_in_sync;
        const Terminal_backend_result backend_result =
            m_backend->resize({resize.id, resize.target_grid_size});
        if (is_backend_rejection(backend_result)) {
            resize.backend_result           = Terminal_backend_resize_result::FAILED;
            resize.backend_geometry_in_sync = false;
            m_backend_geometry_in_sync      = false;
            resize_context.backend_error    = backend_result.error;
            resize_context.render_snapshot_metadata_changed =
                backend_geometry_was_in_sync;
            // The failed resize path keeps the backend error before the resize-failed notification.
            record_resize_transaction(resize);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            if (m_config.transcript_recorder != nullptr) {
                (void)m_config.transcript_recorder->record_session_resize(command.sequence, resize);
            }
#endif
            record_backend_error(command.sequence, *backend_result.error);
            record_notification({
                Terminal_session_notification_kind::RESIZE_TRANSACTION,
                command.sequence,
                QStringLiteral("resize failed"),
                std::nullopt,
                std::nullopt,
                resize,
                false,
            });
        }
        else {
            resize.backend_result           = Terminal_backend_resize_result::APPLIED;
            resize.backend_geometry_in_sync = true;
            m_backend_geometry_in_sync      = true;
            resize_context.render_snapshot_metadata_changed =
                !backend_geometry_was_in_sync;
            finalize_resize_transaction(
                resize,
                command.sequence,
                QStringLiteral("resize applied"));
        }
    }

    sync_viewport_from_model_result(model_result, std::nullopt);
    m_render_snapshot_model_result = model_result;
    const bool render_snapshot_available =
        model_allows_render_snapshot(*m_screen_model);
    const bool grid_size_changed = !grid_sizes_match(previous_grid_size, m_grid_size);

    publish_resize_outcome(
        command.sequence,
        render_snapshot_available,
        grid_size_changed,
        resize_context.render_snapshot_metadata_changed,
        model_result);

    if (resize_context.backend_error.has_value()) {
        return make_backend_rejected_result(
            command.sequence,
            resize_context.backend_error);
    }

    return make_accepted_result(command.sequence);
}

void Terminal_session::finalize_resize_transaction(
    const Terminal_resize_transaction& resize,
    std::uint64_t                      sequence,
    QString                            message)
{
    record_resize_transaction(resize);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_session_resize(sequence, resize);
    }
#endif
    record_notification({
        Terminal_session_notification_kind::RESIZE_TRANSACTION,
        sequence,
        std::move(message),
        std::nullopt,
        std::nullopt,
        resize,
        false,
    });
}

void Terminal_session::consume_screen_model_resize_transition_callback(
    void* context,
    const terminal_screen_model_resize_transition_t& transition)
{
    auto& resize_context = *static_cast<Resize_transition_context*>(context);
    Q_ASSERT(resize_context.session != nullptr);
    resize_context.session->consume_screen_model_resize_transition(
        resize_context,
        transition);
}

void Terminal_session::consume_screen_model_resize_transition(
    Resize_transition_context&                        context,
    const terminal_screen_model_resize_transition_t& transition)
{
    Q_ASSERT(m_screen_model.has_value());
    Q_ASSERT(transition.sequence == context.sequence);
    Q_ASSERT(grid_sizes_match(m_grid_size, transition.grid_size_before));

    const Terminal_viewport_state previous_viewport = m_viewport_controller.state();
    m_grid_size = transition.grid_size_after;
    sync_viewport_from_model_result(
        transition.result,
        context.detached_viewport_anchor,
        false);

    const bool render_snapshot_available = model_allows_render_snapshot(*m_screen_model);
    if (!render_snapshot_available) {
        invalidate_public_projection(
            Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED);
        accumulate_synchronized_selection_continuity(transition.result);
    }

    const Terminal_screen_model_result selection_basis_result =
        model_result_with_deferred_synchronized_row_origins(transition.result);
    const std::optional<Terminal_selection_attachment_resolution_status>
        synchronized_failure = !render_snapshot_available &&
            m_synchronized_selection_continuity_hold.has_value() &&
            m_synchronized_selection_continuity_hold->selection.has_value()
                ? m_synchronized_selection_continuity_hold->selection->failure
                : std::nullopt;
    const std::optional<Terminal_selection_attachment_resolution_status>
        synchronized_drag_failure = !render_snapshot_available &&
            m_synchronized_selection_continuity_hold.has_value() &&
            m_synchronized_selection_continuity_hold->drag.has_value()
                ? m_synchronized_selection_continuity_hold->drag->failure
                : std::nullopt;
    advance_selection_content_basis_for_model_result(
        selection_basis_result,
        previous_viewport,
        transition.grid_size_before,
        synchronized_failure,
        synchronized_drag_failure,
        true);
    if (!render_snapshot_available) {
        rebase_synchronized_selection_continuity_hold();
    }

    Terminal_resize_transaction resize = context.host_resize != nullptr
        ? *context.host_resize
        : Terminal_resize_transaction{};
    if (context.host_resize == nullptr) {
        resize.id = next_resize_id();
    }
    resize.target_grid_size   = transition.grid_size_after;
    resize.active_buffer      = m_screen_model->active_buffer_id();
    resize.model_result       = Terminal_model_resize_result::APPLIED;
    resize.snapshot_grid_size = transition.grid_size_after;

    const bool backend_geometry_was_in_sync = m_backend_geometry_in_sync;
    const QString outcome_prefix = context.host_resize != nullptr
        ? QStringLiteral("resize")
        : QStringLiteral("text-area resize");
    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        context.render_snapshot_metadata_changed =
            context.render_snapshot_metadata_changed || backend_geometry_was_in_sync;
        record_resize_transaction(resize);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            context.sequence,
            outcome_prefix + QStringLiteral(" requires a running backend"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });
    }
    else {
        const Terminal_backend_result backend_result =
            m_backend->resize({resize.id, resize.target_grid_size});
        if (is_backend_rejection(backend_result)) {
            resize.backend_result           = Terminal_backend_resize_result::FAILED;
            resize.backend_geometry_in_sync = false;
            m_backend_geometry_in_sync      = false;
            context.backend_error           = backend_result.error;
            context.render_snapshot_metadata_changed =
                context.render_snapshot_metadata_changed || backend_geometry_was_in_sync;
            record_resize_transaction(resize);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            if (context.host_resize != nullptr && m_config.transcript_recorder != nullptr) {
                (void)m_config.transcript_recorder->record_session_resize(
                    context.sequence,
                    resize);
            }
#endif
            record_backend_error(context.sequence, *backend_result.error);
            record_notification({
                Terminal_session_notification_kind::RESIZE_TRANSACTION,
                context.sequence,
                outcome_prefix + QStringLiteral(" failed"),
                std::nullopt,
                std::nullopt,
                resize,
                false,
            });
        }
        else {
            resize.backend_result           = Terminal_backend_resize_result::APPLIED;
            resize.backend_geometry_in_sync = true;
            m_backend_geometry_in_sync      = true;
            context.render_snapshot_metadata_changed =
                context.render_snapshot_metadata_changed || !backend_geometry_was_in_sync;
            if (context.host_resize != nullptr) {
                finalize_resize_transaction(
                    resize,
                    context.sequence,
                    outcome_prefix + QStringLiteral(" applied"));
            }
            else {
                record_resize_transaction(resize);
                record_notification({
                    Terminal_session_notification_kind::RESIZE_TRANSACTION,
                    context.sequence,
                    outcome_prefix + QStringLiteral(" applied"),
                    std::nullopt,
                    std::nullopt,
                    resize,
                    false,
                });
            }
        }
    }

    if (context.host_resize != nullptr) {
        *context.host_resize = resize;
    }
    context.transition_consumed = true;
}

void Terminal_session::consume_screen_model_text_area_resize_request_callback(
    void* context,
    const terminal_screen_model_text_area_resize_request_t& request)
{
    auto& resize_context = *static_cast<Resize_transition_context*>(context);
    Q_ASSERT(resize_context.session != nullptr);
    resize_context.session->consume_screen_model_text_area_resize_request(
        resize_context,
        request);
}

void Terminal_session::consume_screen_model_text_area_resize_request(
    Resize_transition_context&                              context,
    const terminal_screen_model_text_area_resize_request_t& request)
{
    Q_ASSERT(m_screen_model.has_value());
    Q_ASSERT(context.host_resize == nullptr);
    Q_ASSERT(request.sequence == context.sequence);
    Q_ASSERT(request.ordinal == context.parser_resize_requests.size() + 1U);
    Q_ASSERT(grid_sizes_match(m_screen_model->grid_size(), request.grid_size));
    Q_ASSERT(grid_sizes_match(m_grid_size, request.grid_size));

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_text_area_resize_request(
            context.sequence,
            request.grid_size);
    }
#endif
    if (!request.model_grid_changed) {
        context.render_snapshot_metadata_changed =
            retry_text_area_resize_request(
                context.sequence,
                request.grid_size) ||
            context.render_snapshot_metadata_changed;
    }
    record_notification({
        Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED,
        context.sequence,
        QStringLiteral("text-area resize requested"),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        false,
        std::nullopt,
        request.grid_size,
    });
    context.parser_resize_requests.push_back(request.grid_size);
}

void Terminal_session::publish_resize_outcome(
    std::uint64_t                       sequence,
    bool                                render_snapshot_available,
    bool                                grid_size_changed,
    bool                                geometry_transition_warrants_publication,
    const Terminal_screen_model_result& model_result)
{
    if (!render_snapshot_available &&
        (grid_size_changed ||
            geometry_transition_warrants_publication ||
            m_latest_render_snapshot == nullptr))
    {
        publish_synchronized_resize_snapshot(
            sequence,
            QStringLiteral("resize geometry snapshot ready"));
    }
    else
    if (render_snapshot_available &&
        (model_result_warrants_render_snapshot(model_result) ||
            geometry_transition_warrants_publication))
    {
        publish_render_snapshot(sequence, QStringLiteral("resize snapshot ready"));
    }
}

Terminal_session_result Terminal_session::process_interrupt_command(
    const Terminal_session_command& command)
{
    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::INTERRUPT_FAILED,
                QStringLiteral("interrupt requires a running backend")));
    }

    const Terminal_backend_result backend_result = m_backend->interrupt();
    if (is_backend_rejection(backend_result)) {
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return make_backend_rejected_result(command.sequence, backend_result.error);
    }

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_terminate_command(
    const Terminal_session_command& command)
{
    if (m_backend           == nullptr                           ||
        (m_process_state != Terminal_process_state::RUNNING &&
         m_process_state != Terminal_process_state::STARTING) ||
        m_stop_requested)
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                QStringLiteral("terminate requires a live backend")));
    }

    m_stop_requested          = true;
    m_stop_requested_sequence = command.sequence;
    m_backend_ready           = false;
    const Terminal_backend_result backend_result = m_backend->terminate();
    if (is_backend_rejection(backend_result)) {
        m_stop_requested          = false;
        m_stop_requested_sequence = 0U;
        m_backend_ready           = true;
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return make_backend_rejected_result(command.sequence, backend_result.error);
    }

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_force_release_synchronized_output_command(
    const Terminal_session_command& command)
{
    return force_release_synchronized_output_locked(command.sequence);
}

Terminal_session_result Terminal_session::force_release_synchronized_output_locked(
    std::uint64_t sequence)
{
    if (!m_screen_model.has_value()) {
        return make_rejected_result(
            sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::READ_FAILED,
                QStringLiteral("synchronized output release requires an initialized screen model")));
    }

    const bool had_public_projection_hold = public_projection_hold_active();
    const std::optional<Terminal_public_release_intent> release_intent =
        had_public_projection_hold
            ? std::optional<Terminal_public_release_intent>(
                m_public_viewport_controller.release_intent())
            : std::nullopt;
    const std::optional<Live_primary_viewport_anchor> detached_viewport_anchor =
        capture_live_primary_detached_viewport_anchor();
    terminal_screen_model_trailing_changes_t trailing_changes;
    Terminal_screen_model_result model_result =
        m_screen_model->force_release_synchronized_output(&trailing_changes);
    const bool render_result_warrants_snapshot =
        model_result_warrants_render_snapshot(model_result);
    if (m_config.capture_last_model_ingest_result) {
        m_last_model_ingest_result = model_result;
    }
    const Terminal_viewport_state previous_viewport  = m_viewport_controller.state();
    const terminal_grid_size_t    previous_grid_size = m_grid_size;
    m_render_snapshot_model_result = model_result;
    sync_viewport_from_model_result(model_result, detached_viewport_anchor);
    apply_trailing_changes(model_result, trailing_changes);
    accumulate_synchronized_selection_continuity(model_result);

    if (render_result_warrants_snapshot || m_visual_bell_active) {
        Terminal_screen_model_result selection_basis_result =
            model_result_with_deferred_synchronized_row_origins(model_result);
        selection_basis_result = compose_synchronized_selection_release(
            std::move(selection_basis_result));
        const std::optional<Terminal_selection_attachment_resolution_status>
            synchronized_failure = synchronized_selection_release_failure();
        const std::optional<Terminal_selection_attachment_resolution_status>
            synchronized_drag_failure = m_synchronized_selection_continuity_hold.has_value()
                && m_synchronized_selection_continuity_hold->drag.has_value()
                ? m_synchronized_selection_continuity_hold->drag->failure
                : std::nullopt;
        advance_selection_content_basis_for_model_result(
            selection_basis_result,
            previous_viewport,
            previous_grid_size,
            synchronized_failure,
            synchronized_drag_failure,
            true);
        const std::optional<Terminal_public_scroll_diagnostics> release_diagnostics =
            release_intent.has_value()
                ? reconcile_public_projection_release(
                    *release_intent,
                    m_viewport_controller.state())
                : synchronized_output_policy_change_diagnostics();
        publish_render_snapshot(
            sequence,
            QStringLiteral("synchronized output force released"),
            Terminal_render_snapshot_purpose::CONTENT,
            release_diagnostics.value_or(Terminal_public_scroll_diagnostics{}));
    }
    if (model_allows_render_snapshot(*m_screen_model)) {
        reset_synchronized_selection_continuity_hold();
        reset_synchronized_output_policy_lifecycle();
    }
    if (had_public_projection_hold && model_allows_render_snapshot(*m_screen_model)) {
        reset_public_projection_lifecycle();
    }

    return make_accepted_result(sequence);
}

Terminal_session_result Terminal_session::process_backend_output_command(
    const Terminal_session_command& command)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::process_backend_output_command");

    if (should_ignore_backend_output_after_stop(command.sequence)) {
        complete_processing_backend_callback_side_effects();
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output ignored after terminal stop request")));
    }

    record_output_chunk(command.bytes);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_backend_output(
            command.sequence,
            command.bytes);
    }
#endif
    if (!m_screen_model.has_value()) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::READ_FAILED,
            QStringLiteral("backend output requires an initialized screen model"));
        record_backend_error(command.sequence, error);
        complete_processing_backend_callback_side_effects();
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            std::move(error));
    }

    if (!command.bytes.isEmpty()) {
        record_output_activity(command.sequence);
    }

    const Terminal_utf8_scan_state backend_output_prescan_utf8_state =
        m_backend_output_prescan_utf8_state;
    const bool use_plain_ascii_prescan_fast_path =
        !command.bytes.isEmpty()                                      &&
        m_backend_output_prescan_pending.isEmpty()                    &&
        utf8_scan_state_is_reset(backend_output_prescan_utf8_state)   &&
        backend_output_is_plain_ascii_without_prescan_intro(command.bytes);
    if (use_plain_ascii_prescan_fast_path) {
        ingest_backend_output_segment(
            command.sequence,
            QByteArrayView(command.bytes),
            true);
        return make_accepted_result(command.sequence);
    }

    QByteArray combined_output;
    QByteArrayView remaining(command.bytes);
    if (!m_backend_output_prescan_pending.isEmpty()) {
        combined_output = m_backend_output_prescan_pending + command.bytes;
        m_backend_output_prescan_pending.clear();
        remaining = QByteArrayView(combined_output);
    }

    const qsizetype incomplete_csi_start = trailing_incomplete_csi_start(
        remaining,
        backend_output_prescan_utf8_state);
    if (incomplete_csi_start >= 0) {
        const qsizetype pending_size = remaining.size() - incomplete_csi_start;
        if (static_cast<std::size_t>(pending_size) <= k_control_sequence_pending_limit_bytes) {
            m_backend_output_prescan_pending = QByteArray(
                remaining.data() + incomplete_csi_start,
                pending_size);
            remaining = remaining.sliced(0, incomplete_csi_start);
        }
    }

    Terminal_utf8_scan_state remaining_utf8_scan_state = backend_output_prescan_utf8_state;
    while (!remaining.empty()) {
        if (immediate_public_projection_policy_enabled() &&
            m_screen_model->mode_state().synchronized_output)
        {
            const Sync_set_sequence sync_reset = next_synchronized_output_reset_sequence(
                remaining,
                remaining_utf8_scan_state);
            if (sync_reset.start >= 0) {
                if (sync_reset.start > 0) {
                    ingest_backend_output_segment(
                        command.sequence,
                        remaining.sliced(0, sync_reset.start));
                    remaining = remaining.sliced(sync_reset.start);
                    reset_utf8_scan_state(remaining_utf8_scan_state);
                    continue;
                }

                flush_deferred_backend_content_snapshot();
                const QByteArray prefix = sync_sequence_prefix(remaining, sync_reset, 'l');
                if (!prefix.isEmpty()) {
                    ingest_backend_output_segment(command.sequence, QByteArrayView(prefix));
                    combined_output =
                        sync_sequence_from_sync_parameter_and_tail(remaining, sync_reset, 'l');
                    remaining = QByteArrayView(combined_output);
                    reset_utf8_scan_state(remaining_utf8_scan_state);
                    continue;
                }

                const QByteArray release = sync_sequence_only('l');
                combined_output =
                    sync_sequence_post_parameter_suffix_and_tail(remaining, sync_reset, 'l');
                ingest_backend_output_segment(
                    command.sequence,
                    QByteArrayView(release),
                    m_backend_output_prescan_pending.isEmpty() && combined_output.isEmpty());
                remaining = QByteArrayView(combined_output);
                reset_utf8_scan_state(remaining_utf8_scan_state);
                continue;
            }
        }

        const Sync_set_sequence sync_set = next_synchronized_output_set_sequence(
            remaining,
            remaining_utf8_scan_state);
        if (sync_set.start < 0) {
            ingest_backend_output_segment(
                command.sequence,
                remaining,
                m_backend_output_prescan_pending.isEmpty());
            break;
        }

        if (sync_set.start > 0) {
            ingest_backend_output_segment(
                command.sequence,
                remaining.sliced(0, sync_set.start));
            remaining = remaining.sliced(sync_set.start);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        const bool synchronized_output_entry_boundary =
            !m_screen_model->mode_state().synchronized_output;
        if (synchronized_output_entry_boundary) {
            flush_deferred_backend_content_snapshot();
            latch_synchronized_output_scroll_policy_for_new_hold();
        }
        const bool immediate_entry_boundary =
            synchronized_output_entry_boundary &&
            immediate_public_projection_policy_enabled();
        const QByteArray prefix = sync_sequence_prefix(remaining, sync_set, 'h');
        if (!prefix.isEmpty()) {
            ingest_backend_output_segment(command.sequence, QByteArrayView(prefix));
            combined_output =
                immediate_entry_boundary
                    ? sync_sequence_from_sync_parameter_and_tail(remaining, sync_set, 'h')
                    : sync_sequence_suffix_and_tail(remaining, sync_set);
            remaining = QByteArrayView(combined_output);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        if (immediate_entry_boundary) {
            const QByteArray entry = sync_sequence_only('h');
            combined_output =
                sync_sequence_post_parameter_suffix_and_tail(remaining, sync_set, 'h');
            ingest_backend_output_segment(
                command.sequence,
                QByteArrayView(entry),
                m_backend_output_prescan_pending.isEmpty() && combined_output.isEmpty());
            (void)capture_public_projection_from_latest_content_basis();
            remaining = QByteArrayView(combined_output);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        if (sync_set.end < remaining.size()) {
            ingest_backend_output_segment(
                command.sequence,
                remaining.sliced(0, sync_set.end));
            remaining = remaining.sliced(sync_set.end);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        ingest_backend_output_segment(
            command.sequence,
            remaining,
            m_backend_output_prescan_pending.isEmpty());
        break;
    }

    if (command.bytes.isEmpty() && m_backend_output_prescan_pending.isEmpty()) {
        complete_processing_backend_output_side_effects();
    }

    if (!m_backend_output_prescan_pending.isEmpty()) {
        record_incomplete_processing_backend_output_side_effects();
    }

    m_backend_output_prescan_utf8_state = utf8_scan_state_after(
        command.bytes,
        m_backend_output_prescan_utf8_state);

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_backend_exit_command(
    const Terminal_session_command& command)
{
    if (!command.exit.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend exit command requires exit status")));
    }

    if (m_exit_status.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend exit was already reported")));
    }

    m_exit_status    = *command.exit;
    m_process_state  = command.exit->reason == Terminal_exit_reason::FAILED_TO_START
        ? Terminal_process_state::FAILED
        : Terminal_process_state::EXITED;
    m_backend_ready  = false;
    m_stop_requested = false;
    if (m_backend_output_capture_writer) {
        const Backend_output_capture_writer_result capture_result =
            m_backend_output_capture_writer->finalize();
        if (!capture_result.accepted &&
            !m_backend_output_capture_failure_recorded.exchange(
                true,
                std::memory_order_acq_rel))
        {
            record_backend_error(
                command.sequence,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    capture_result.error));
        }
    }
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_session_process_exit(
            command.sequence,
            *command.exit);
    }
#endif
    record_notification({
        Terminal_session_notification_kind::PROCESS_EXITED,
        command.sequence,
        QStringLiteral("process exited"),
        std::nullopt,
        *command.exit,
        std::nullopt,
        false,
    });

    return make_accepted_result(command.sequence);
}

Terminal_session_result Terminal_session::process_backend_error_command(
    const Terminal_session_command& command)
{
    if (!command.error.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend error command requires an error")));
    }

    record_backend_error(command.sequence, *command.error);
    return make_accepted_result(command.sequence);
}

Terminal_backend_callbacks Terminal_session::make_backend_callbacks()
{
    const std::shared_ptr<Terminal_session_callback_lifetime> lifetime =
        m_callback_lifetime;
    const std::function<void()> backend_event_notifier = m_config.backend_event_notifier;
    const std::function<void(std::uint64_t, bool)> backend_event_epoch_notifier =
        m_config.backend_event_epoch_notifier;
    const auto notify_backend_event = [
        backend_event_notifier,
        backend_event_epoch_notifier](
            Terminal_session* session,
            bool              callback_output_pressure) {
        if (backend_event_epoch_notifier) {
            backend_event_epoch_notifier(
                session->backend_callback_enqueue_epoch(),
                callback_output_pressure);
            if (!backend_event_notifier) {
                return;
            }
        }
        if (backend_event_notifier) {
            // Callback commands are durable in the lifetime queue; the notifier
            // only needs to wake the owner once for all commands queued so far.
            backend_event_notifier();
            return;
        }

        session->process_backend_callback_events();
    };

    Terminal_backend_callbacks callbacks;
    callbacks.output_received =
        [lifetime, notify_backend_event](QByteArray bytes) {
            Backend_callback_invocation callback(lifetime);
            if (Terminal_session* session = callback.session()) {
                session->record_backend_output_capture_chunk(bytes);
            }
            const Terminal_queue_result queue_result =
                callback.enqueue(make_backend_output_command(0U, std::move(bytes)));
            if (Terminal_session* session = callback.session()) {
                const bool callback_output_pressure =
                    queue_result.code == Terminal_queue_result_code::ACCEPTED &&
                    queue_result.high_water_reached;
                if (callback_output_pressure)
                {
                    session->pause_backend_output_from_callback_ingress();
                }
                notify_backend_event(session, callback_output_pressure);
            }
        };
    callbacks.process_exited = [lifetime, notify_backend_event](Terminal_backend_exit exit) {
        Backend_callback_invocation callback(lifetime);
        callback.enqueue(make_backend_exit_command(0U, exit));
        if (Terminal_session* session = callback.session()) {
            notify_backend_event(session, false);
        }
    };
    callbacks.error_reported = [lifetime, notify_backend_event](Terminal_backend_error error) {
        Backend_callback_invocation callback(lifetime);
        callback.enqueue(make_backend_error_command(0U, std::move(error)));
        if (Terminal_session* session = callback.session()) {
            notify_backend_event(session, false);
        }
    };
    return callbacks;
}

void Terminal_session::process_backend_callback_events()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    (void)process_pending_commands();
}

Backend_callback_drain_stop Terminal_session::process_backend_callback_events_for(
    std::chrono::steady_clock::duration budget)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const std::chrono::steady_clock::duration zero =
        std::chrono::steady_clock::duration::zero();
    if (budget < zero) {
        budget = zero;
    }

    return process_pending_commands(
        Backend_callback_drain_policy::DRAIN_CALLBACKS,
        std::chrono::steady_clock::now() + budget);
}

Backend_callback_drain_stop Terminal_session::process_backend_callback_events_until_epoch(
    std::uint64_t target_epoch,
    std::optional<std::chrono::steady_clock::duration> budget)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (target_epoch == 0U ||
        m_last_processed_backend_callback_epoch >= target_epoch)
    {
        return Backend_callback_drain_stop::COMPLETE;
    }

    Backend_callback_drain_deadline deadline = std::nullopt;
    if (budget.has_value()) {
        const std::chrono::steady_clock::duration zero =
            std::chrono::steady_clock::duration::zero();
        deadline = std::chrono::steady_clock::now() + std::max(*budget, zero);
    }

    return process_pending_commands(
        Backend_callback_drain_policy::DRAIN_CALLBACKS,
        deadline,
        target_epoch);
}

void Terminal_session::pause_backend_output_from_callback_ingress()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_callback_lifetime->high_water_reached() &&
        !queue_high_water_reached(Queue_category::OUTPUT))
    {
        return;
    }

    const std::uint64_t sequence = next_sequence();
    if (m_output_backpressure_active) {
        const bool backend_can_accept_pause =
            m_backend != nullptr &&
            (m_process_state == Terminal_process_state::RUNNING ||
             m_process_state == Terminal_process_state::STARTING) &&
            !m_stop_requested;
        if (backend_can_accept_pause) {
            const Terminal_backend_result pause_result = m_backend->set_output_paused(true);
            if (is_backend_rejection(pause_result)) {
                record_backend_error(sequence, *pause_result.error);
            }
        }
        return;
    }

    set_output_backpressure_active(true, sequence);
}

void Terminal_session::drain_backend_callback_commands()
{
    for (;;) {
        std::vector<std::uint64_t> backend_output_capture_failure_epochs;
        std::deque<Terminal_session_command> commands =
            m_callback_lifetime->take_pending_commands(
                backend_output_capture_failure_epochs);
        if (commands.empty()) {
            return;
        }

        while (!commands.empty()) {
            Terminal_session_command command = std::move(commands.front());
            commands.pop_front();

            command.sequence = next_sequence();
            const bool backend_output_capture_failure =
                std::find(
                    backend_output_capture_failure_epochs.begin(),
                    backend_output_capture_failure_epochs.end(),
                    command.backend_callback_epoch) !=
                backend_output_capture_failure_epochs.end();
            if (command.kind == Terminal_session_command_kind::BACKEND_ERROR &&
                m_processing_commands &&
                !backend_output_capture_failure)
            {
                m_backend_error_queued_during_command = true;
            }

            if (command.kind        == Terminal_session_command_kind::BACKEND_ERROR &&
                command.error.has_value()                                           &&
                command.error->code == Terminal_backend_error_code::OUTPUT_OVERFLOW)
            {
                Terminal_session_result result = handle_output_overflow(
                    command.sequence,
                    command.error->message);
                record_result(std::move(result));
                advance_processed_backend_callback_epoch(command.backend_callback_epoch);
                continue;
            }

            if (command.kind == Terminal_session_command_kind::BACKEND_OUTPUT &&
                should_ignore_backend_output_after_stop(command.sequence))
            {
                Terminal_session_result result = make_rejected_result(
                    command.sequence,
                    Terminal_session_result_code::INVALID_STATE,
                    make_backend_error(
                        Terminal_backend_error_code::OUTPUT_OVERFLOW,
                        QStringLiteral("backend output ignored after terminal stop request")));
                record_result(std::move(result));
                advance_processed_backend_callback_epoch(command.backend_callback_epoch);
                continue;
            }

            const std::uint64_t command_backend_callback_epoch =
                command.backend_callback_epoch;
            Terminal_session_result result = enqueue_command(std::move(command));
            if (result.code != Terminal_session_result_code::ACCEPTED) {
                advance_processed_backend_callback_epoch(command_backend_callback_epoch);
            }
            record_result(std::move(result));
        }
    }
}

void Terminal_session::record_processed_command(Terminal_session_command command)
{
    if (m_config.trace_command_limit == 0U) {
        return;
    }

    push_bounded(m_processed_commands, std::move(command), m_config.trace_command_limit);
}

void Terminal_session::record_notification(Terminal_session_notification notification)
{
    record_pending_notification(notification);

    if (m_config.trace_notification_limit == 0U) {
        return;
    }

    push_bounded(m_notifications, std::move(notification), m_config.trace_notification_limit);
}

void Terminal_session::record_pending_notification(
    Terminal_session_notification notification)
{
    const auto same_kind = [&notification](const Terminal_session_notification& pending) {
        return pending.kind == notification.kind;
    };

    switch (notification.kind) {
        case Terminal_session_notification_kind::SNAPSHOT_READY:
        case Terminal_session_notification_kind::RESIZE_TRANSACTION:
            return;
        case Terminal_session_notification_kind::BELL_REQUESTED:
            if (auto it = std::find_if(
                    m_pending_notifications.begin(),
                    m_pending_notifications.end(),
                    same_kind);
                it != m_pending_notifications.end())
            {
                it->sequence      = notification.sequence;
                it->message       = std::move(notification.message);
                it->bell_audible  = it->bell_audible || notification.bell_audible;
                it->bell_visual   = it->bell_visual  || notification.bell_visual;
                return;
            }
            break;
        case Terminal_session_notification_kind::OUTPUT_ACTIVITY:
        case Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED:
        case Terminal_session_notification_kind::TITLE_CHANGED:
        case Terminal_session_notification_kind::ICON_NAME_CHANGED:
        case Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
            if (auto it = std::find_if(
                    m_pending_notifications.begin(),
                    m_pending_notifications.end(),
                    same_kind);
                it != m_pending_notifications.end())
            {
                *it = std::move(notification);
                return;
            }
            break;
        case Terminal_session_notification_kind::PROCESS_STARTED:
        case Terminal_session_notification_kind::PROCESS_EXITED:
        case Terminal_session_notification_kind::BACKEND_ERROR:
        case Terminal_session_notification_kind::HOST_REQUEST:
            break;
    }

    if (m_pending_notifications.size() < k_pending_notification_limit) {
        m_pending_notifications.push_back(std::move(notification));
        return;
    }

    const auto coalescible = [](const Terminal_session_notification& pending) {
        return
            pending.kind == Terminal_session_notification_kind::OUTPUT_ACTIVITY             ||
            pending.kind == Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED ||
            pending.kind == Terminal_session_notification_kind::BELL_REQUESTED              ||
            pending.kind == Terminal_session_notification_kind::TITLE_CHANGED               ||
            pending.kind == Terminal_session_notification_kind::ICON_NAME_CHANGED           ||
            pending.kind == Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED;
    };
    if (auto it = std::find_if(
            m_pending_notifications.begin(),
            m_pending_notifications.end(),
            coalescible);
        it != m_pending_notifications.end())
    {
        *it = std::move(notification);
        return;
    }

    m_pending_notifications.erase(m_pending_notifications.begin());
    m_pending_notifications.push_back(std::move(notification));
}

void Terminal_session::record_resize_transaction(Terminal_resize_transaction resize)
{
    if (m_config.trace_resize_limit == 0U) {
        return;
    }

    push_bounded(m_resize_transactions, std::move(resize), m_config.trace_resize_limit);
}

void Terminal_session::record_backend_output_capture_chunk(QByteArrayView bytes)
{
    if (!m_backend_output_capture_writer) {
        return;
    }

    const Backend_output_capture_writer_result result =
        m_backend_output_capture_writer->append(bytes);
    if (!result.accepted &&
        !m_backend_output_capture_failure_recorded.exchange(
            true,
            std::memory_order_acq_rel))
    {
        m_callback_lifetime->enqueue_backend_output_capture_failure(
            make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                result.error));
    }
}

void Terminal_session::record_output_chunk(QByteArray bytes)
{
    if (m_config.trace_output_chunk_limit == 0U) {
        return;
    }

    push_bounded(m_output_chunks, std::move(bytes), m_config.trace_output_chunk_limit);
}

void Terminal_session::record_output_activity(std::uint64_t sequence)
{
    record_notification({
        Terminal_session_notification_kind::OUTPUT_ACTIVITY,
        sequence,
        QStringLiteral("output activity"),
    });
}

void Terminal_session::advance_processed_backend_callback_epoch(std::uint64_t epoch)
{
    if (epoch == 0U) {
        return;
    }

    m_ready_processed_backend_callback_epoch =
        std::max(m_ready_processed_backend_callback_epoch, epoch);
    flush_ready_processed_backend_callback_epoch();
}

void Terminal_session::flush_ready_processed_backend_callback_epoch()
{
    const std::uint64_t epoch = m_ready_processed_backend_callback_epoch;
    if (epoch == 0U) {
        return;
    }

    if (m_incomplete_backend_output_callback_epoch != 0U &&
        epoch >= m_incomplete_backend_output_callback_epoch)
    {
        return;
    }

    if (pending_backend_callback_blocks_processed_callback_epoch(epoch)) {
        return;
    }

    m_last_processed_backend_callback_epoch =
        std::max(m_last_processed_backend_callback_epoch, epoch);
    m_ready_processed_backend_callback_epoch = 0U;
}

bool Terminal_session::pending_backend_callback_blocks_processed_callback_epoch(
    std::uint64_t epoch) const
{
    for (const Terminal_session_command& command : m_pending_commands) {
        if (command.backend_callback_epoch != 0U &&
            command.backend_callback_epoch <= epoch)
        {
            return true;
        }
    }
    return false;
}

void Terminal_session::complete_processing_backend_callback_side_effects()
{
    advance_processed_backend_callback_epoch(m_processing_backend_callback_epoch);
}

void Terminal_session::complete_processing_backend_output_side_effects()
{
    const std::uint64_t epoch = m_processing_backend_callback_epoch;
    if (epoch == 0U) {
        return;
    }

    if (m_incomplete_backend_output_callback_epoch != 0U &&
        epoch > m_incomplete_backend_output_callback_epoch)
    {
        m_incomplete_backend_output_callback_epoch = 0U;
    }

    advance_processed_backend_callback_epoch(epoch);
}

void Terminal_session::record_incomplete_processing_backend_output_side_effects()
{
    const std::uint64_t epoch = m_processing_backend_callback_epoch;
    if (epoch == 0U) {
        return;
    }

    if (m_incomplete_backend_output_callback_epoch == 0U ||
        epoch < m_incomplete_backend_output_callback_epoch)
    {
        m_incomplete_backend_output_callback_epoch = epoch;
    }
}

void Terminal_session::record_backend_error(
    std::uint64_t          sequence,
    Terminal_backend_error error)
{
    const QString message = error.message;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_session_backend_error(sequence, error);
    }
#endif
    record_notification({
        Terminal_session_notification_kind::BACKEND_ERROR,
        sequence,
        message,
        std::move(error),
        std::nullopt,
        std::nullopt,
        false,
    });
}

void Terminal_session::initialize_screen_model(terminal_grid_size_t grid_size)
{
    Terminal_screen_model_config screen_config;
    screen_config.grid_size                 = grid_size;
    screen_config.scrollback_limit          = m_config.scrollback_limit;

    screen_config.retained_history_capacity_bytes =
        m_config.retained_history_capacity_bytes;

    screen_config.retain_structural_actions = m_config.capture_last_model_ingest_result;
    screen_config.recover_scrollback_from_primary_repaints =
        m_config.recover_scrollback_from_primary_repaints;
    screen_config.text_area_resize_policy = m_config.text_area_resize_policy;
    m_screen_model.emplace(screen_config);
    if (m_color_state.has_value()) {
        m_screen_model->set_color_state(*m_color_state);
    }
    m_screen_model->set_dirty_row_stats_enabled(m_config.capture_dirty_row_stats);
    if (m_config.capture_dirty_row_stats) {
        m_profile_stats.enabled = true;
    }
    m_screen_model->set_profile_stats_enabled(m_profile_stats.enabled);
    m_viewport_controller = Terminal_viewport_controller{};
    m_viewport_controller.set_visible_rows(grid_size.rows);
    m_backend_output_prescan_pending.clear();
    m_incomplete_backend_output_callback_epoch = 0U;
    reset_utf8_scan_state(m_backend_output_prescan_utf8_state);
    m_latest_render_snapshot.reset();
    m_latest_content_render_snapshot.reset();
    m_latest_content_render_snapshot_content_basis = {};
    reset_public_projection_lifecycle();
    m_search_projection.reset();
    m_search.clear();
    m_last_model_ingest_result.reset();
    m_render_snapshot_model_result.reset();
    m_deferred_backend_content_snapshot.reset();
    m_selection.clear();
    m_selection_buffer_id                    = Terminal_buffer_id::PRIMARY;
    m_selection_content_basis                = {};
    m_last_selection_attachment_resolution_status.reset();
    m_ime_preedit                            = {};
    m_render_snapshot_generation             = 0U;
    m_render_snapshot_installed_generation  = 0U;
    m_render_snapshot_rendered_generation   = 0U;
    m_unrendered_render_snapshot_dirty_basis.reset();
    m_next_public_projection_generation      = 1U;
    m_next_search_projection_generation      = 1U;
    m_synchronized_output_hold_policy.reset();
    m_synchronized_output_policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
    m_synchronized_selection_continuity_hold.reset();
    m_ime_preedit_generation            = 0U;
    m_alternate_scroll_mode_generation  = 0U;
    m_active_buffer_epoch               = 0U;
    m_deferred_synchronized_evicted_scrollback_rows = 0;
    ++m_selection_session_epoch;
    if (m_selection_session_epoch == 0U) {
        m_selection_session_epoch = 1U;
    }
    m_visual_bell_active                = false;
}

void Terminal_session::ingest_backend_output_segment(
    std::uint64_t  sequence,
    QByteArrayView bytes,
    bool           completes_backend_output_callback)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::ingest_backend_output_segment");

    if (bytes.empty()) {
        return;
    }

    const bool render_snapshot_was_blocked =
        !model_allows_render_snapshot(*m_screen_model);
    const bool had_public_projection_hold = public_projection_hold_active();
    const std::optional<Terminal_public_release_intent> release_intent =
        had_public_projection_hold
            ? std::optional<Terminal_public_release_intent>(
                m_public_viewport_controller.release_intent())
            : std::nullopt;
    const std::optional<Live_primary_viewport_anchor> detached_viewport_anchor =
        capture_live_primary_detached_viewport_anchor();
    Resize_transition_context resize_context;
    resize_context.session                  = this;
    resize_context.detached_viewport_anchor = detached_viewport_anchor;
    resize_context.sequence                 = sequence;
    terminal_screen_model_trailing_changes_t trailing_changes;
    const terminal_screen_model_resize_transition_sink_t resize_transition_sink{
        &resize_context,
        &Terminal_session::consume_screen_model_resize_transition_callback,
        sequence,
        &trailing_changes,
        &Terminal_session::consume_screen_model_text_area_resize_request_callback,
    };
    Terminal_screen_model_result ingest_result;
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::model_ingest");
        ingest_result = m_screen_model->ingest(bytes, &resize_transition_sink);
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::store_ingest_result");
        if (m_config.capture_last_model_ingest_result) {
            m_last_model_ingest_result = ingest_result;
        }
        m_render_snapshot_model_result = ingest_result;
    }

    bool render_snapshot_metadata_changed = false;
    const Terminal_viewport_state previous_viewport  = m_viewport_controller.state();
    const terminal_grid_size_t    previous_grid_size = m_grid_size;
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::handle_parser_actions");
        handle_parser_actions(
            sequence,
            ingest_result,
            resize_context.parser_resize_requests);
        render_snapshot_metadata_changed =
            resize_context.render_snapshot_metadata_changed;
    }
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::sync_viewport_from_model_result");
        sync_viewport_from_model_result(ingest_result, detached_viewport_anchor);
    }
    const bool render_result_warrants_snapshot =
        model_result_warrants_render_snapshot(ingest_result);
    const bool render_terminal_content_changed = ingest_result.terminal_content_changed;
    apply_trailing_changes(ingest_result, trailing_changes);
    if (completes_backend_output_callback) {
        complete_processing_backend_output_side_effects();
    }

    const bool render_snapshot_available = model_allows_render_snapshot(*m_screen_model);
    if (!render_snapshot_was_blocked && !render_snapshot_available) {
        begin_synchronized_selection_continuity_hold();
    }
    if (!render_snapshot_available ||
        (render_snapshot_was_blocked && render_snapshot_available))
    {
        accumulate_synchronized_selection_continuity(ingest_result);
    }
    if (!render_snapshot_available) {
        record_blocked_synchronized_row_origin_change(ingest_result);
    }
    const bool synchronized_output_released =
        render_snapshot_was_blocked && render_snapshot_available;
    if ((render_result_warrants_snapshot                         ||
        render_snapshot_metadata_changed                      ||
        m_visual_bell_active)
        &&
        render_snapshot_available)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_backend_render_snapshot");
        Terminal_screen_model_result selection_basis_result =
            model_result_with_deferred_synchronized_row_origins(ingest_result);
        if (synchronized_output_released) {
            selection_basis_result = compose_synchronized_selection_release(
                std::move(selection_basis_result));
        }
        const std::optional<Terminal_selection_attachment_resolution_status>
            synchronized_failure = synchronized_output_released
                ? synchronized_selection_release_failure()
                : std::nullopt;
        const std::optional<Terminal_selection_attachment_resolution_status>
            synchronized_drag_failure = synchronized_output_released &&
                m_synchronized_selection_continuity_hold.has_value() &&
                m_synchronized_selection_continuity_hold->drag.has_value()
                    ? m_synchronized_selection_continuity_hold->drag->failure
                    : std::nullopt;
        advance_selection_content_basis_for_model_result(
            selection_basis_result,
            previous_viewport,
            previous_grid_size,
            synchronized_failure,
            synchronized_drag_failure,
            synchronized_output_released);
        const std::optional<Terminal_public_scroll_diagnostics> release_diagnostics =
            release_intent.has_value()
                ? reconcile_public_projection_release(
                    *release_intent,
                    m_viewport_controller.state())
                : synchronized_output_policy_change_diagnostics();
        const bool can_defer_content_snapshot =
            m_backend_content_snapshot_deferral_active &&
            !synchronized_output_released              &&
            !render_snapshot_metadata_changed          &&
            !release_diagnostics.has_value();
        if (can_defer_content_snapshot) {
            defer_backend_content_snapshot(
                sequence,
                QStringLiteral("backend output received"),
                *m_render_snapshot_model_result,
                Terminal_public_scroll_diagnostics{});
        }
        else {
            flush_deferred_backend_content_snapshot();
            const std::uint64_t generation_before_publish =
                m_render_snapshot_generation;
            publish_render_snapshot(
                sequence,
                QStringLiteral("backend output received"),
                Terminal_render_snapshot_purpose::CONTENT,
                release_diagnostics.value_or(Terminal_public_scroll_diagnostics{}));
            if (m_render_snapshot_generation != generation_before_publish) {
                record_backend_callback_drain_content_publication(
                    m_render_snapshot_generation,
                    synchronized_output_released,
                    render_terminal_content_changed);
            }
        }
    }
    if (render_snapshot_available) {
        reset_synchronized_selection_continuity_hold();
        reset_synchronized_output_policy_lifecycle();
    }
    if (had_public_projection_hold && render_snapshot_available) {
        reset_public_projection_lifecycle();
    }
}

void Terminal_session::defer_backend_content_snapshot(
    std::uint64_t                       sequence,
    QString                             message,
    const Terminal_screen_model_result& model_result,
    Terminal_public_scroll_diagnostics  public_scroll_diagnostics)
{
    if (!m_deferred_backend_content_snapshot.has_value()) {
        m_deferred_backend_content_snapshot = Deferred_backend_content_snapshot{
            model_result,
            std::move(public_scroll_diagnostics),
            std::move(message),
            sequence,
        };
        return;
    }

    Deferred_backend_content_snapshot& pending =
        *m_deferred_backend_content_snapshot;
    coalesce_backend_content_model_result(pending.model_result, model_result);
    pending.public_scroll_diagnostics = std::move(public_scroll_diagnostics);
    pending.message                   = std::move(message);
    pending.sequence                  = sequence;
}

void Terminal_session::flush_deferred_backend_content_snapshot()
{
    if (!m_deferred_backend_content_snapshot.has_value()) {
        return;
    }

    Deferred_backend_content_snapshot pending =
        std::move(*m_deferred_backend_content_snapshot);
    m_deferred_backend_content_snapshot.reset();
    m_render_snapshot_model_result = std::move(pending.model_result);
    const bool terminal_content_changed =
        m_render_snapshot_model_result->terminal_content_changed;
    const std::uint64_t generation_before_publish = m_render_snapshot_generation;
    publish_render_snapshot(
        pending.sequence,
        std::move(pending.message),
        Terminal_render_snapshot_purpose::CONTENT,
        pending.public_scroll_diagnostics);
    if (m_render_snapshot_generation != generation_before_publish) {
        record_backend_callback_drain_content_publication(
            m_render_snapshot_generation,
            false,
            terminal_content_changed);
    }
}

void Terminal_session::record_backend_callback_drain_content_publication(
    std::uint64_t publication_generation,
    bool          synchronized_output_release,
    bool          terminal_content_changed)
{
    Q_ASSERT(m_processing_commands);

    if (synchronized_output_release) {
        m_drain_synchronized_release_publication_generation =
            publication_generation;
    }
    if (terminal_content_changed) {
        m_drain_latest_live_content_publication_generation =
            publication_generation;
    }
}

bool Terminal_session::retry_text_area_resize_request(
    std::uint64_t          sequence,
    terminal_grid_size_t   grid_size)
{
    if (!m_screen_model.has_value() ||
        !is_terminal_screen_model_grid_size_supported(grid_size) ||
        !grid_sizes_match(m_screen_model->grid_size(), grid_size) ||
        !grid_sizes_match(m_grid_size, grid_size)                  ||
        m_backend_geometry_in_sync)
    {
        return false;
    }

    const bool backend_geometry_was_in_sync = m_backend_geometry_in_sync;

    Terminal_resize_transaction resize;
    resize.id                 = next_resize_id();
    resize.target_grid_size   = grid_size;
    resize.active_buffer      = m_screen_model->active_buffer_id();
    resize.model_result       = Terminal_model_resize_result::APPLIED;
    resize.snapshot_grid_size = grid_size;

    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        record_resize_transaction(resize);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            sequence,
            QStringLiteral("text-area resize requires a running backend"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });
        return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
    }

    const Terminal_backend_result backend_result =
        m_backend->resize({resize.id, resize.target_grid_size});
    if (is_backend_rejection(backend_result)) {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        record_resize_transaction(resize);
        record_backend_error(sequence, *backend_result.error);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            sequence,
            QStringLiteral("text-area resize failed"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });
        return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
    }

    resize.backend_result           = Terminal_backend_resize_result::APPLIED;
    resize.backend_geometry_in_sync = true;
    m_backend_geometry_in_sync      = true;
    record_resize_transaction(resize);
    record_notification({
        Terminal_session_notification_kind::RESIZE_TRANSACTION,
        sequence,
        QStringLiteral("text-area resize applied"),
        std::nullopt,
        std::nullopt,
        resize,
        false,
    });
    return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
}

void Terminal_session::handle_parser_actions(
    std::uint64_t                          sequence,
    const Terminal_screen_model_result&    result,
    std::span<const terminal_grid_size_t>  consumed_resize_requests)
{
    std::size_t consumed_resize_index = 0U;

    for (const Parser_action& action : result.actions) {
        switch (parser_action_kind(action)) {
            case Parser_action_kind::NOTIFICATION:
                {
                    const Parser_notification& notification =
                        std::get<Parser_notification>(action.payload);
                    switch (notification.kind) {
                        case Parser_notification_kind::BELL_REQUESTED:
                            handle_bell_request(sequence);
                            break;
                        case Parser_notification_kind::TITLE_CHANGED:
                            record_notification({
                                Terminal_session_notification_kind::TITLE_CHANGED,
                                sequence,
                                notification.text,
                            });
                            break;
                        case Parser_notification_kind::ICON_NAME_CHANGED:
                            record_notification({
                                Terminal_session_notification_kind::ICON_NAME_CHANGED,
                                sequence,
                                notification.text,
                            });
                            break;
                        case Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
                            {
                                const terminal_grid_size_t requested_grid_size{
                                    notification.rows,
                                    notification.columns,
                                };
                                Q_ASSERT(
                                    consumed_resize_index < consumed_resize_requests.size());
                                if (consumed_resize_index < consumed_resize_requests.size()) {
                                    Q_ASSERT(grid_sizes_match(
                                        consumed_resize_requests[consumed_resize_index],
                                        requested_grid_size));
                                    ++consumed_resize_index;
                                }
                            }
                            break;
                        case Parser_notification_kind::OUTPUT_ACTIVITY:
                            break;
                    }
                    break;
            }
            case Parser_action_kind::TERMINAL_REPLY:
                {
                    const Terminal_reply& reply          = std::get<Terminal_reply>(action.payload);
                    const std::uint64_t   reply_sequence = next_sequence();
                    const Terminal_session_result enqueue_result = enqueue_command(
                        make_terminal_reply_command(reply_sequence, reply));
                    record_result(enqueue_result);
                    if (enqueue_result.code != Terminal_session_result_code::ACCEPTED &&
                        enqueue_result.error.has_value())
                    {
                        record_backend_error(reply_sequence, *enqueue_result.error);
                    }
                    break;
            }
            case Parser_action_kind::HOST_REQUEST:
                {
                    const Terminal_osc52_write_request& request = std::get<Terminal_osc52_write_request>(
                        action.payload);
                    record_notification({
                        Terminal_session_notification_kind::HOST_REQUEST,
                        sequence,
                        request.source_sequence,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        false,
                        request,
                    });
                    break;
            }
            case Parser_action_kind::SCREEN_MUTATION:
            case Parser_action_kind::STYLE_MUTATION:
            case Parser_action_kind::CONTROL_SEQUENCE:
            case Parser_action_kind::TERMINAL_QUERY:
            case Parser_action_kind::DIAGNOSTIC:
                break;
        }
    }

    Q_ASSERT(consumed_resize_index == consumed_resize_requests.size());
}

void Terminal_session::handle_bell_request(std::uint64_t sequence)
{
    const Terminal_bell_request request =
        record_bell_event(m_bell_state, bell_clock_milliseconds());
    if (!request.audible && !request.visual) {
        return;
    }

    Terminal_session_notification notification;
    notification.kind         = Terminal_session_notification_kind::BELL_REQUESTED;
    notification.sequence     = sequence;
    notification.message      = QStringLiteral("bell requested");
    notification.bell_audible = request.audible;
    notification.bell_visual  = request.visual;
    record_notification(std::move(notification));
    if (request.visual) {
        m_visual_bell_active = true;
    }
}

void Terminal_session::sync_viewport_from_model_result(
    const Terminal_screen_model_result& result,
    std::optional<Live_primary_viewport_anchor> detached_viewport_anchor,
    bool                                        advance_publication_generations)
{
    if (!m_screen_model.has_value()) {
        return;
    }

    if (advance_publication_generations && result.alternate_scroll_mode_changed) {
        ++m_alternate_scroll_mode_generation;
    }
    if (advance_publication_generations && result.active_buffer_changed) {
        ++m_active_buffer_epoch;
        if (m_active_buffer_epoch == 0U) {
            m_active_buffer_epoch = 1U;
        }
    }

    m_viewport_controller.set_visible_rows(m_screen_model->grid_size().rows);
    const Terminal_buffer_id active_buffer =
        active_buffer_after_from_mode_transition_delta(result).value_or(
            m_screen_model->active_buffer_id());
    m_viewport_controller.set_scrollback_rows(result.scrollback_rows);
    if (!m_selection.has_selection())                   { m_selection_buffer_id = active_buffer;          }
    if (active_buffer == Terminal_buffer_id::ALTERNATE) {
        if (detached_viewport_anchor.has_value()) {
            resolve_live_primary_detached_viewport_anchor(*detached_viewport_anchor);
        }
        m_viewport_controller.enter_alternate_screen();
    }
    else {
        m_viewport_controller.leave_alternate_screen();
        if (detached_viewport_anchor.has_value()) {
            resolve_live_primary_detached_viewport_anchor(*detached_viewport_anchor);
        }
    }
}

std::optional<Terminal_session::Live_primary_viewport_anchor>
Terminal_session::capture_live_primary_detached_viewport_anchor() const
{
    if (!m_screen_model.has_value()) {
        return std::nullopt;
    }

    const Terminal_viewport_state viewport = m_viewport_controller.state();
    if (viewport.active_buffer != Terminal_buffer_id::PRIMARY ||
        viewport.follow_tail                                   ||
        viewport.offset_from_tail <= 0)
    {
        return std::nullopt;
    }

    const int first_visible_logical_row =
        viewport.scrollback_rows - viewport.offset_from_tail;
    if (first_visible_logical_row < 0 ||
        first_visible_logical_row >= viewport.scrollback_rows)
    {
        return std::nullopt;
    }

    const std::optional<terminal_history_handle_t> history_handle =
        m_screen_model->retained_history_handle_at_logical_row(
            Terminal_buffer_id::PRIMARY,
            first_visible_logical_row);
    if (!history_handle.has_value()) {
        return std::nullopt;
    }

    return Live_primary_viewport_anchor{*history_handle, 0};
}

void Terminal_session::resolve_live_primary_detached_viewport_anchor(
    const Live_primary_viewport_anchor& detached_viewport_anchor)
{
    if (!m_screen_model.has_value() ||
        m_viewport_controller.state().active_buffer != Terminal_buffer_id::PRIMARY)
    {
        return;
    }

    const Terminal_viewport_state viewport = m_viewport_controller.state();
    Terminal_retained_line_lookup_result lookup;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_session::resolve_live_primary_detached_viewport_anchor::retained_line_lookup");
        lookup = m_screen_model->retained_line_lookup(
            Terminal_buffer_id::PRIMARY,
            detached_viewport_anchor.history_handle);
    }
    if (lookup.exact_match) {
        const int first_visible_logical_row =
            lookup.exact_logical_row - detached_viewport_anchor.viewport_row;
        scroll_live_primary_viewport_to_offset_from_tail(
            viewport.scrollback_rows - first_visible_logical_row);
        return;
    }

    if (lookup.nearest_successor) {
        const int first_visible_logical_row =
            lookup.nearest_successor_logical_row - detached_viewport_anchor.viewport_row;
        scroll_live_primary_viewport_to_offset_from_tail(
            viewport.scrollback_rows - first_visible_logical_row);
        return;
    }

    scroll_live_primary_viewport_to_offset_from_tail(viewport.scrollback_rows);
}

void Terminal_session::scroll_live_primary_viewport_to_offset_from_tail(
    int offset_from_tail)
{
    const Terminal_viewport_state viewport = m_viewport_controller.state();
    if (viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
        return;
    }

    const int target_offset =
        std::clamp(offset_from_tail, 0, std::max(0, viewport.scrollback_rows));
    const int line_delta = target_offset - viewport.offset_from_tail;
    if (line_delta != 0) {
        (void)m_viewport_controller.scroll_lines(line_delta);
    }
}

bool Terminal_session::publish_viewport_snapshot_if_allowed(
    std::uint64_t  sequence,
    QString        message)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_viewport_snapshot_if_allowed");

    if (!m_screen_model.has_value()) {
        return false;
    }

    if (!model_allows_render_snapshot(*m_screen_model)) {
        m_deferred_viewport_changed = true;
        return false;
    }

    Terminal_screen_model_result viewport_result;
    viewport_result.viewport_changed = true;
    m_render_snapshot_model_result = viewport_result;
    publish_render_snapshot(sequence, std::move(message));
    return true;
}

bool Terminal_session::return_viewport_to_tail_after_user_input(std::uint64_t sequence)
{
    if (!m_screen_model.has_value()) {
        return false;
    }

    const int previous_offset = m_viewport_controller.state().offset_from_tail;
    m_viewport_controller.notify_user_input();
    if (previous_offset == 0) {
        return false;
    }

    return publish_viewport_snapshot_if_allowed(
        sequence,
        QStringLiteral("viewport returned to tail"));
}

void Terminal_session::publish_selection_snapshot(
    std::uint64_t  sequence,
    QString        message,
    bool           allow_blocked_selection_only_snapshot)
{
    if (!m_screen_model.has_value()) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral("session publish-selection-snapshot skipped reason=no-screen-model sequence=%1")
                    .arg(static_cast<qulonglong>(sequence)));
        }
        return;
    }

    if (!model_allows_render_snapshot(*m_screen_model)) {
        if (!allow_blocked_selection_only_snapshot) {
            if (selection_trace_requested(m_config.selection_trace_enabled)) {
                write_selection_trace(m_config.selection_trace_enabled,
                    QStringLiteral(
                        "session publish-selection-snapshot deferred reason=publication-blocked sequence=%1")
                        .arg(static_cast<qulonglong>(sequence)));
            }
            return;
        }

#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.render_snapshot_requests;
        }
#endif
        const bool latest_render_snapshot_is_current =
            m_latest_render_snapshot != nullptr &&
            grid_sizes_match(m_latest_render_snapshot->grid_size, m_grid_size);
        Terminal_render_snapshot snapshot =
            latest_render_snapshot_is_current
                ? *m_latest_render_snapshot
            : m_latest_content_render_snapshot != nullptr
                ? geometry_snapshot_from_public_snapshot(
                    *m_latest_content_render_snapshot,
                    m_grid_size,
                    sequence,
                    m_backend_geometry_in_sync,
                    &m_profile_stats)
                : make_empty_render_snapshot(
                    m_grid_size,
                    viewport_adapted_to_grid(m_viewport_controller.state(), m_grid_size),
                    sequence);
        // Visual detaches during synchronized output must clear public spans
        // without basing the snapshot on unpublished held content.
        snapshot.basis                     = Terminal_render_snapshot_basis::LIVE_CONTENT;
        snapshot.purpose                   = Terminal_render_snapshot_purpose::SELECTION_DERIVED;
        snapshot.public_scroll_diagnostics = {};
        snapshot.selection_spans.clear();
        snapshot.dirty_row_ranges = compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        snapshot.metadata.sequence                     = sequence;
        snapshot.metadata.publication_generation       = m_render_snapshot_generation + 1U;
        snapshot.metadata.processed_backend_callback_epoch =
            m_last_processed_backend_callback_epoch;
        snapshot.metadata.backend_geometry_in_sync     = m_backend_geometry_in_sync;
        snapshot.metadata.visual_bell_active           = false;
        snapshot.metadata.mouse_reporting_mode_changed = false;
        snapshot.metadata.row_origin_generation        = m_row_origin_generation;
        m_latest_render_snapshot =
            std::make_shared<const Terminal_render_snapshot>(std::move(snapshot));
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.render_snapshots_constructed;
        }
#endif
        m_unrendered_render_snapshot_dirty_basis = m_latest_render_snapshot;
        record_snapshot_publication_queued_for_bridge(
            m_latest_render_snapshot->metadata.publication_generation);
        ++m_render_snapshot_generation;
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.render_snapshot_publications;
            ++m_profile_stats.full_snapshot_publications;
            ++m_profile_stats.selection_snapshot_publications;
            update_retained_render_snapshot_profile_stats(
                m_profile_stats,
                m_latest_render_snapshot,
                m_latest_content_render_snapshot);
        }
#endif
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral(
                    "session publish-selection-snapshot selection-only sequence=%1 generation=%2 basis={%3}")
                    .arg(static_cast<qulonglong>(sequence))
                    .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                    .arg(selection_trace_content_basis(m_selection_content_basis)));
        }
        record_notification({
            Terminal_session_notification_kind::SNAPSHOT_READY,
            sequence,
            std::move(message),
        });
        return;
    }

    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-selection-snapshot sequence=%1 message=\"%2\" generation=%3 basis={%4}")
                .arg(static_cast<qulonglong>(sequence))
                .arg(message)
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    Terminal_screen_model_result selection_result;
    m_render_snapshot_model_result = selection_result;
    publish_render_snapshot(
        sequence,
        std::move(message),
        Terminal_render_snapshot_purpose::SELECTION_DERIVED);
}

void Terminal_session::advance_selection_content_basis_for_model_result(
    const Terminal_screen_model_result& result,
    const Terminal_viewport_state&      previous_viewport,
    terminal_grid_size_t                previous_grid_size,
    std::optional<Terminal_selection_attachment_resolution_status>
                                        forced_failure,
    std::optional<Terminal_selection_attachment_resolution_status>
                                        forced_drag_failure,
    bool                                cache_drag_success)
{
    Q_UNUSED(previous_viewport);
    const bool content_basis_changed =
        result.terminal_content_changed || result.active_buffer_changed;
    const bool grid_reflow_basis_changed =
        previous_grid_size.columns != m_screen_model->grid_size().columns;
    const bool grid_height_changed =
        previous_grid_size.rows != m_screen_model->grid_size().rows;
    const bool row_origin_changed        = result.evicted_scrollback_rows > 0;
    if (content_basis_changed) {
        ++m_selection_content_basis.content_generation;
    }

    if (grid_reflow_basis_changed) {
        ++m_selection_content_basis.grid_reflow_generation;
    }

    if (row_origin_changed) {
        ++m_row_origin_generation;
    }

    if (!content_basis_changed      &&
        !grid_reflow_basis_changed  &&
        !grid_height_changed        &&
        !row_origin_changed         &&
        !forced_failure.has_value() &&
        !forced_drag_failure.has_value())
    {
        return;
    }

    terminal_selection_source_identity_t target_source;
    target_source.source_content_basis  = m_selection_content_basis;
    target_source.anchor_domain = selection_anchor_domain_for_buffer(
        m_screen_model->active_buffer_id());
    target_source.session_epoch         = m_selection_session_epoch;
    target_source.buffer_id             = m_screen_model->active_buffer_id();
    target_source.grid_reflow_basis     =
        m_selection_content_basis.grid_reflow_generation;
    target_source.row_origin_generation = m_row_origin_generation;
    target_source.grid_size             = m_screen_model->grid_size();
    target_source.viewport_mapping      = m_viewport_controller.state();

    advance_selection_drag_continuity(
        result,
        target_source,
        forced_drag_failure,
        cache_drag_success);

    const std::optional<terminal_selection_visual_lease_t> prior_lease =
        m_selection.visual_lease();
    if (!prior_lease.has_value() ||
        m_selection.internal_state() != Terminal_selection_internal_state::ATTACHED)
    {
        refresh_selection_drag_cached_success_range(target_source);
        return;
    }

    Terminal_selection_attachment_resolution resolution =
        m_screen_model->resolve_selection_attachment(
            *prior_lease,
            target_source,
            result);
    const Terminal_selection_attachment_resolution_status model_resolution_status =
        resolution.status;
    if (forced_failure.has_value()) {
        resolution.status = *forced_failure;
        resolution.translated_lease.reset();
    }
    m_last_selection_attachment_resolution_status = resolution.status;
    if (resolution.status == Terminal_selection_attachment_resolution_status::TRANSLATED &&
        resolution.translated_lease.has_value())
    {
        const Terminal_selection_range translated_range =
            resolution.translated_lease->selected_range;
        m_selection.install_translated_attachment(
            translated_range,
            *resolution.translated_lease);
        m_selection_buffer_id = target_source.buffer_id;
    }
    else {
        m_selection.detach_visual_attachment();
    }
    refresh_selection_drag_cached_success_range(target_source);

    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session attachment-resolution status=%1 model_status=%2 hold_poison=%3 "
                "action=%4 old_rows={%5} final_rows={%6} deltas={%7} capability=%8 "
                "recovery_proposals={%9} recovery_attempts={%10} "
                "lease_before=%11 lease_after=%12")
                .arg(static_cast<int>(resolution.status))
                .arg(static_cast<int>(model_resolution_status))
                .arg(forced_failure.has_value()
                    ? QString::number(static_cast<int>(*forced_failure))
                    : QStringLiteral("none"))
                .arg(resolution.translated_lease.has_value()
                    ? QStringLiteral("atomic-install")
                    : QStringLiteral("detach-payload-only"))
                .arg(selection_trace_int_values(resolution.old_logical_rows))
                .arg(selection_trace_int_values(resolution.final_logical_rows))
                .arg(selection_trace_int_values(resolution.row_deltas))
                .arg(selection_trace_continuity(result.selection_continuity))
                .arg(selection_trace_recovery_proposals(result.recovery_proposals))
                .arg(selection_trace_recovery_attempts(
                    result.recovery_attempts,
                    result.dropped_recovery_attempts))
                .arg(selection_trace_visual_lease(prior_lease))
                .arg(selection_trace_visual_lease(m_selection.visual_lease())));
    }
}

void Terminal_session::advance_selection_drag_continuity(
    const Terminal_screen_model_result&         result,
    const terminal_selection_source_identity_t& target_source,
    std::optional<Terminal_selection_attachment_resolution_status>
                                              forced_failure,
    bool                                      cache_success)
{
    if (!m_selection_drag_continuity.has_value() || !m_screen_model.has_value()) {
        return;
    }

    Selection_drag_continuity& continuity = *m_selection_drag_continuity;
    if (continuity.failure.has_value()) {
        return;
    }
    continuity.cached_success.reset();

    Terminal_selection_attachment_resolution resolution =
        m_screen_model->resolve_selection_attachment(
            continuity.anchor_lease,
            target_source,
            result);
    if (forced_failure.has_value()) {
        resolution.status = *forced_failure;
        resolution.translated_lease.reset();
    }
    if (resolution.status != Terminal_selection_attachment_resolution_status::TRANSLATED ||
        !resolution.translated_lease.has_value())
    {
        continuity.failure = resolution.status;
        return;
    }
    continuity.anchor_lease = *resolution.translated_lease;
    if (cache_success) {
        Selection_drag_continuity::Cached_success success;
        success.anchor_lease = continuity.anchor_lease;
        success.source       = target_source;
        continuity.cached_success = std::move(success);
    }
}

void Terminal_session::refresh_selection_drag_cached_success_range(
    const terminal_selection_source_identity_t& target_source)
{
    if (!m_selection_drag_continuity.has_value()) {
        return;
    }

    Selection_drag_continuity& continuity = *m_selection_drag_continuity;
    if (!continuity.cached_success.has_value()) {
        return;
    }
    Selection_drag_continuity::Cached_success& success =
        *continuity.cached_success;
    if (continuity.failure.has_value() ||
        !selection_source_identities_match(success.source, target_source) ||
        !selection_visual_leases_match(success.anchor_lease, continuity.anchor_lease))
    {
        continuity.cached_success.reset();
        return;
    }

    success.proven_selection_lease.reset();
    if (!continuity.established) {
        return;
    }

    const std::optional<terminal_selection_visual_lease_t>& lease =
        m_selection.visual_lease();
    if (m_selection.internal_state() != Terminal_selection_internal_state::ATTACHED ||
        !lease.has_value() ||
        lease->durable_payload_identity != continuity.durable_payload_identity ||
        lease->provisional_payload_identity != continuity.provisional_payload_identity ||
        lease->selected_range != m_selection.range() ||
        !selection_lease_has_compatible_visual_source(*lease, target_source, true))
    {
        continuity.cached_success.reset();
        return;
    }
    success.proven_selection_lease = *lease;
}

void Terminal_session::begin_synchronized_selection_continuity_hold()
{
    m_synchronized_selection_continuity_hold.reset();
    if (!m_screen_model.has_value()) {
        return;
    }

    Synchronized_selection_continuity_hold hold;
    if (m_selection.internal_state() == Terminal_selection_internal_state::ATTACHED) {
        const std::optional<terminal_selection_visual_lease_t> lease =
            m_selection.visual_lease();
        if (lease.has_value() && !lease->selected_lines.empty()) {
            Synchronized_continuity_track selection_track;
            selection_track.original_lease = *lease;
            selection_track.latest_handles.reserve(lease->selected_lines.size());
            for (const terminal_selection_line_lease_t& line : lease->selected_lines) {
                selection_track.latest_handles.push_back(line.history_handle);
            }
            hold.selection = std::move(selection_track);
        }
    }

    if (m_selection_drag_continuity.has_value() &&
        !m_selection_drag_continuity->failure.has_value())
    {
        Synchronized_continuity_track drag_track;
        drag_track.original_lease = m_selection_drag_continuity->anchor_lease;
        if (!drag_track.original_lease.selected_lines.empty()) {
            drag_track.latest_handles.push_back(
                drag_track.original_lease.selected_lines.front().history_handle);
            hold.drag = std::move(drag_track);
        }
    }

    if (!hold.selection.has_value() && !hold.drag.has_value()) {
        return;
    }

    m_synchronized_selection_continuity_hold = std::move(hold);
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session hold-continuity begin selection_lines=%1 drag_lines=%2 "
                "selection_originals={%3} drag_originals={%4}")
                .arg(m_synchronized_selection_continuity_hold->selection.has_value()
                    ? static_cast<qulonglong>(
                        m_synchronized_selection_continuity_hold->selection->
                            latest_handles.size())
                    : 0U)
                .arg(m_synchronized_selection_continuity_hold->drag.has_value()
                    ? static_cast<qulonglong>(
                        m_synchronized_selection_continuity_hold->drag->latest_handles.size())
                    : 0U)
                .arg(selection_trace_history_handles(
                    m_synchronized_selection_continuity_hold->selection.has_value()
                        ? m_synchronized_selection_continuity_hold->selection->latest_handles
                        : std::vector<terminal_history_handle_t>{}))
                .arg(selection_trace_history_handles(
                    m_synchronized_selection_continuity_hold->drag.has_value()
                        ? m_synchronized_selection_continuity_hold->drag->latest_handles
                        : std::vector<terminal_history_handle_t>{})));
    }
}

void Terminal_session::accumulate_synchronized_selection_continuity(
    const Terminal_screen_model_result& result)
{
    if (!m_synchronized_selection_continuity_hold.has_value() ||
        !m_screen_model.has_value())
    {
        return;
    }

    Synchronized_selection_continuity_hold& hold =
        *m_synchronized_selection_continuity_hold;
    if (hold.selection.has_value()) {
        accumulate_synchronized_continuity_track(
            *hold.selection,
            result,
            "selection");
    }
    if (hold.drag.has_value()) {
        accumulate_synchronized_continuity_track(
            *hold.drag,
            result,
            "drag");
    }
}

void Terminal_session::accumulate_synchronized_continuity_track(
    Synchronized_continuity_track&       track,
    const Terminal_screen_model_result& result,
    const char*                         domain)
{
    const auto trace_hold = [&](QString action) {
        if (!selection_trace_requested(m_config.selection_trace_enabled)) {
            return;
        }
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session hold-continuity accumulate action=%1 domain=%2 "
                "failure=%3 latest={%4} input=%5")
                .arg(std::move(action))
                .arg(QString::fromLatin1(domain))
                .arg(track.failure.has_value()
                    ? QString::number(static_cast<int>(*track.failure))
                    : QStringLiteral("none"))
                .arg(selection_trace_history_handles(track.latest_handles))
                .arg(selection_trace_continuity(result.selection_continuity)));
    };
    if (track.failure.has_value()) {
        trace_hold(QStringLiteral("already-poisoned"));
        return;
    }
    if (track.original_lease.session_epoch != m_selection_session_epoch) {
        track.failure = Terminal_selection_attachment_resolution_status::EPOCH_MISMATCH;
        trace_hold(QStringLiteral("poison-epoch"));
        return;
    }
    if (result.active_buffer_changed ||
        m_screen_model->active_buffer_id() != track.original_lease.buffer_id)
    {
        track.failure = Terminal_selection_attachment_resolution_status::BUFFER_MISMATCH;
        trace_hold(QStringLiteral("poison-buffer"));
        return;
    }
    if (m_screen_model->grid_size().columns != track.original_lease.grid_size.columns) {
        track.failure = Terminal_selection_attachment_resolution_status::GRID_REFLOW_MISMATCH;
        trace_hold(QStringLiteral("poison-reflow"));
        return;
    }
    if (!result.terminal_content_changed &&
        result.evicted_scrollback_rows <= 0 &&
        !result.selection_continuity.has_value())
    {
        trace_hold(QStringLiteral("unaffected"));
        return;
    }

    for (terminal_history_handle_t& handle : track.latest_handles) {
        const Terminal_retained_line_lookup_result current_lookup =
            m_screen_model->retained_line_lookup(track.original_lease.buffer_id, handle);
        if (current_lookup.exact_match &&
            current_lookup.retained_line_id_match_count == 1)
        {
            continue;
        }
        if (current_lookup.retained_line_id_match_count > 1) {
            track.failure = Terminal_selection_attachment_resolution_status::
                DUPLICATE_RESOLUTION;
            trace_hold(QStringLiteral("poison-duplicate-current"));
            return;
        }
        if (current_lookup.retained_line_content_generation_mismatch) {
            track.failure = Terminal_selection_attachment_resolution_status::
                CONTENT_GENERATION_MISMATCH;
            trace_hold(QStringLiteral("poison-generation"));
            return;
        }

        const terminal_selection_continuity_capability_t* continuity =
            result.selection_continuity.has_value()
                ? &*result.selection_continuity
                : nullptr;
        if (continuity == nullptr ||
            continuity->version != k_terminal_selection_continuity_capability_version)
        {
            track.failure = Terminal_selection_attachment_resolution_status::MISSING_LINE;
            trace_hold(QStringLiteral("poison-capability-missing"));
            return;
        }

        const auto successor_it =
            continuity->successors_by_old_retained_line_id.find(handle.row_sequence);
        if (successor_it == continuity->successors_by_old_retained_line_id.end()) {
            track.failure = Terminal_selection_attachment_resolution_status::MISSING_LINE;
            trace_hold(QStringLiteral("poison-successor-key-missing"));
            return;
        }

        const terminal_selection_line_successor_t* successor = nullptr;
        std::size_t                                 match_count = 0U;
        for (const terminal_selection_line_successor_t& candidate : successor_it->second) {
            if (candidate.old_handle == handle) {
                successor = &candidate;
                ++match_count;
            }
        }
        if (match_count != 1U || successor == nullptr) {
            track.failure = match_count > 1U
                ? Terminal_selection_attachment_resolution_status::DUPLICATE_RESOLUTION
                : Terminal_selection_attachment_resolution_status::MISSING_LINE;
            trace_hold(match_count > 1U
                ? QStringLiteral("poison-successor-ambiguous")
                : QStringLiteral("poison-successor-missing"));
            return;
        }

        const Terminal_retained_line_lookup_result final_lookup =
            m_screen_model->retained_line_lookup(
                track.original_lease.buffer_id,
                successor->final_handle);
        if (final_lookup.retained_line_id_match_count > 1) {
            track.failure = Terminal_selection_attachment_resolution_status::
                DUPLICATE_RESOLUTION;
            trace_hold(QStringLiteral("poison-final-duplicate"));
            return;
        }
        if (!final_lookup.exact_match ||
            final_lookup.retained_line_id_match_count != 1)
        {
            track.failure = final_lookup.retained_line_content_generation_mismatch
                ? Terminal_selection_attachment_resolution_status::CONTENT_GENERATION_MISMATCH
                : Terminal_selection_attachment_resolution_status::MISSING_LINE;
            trace_hold(QStringLiteral("poison-final-missing"));
            return;
        }
        handle = successor->final_handle;
    }
    trace_hold(QStringLiteral("updated"));
}

Terminal_screen_model_result Terminal_session::compose_synchronized_selection_release(
    Terminal_screen_model_result result)
{
    if (!m_synchronized_selection_continuity_hold.has_value() ||
        !m_screen_model.has_value())
    {
        return result;
    }

    Synchronized_selection_continuity_hold& hold =
        *m_synchronized_selection_continuity_hold;
    terminal_selection_continuity_capability_t composed;
    bool has_successor = false;
    const auto compose_track = [&](std::optional<Synchronized_continuity_track>& track) {
        if (!track.has_value() || track->failure.has_value()) {
            return;
        }

        const terminal_grid_position_t old_start =
            normalized_selection_start(track->original_lease.selected_range);
        for (std::size_t index = 0U; index < track->latest_handles.size(); ++index) {
            const terminal_history_handle_t& old_handle =
                track->original_lease.selected_lines[index].history_handle;
            const terminal_history_handle_t& final_handle = track->latest_handles[index];
            if (old_handle == final_handle) {
                continue;
            }

            const Terminal_retained_line_lookup_result final_lookup =
                m_screen_model->retained_line_lookup(
                    track->original_lease.buffer_id,
                    final_handle);
            if (!final_lookup.exact_match ||
                final_lookup.retained_line_id_match_count != 1)
            {
                track->failure = final_lookup.retained_line_content_generation_mismatch
                    ? Terminal_selection_attachment_resolution_status::
                        CONTENT_GENERATION_MISMATCH
                    : Terminal_selection_attachment_resolution_status::MISSING_LINE;
                return;
            }

            const int old_logical_row = old_start.row +
                track->original_lease.selected_lines[index].row_offset;
            terminal_selection_line_successor_t successor;
            successor.old_handle        = old_handle;
            successor.final_handle      = final_handle;
            successor.old_logical_row   = old_logical_row;
            successor.final_logical_row = final_lookup.exact_logical_row;
            successor.row_delta         = final_lookup.exact_logical_row - old_logical_row;

            std::vector<terminal_selection_line_successor_t>& candidates =
                composed.successors_by_old_retained_line_id[old_handle.row_sequence];
            const auto same_old = std::find_if(
                candidates.begin(),
                candidates.end(),
                [&](const terminal_selection_line_successor_t& candidate) {
                    return candidate.old_handle == old_handle;
                });
            if (same_old != candidates.end()) {
                if (same_old->final_handle != final_handle) {
                    track->failure = Terminal_selection_attachment_resolution_status::
                        DUPLICATE_RESOLUTION;
                }
                continue;
            }
            candidates.push_back(std::move(successor));
            has_successor = true;
        }
    };

    compose_track(hold.selection);
    compose_track(hold.drag);

    if (has_successor) {
        result.selection_continuity = std::move(composed);
    }
    else
    if ((hold.selection.has_value() && hold.selection->failure.has_value()) ||
        (hold.drag.has_value() && hold.drag->failure.has_value()))
    {
        result.selection_continuity.reset();
    }
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session hold-continuity release action=%1 selection_failure=%2 "
                "drag_failure=%3 selection_latest={%4} drag_latest={%5} output=%6")
                .arg(has_successor
                    ? QStringLiteral("composed")
                    : QStringLiteral("identity-stable"))
                .arg(hold.selection.has_value() && hold.selection->failure.has_value()
                    ? QString::number(static_cast<int>(*hold.selection->failure))
                    : QStringLiteral("none"))
                .arg(hold.drag.has_value() && hold.drag->failure.has_value()
                    ? QString::number(static_cast<int>(*hold.drag->failure))
                    : QStringLiteral("none"))
                .arg(selection_trace_history_handles(
                    hold.selection.has_value()
                        ? hold.selection->latest_handles
                        : std::vector<terminal_history_handle_t>{}))
                .arg(selection_trace_history_handles(
                    hold.drag.has_value()
                        ? hold.drag->latest_handles
                        : std::vector<terminal_history_handle_t>{}))
                .arg(selection_trace_continuity(result.selection_continuity)));
    }
    return result;
}

std::optional<Terminal_selection_attachment_resolution_status>
Terminal_session::synchronized_selection_release_failure() const
{
    if (m_synchronized_selection_continuity_hold.has_value() &&
        m_synchronized_selection_continuity_hold->selection.has_value() &&
        m_synchronized_selection_continuity_hold->selection->failure.has_value())
    {
        return m_synchronized_selection_continuity_hold->selection->failure;
    }

    const std::optional<terminal_selection_visual_lease_t> lease =
        m_selection.visual_lease();
    if (m_selection.internal_state() == Terminal_selection_internal_state::ATTACHED &&
        lease.has_value()                                                          &&
        lease->selected_lines.empty())
    {
        return Terminal_selection_attachment_resolution_status::MISSING_LINE;
    }
    return std::nullopt;
}

void Terminal_session::rebase_synchronized_selection_continuity_hold()
{
    if (!m_synchronized_selection_continuity_hold.has_value()) {
        return;
    }

    const auto rebase_track = [](
        Synchronized_continuity_track&             track,
        const terminal_selection_visual_lease_t& lease)
    {
        track.original_lease = lease;
        track.latest_handles.clear();
        track.latest_handles.reserve(lease.selected_lines.size());
        for (const terminal_selection_line_lease_t& line : lease.selected_lines) {
            track.latest_handles.push_back(line.history_handle);
        }
    };

    Synchronized_selection_continuity_hold& hold =
        *m_synchronized_selection_continuity_hold;
    if (hold.selection.has_value() && !hold.selection->failure.has_value()) {
        const std::optional<terminal_selection_visual_lease_t> lease =
            m_selection.visual_lease();
        if (m_selection.internal_state() == Terminal_selection_internal_state::ATTACHED &&
            lease.has_value())
        {
            rebase_track(*hold.selection, *lease);
        }
        else {
            hold.selection->failure =
                m_last_selection_attachment_resolution_status.value_or(
                    Terminal_selection_attachment_resolution_status::INVALID_LEASE);
        }
    }

    if (hold.drag.has_value() && !hold.drag->failure.has_value()) {
        if (!m_selection_drag_continuity.has_value()) {
            hold.drag->failure =
                Terminal_selection_attachment_resolution_status::INVALID_LEASE;
        }
        else
        if (m_selection_drag_continuity->failure.has_value()) {
            hold.drag->failure = m_selection_drag_continuity->failure;
        }
        else {
            rebase_track(
                *hold.drag,
                m_selection_drag_continuity->anchor_lease);
        }
    }
}

void Terminal_session::reset_synchronized_selection_continuity_hold()
{
    m_synchronized_selection_continuity_hold.reset();
}

void Terminal_session::record_blocked_synchronized_row_origin_change(
    const Terminal_screen_model_result& result)
{
    if (result.evicted_scrollback_rows > 0) {
        m_deferred_synchronized_evicted_scrollback_rows += result.evicted_scrollback_rows;
    }
}

Terminal_screen_model_result Terminal_session::model_result_with_deferred_synchronized_row_origins(
    Terminal_screen_model_result result)
{
    if (m_deferred_synchronized_evicted_scrollback_rows > 0) {
        result.evicted_scrollback_rows += m_deferred_synchronized_evicted_scrollback_rows;
        m_deferred_synchronized_evicted_scrollback_rows = 0;
    }
    return result;
}

void Terminal_session::set_selection_range_from_published_source_locked(
    Terminal_selection_range             range,
    std::optional<terminal_selection_source_identity_t>
                                        expected_source)
{
    if (!m_screen_model.has_value()) {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral("session selection-range ignored reason=no-screen-model range=%1 expected=%2")
                    .arg(selection_trace_range(range))
                    .arg(selection_trace_source_identity(expected_source)));
        }
        return;
    }

    if (public_projection_hold_active()) {
        m_public_viewport_controller.record_selection_mutation_unsupported();
        return;
    }

    const std::optional<terminal_selection_source_identity_t> current_source =
        published_selection_source_identity_unlocked();
    const bool source_matches_snapshot =
        current_source.has_value() &&
        expected_source.has_value() &&
        selection_source_identities_match(*expected_source, *current_source);
    const bool source_has_compatible_content =
        current_source.has_value() &&
        expected_source.has_value() &&
        selection_sources_have_compatible_content(*expected_source, *current_source);

    std::vector<terminal_selection_line_lease_t> selected_lines;
    if (source_matches_snapshot) {
        selected_lines = selection_line_leases_from_snapshot(*m_latest_render_snapshot, range);
    }

    Terminal_selection_result selected_text =
        Terminal_selection_result{Terminal_selection_result_code::INVALID_RANGE, {}};
    if (!selected_lines.empty() && current_source.has_value()) {
        selected_text = m_screen_model->selected_text(
            current_source->buffer_id,
            range,
            std::span<const terminal_selection_line_lease_t>(
                selected_lines.data(),
                selected_lines.size()));
    }
    if (selected_text.code != Terminal_selection_result_code::OK &&
        source_matches_snapshot)
    {
        selected_text = selected_text_from_render_snapshot(*m_latest_render_snapshot, range);
    }

    Terminal_selection_result proven_selected_text = selected_text;
    const bool active_model_source_proven =
        source_has_compatible_content &&
        active_model_matches_published_selection_source(
            *m_screen_model,
            *current_source,
            m_selection_content_basis,
            m_selection_session_epoch,
            m_row_origin_generation,
            m_viewport_controller.state()) &&
        selection_range_is_valid_for_active_model(range);
    if (proven_selected_text.code != Terminal_selection_result_code::OK &&
        active_model_source_proven)
    {
        selected_lines = m_screen_model->selection_line_leases(
            current_source->buffer_id,
            range);
        if (!selected_lines.empty()) {
            proven_selected_text = m_screen_model->selected_text(
                current_source->buffer_id,
                range,
                std::span<const terminal_selection_line_lease_t>(
                    selected_lines.data(),
                    selected_lines.size()));
        }
    }
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session selection-range-proof range=%1 expected=%2 current=%3 "
                "source_match=%4 content_match=%5 active_model_proven=%6 "
                "line_count=%7 selected_result=%8 proven_result=%9 mismatch=%10")
                .arg(selection_trace_range(range))
                .arg(selection_trace_source_identity(expected_source))
                .arg(selection_trace_source_identity(current_source))
                .arg(selection_trace_bool(source_matches_snapshot))
                .arg(selection_trace_bool(source_has_compatible_content))
                .arg(selection_trace_bool(active_model_source_proven))
                .arg(static_cast<qulonglong>(selected_lines.size()))
                .arg(static_cast<int>(selected_text.code))
                .arg(static_cast<int>(proven_selected_text.code))
                .arg(current_source.has_value() && expected_source.has_value()
                    ? selection_trace_source_mismatch_reason(*expected_source, *current_source)
                    : QStringLiteral("source-missing")));
    }

    if (proven_selected_text.code != Terminal_selection_result_code::OK) {
        if (m_selection.has_internal_selection()) {
            if (selection_trace_requested(m_config.selection_trace_enabled)) {
                write_selection_trace(m_config.selection_trace_enabled,
                    QStringLiteral(
                        "session selection-range-failed action=clear range=%1 result=%2 current=%3")
                        .arg(selection_trace_range(range))
                        .arg(static_cast<int>(proven_selected_text.code))
                        .arg(selection_trace_source_identity(current_source)));
            }
            m_selection.clear();
            reset_synchronized_selection_continuity_hold();
            m_selection_buffer_id = current_source.has_value()
                ? current_source->buffer_id
                : m_screen_model->active_buffer_id();
            publish_selection_snapshot(next_sequence(), QStringLiteral("selection cleared"));
        }
        else {
            if (selection_trace_requested(m_config.selection_trace_enabled)) {
                write_selection_trace(m_config.selection_trace_enabled,
                    QStringLiteral(
                        "session selection-range-failed action=none range=%1 result=%2 current=%3")
                        .arg(selection_trace_range(range))
                        .arg(static_cast<int>(proven_selected_text.code))
                        .arg(selection_trace_source_identity(current_source)));
            }
        }
        return;
    }

    const std::optional<terminal_selection_visual_lease_t>& visual_lease =
        m_selection.visual_lease();
    if (m_selection.has_selection()                             &&
        m_selection.range()        == range                     &&
        m_selection_buffer_id      == current_source->buffer_id &&
        m_selection.has_cached_selected_text()                  &&
        visual_lease.has_value()                                &&
        selection_lease_matches_source(*visual_lease, *current_source))
    {
        if (selection_trace_requested(m_config.selection_trace_enabled)) {
            write_selection_trace(m_config.selection_trace_enabled,
                QStringLiteral("session selection-range-noop range=%1 current=%2")
                    .arg(selection_trace_range(range))
                    .arg(selection_trace_source_identity(current_source)));
        }
        return;
    }

    terminal_selection_visual_lease_t lease =
        make_selection_visual_lease(range, *current_source);
    lease.selected_lines = std::move(selected_lines);

    m_selection.set_range(range, proven_selected_text.text, std::move(lease));
    m_selection_buffer_id = current_source->buffer_id;
    if (!model_allows_render_snapshot(*m_screen_model)) {
        begin_synchronized_selection_continuity_hold();
    }
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral("session selection-changed range=%1 size=%2 current=%3 basis={%4}")
                .arg(selection_trace_range(range))
                .arg(proven_selected_text.text.size())
                .arg(selection_trace_source_identity(current_source))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    publish_selection_snapshot(next_sequence(), QStringLiteral("selection changed"));
}

terminal_selection_visual_lease_t Terminal_session::make_selection_visual_lease(
    Terminal_selection_range range) const
{
    terminal_selection_visual_lease_t lease;
    lease.source_content_basis = m_selection_content_basis;
    lease.anchor_domain        =
        selection_anchor_domain_for_buffer(m_screen_model->active_buffer_id());
    lease.session_epoch        = m_selection_session_epoch;
    lease.buffer_id            = m_screen_model->active_buffer_id();
    lease.grid_reflow_basis    = m_selection_content_basis.grid_reflow_generation;
    lease.row_origin_generation = m_row_origin_generation;
    lease.grid_size            = m_screen_model->grid_size();
    lease.viewport_mapping     = m_viewport_controller.state();
    lease.selected_range       = range;
    lease.anchor               = range.start;
    lease.extent               = range.end;
    return lease;
}

terminal_selection_visual_lease_t Terminal_session::make_selection_visual_lease(
    Terminal_selection_range range,
    const terminal_selection_source_identity_t& source) const
{
    terminal_selection_visual_lease_t lease;
    lease.source_content_basis = source.source_content_basis;
    lease.anchor_domain        = source.anchor_domain;
    lease.session_epoch        = source.session_epoch;
    lease.buffer_id            = source.buffer_id;
    lease.grid_reflow_basis    = source.grid_reflow_basis;
    lease.row_origin_generation = source.row_origin_generation;
    lease.grid_size            = source.grid_size;
    lease.viewport_mapping     = source.viewport_mapping;
    lease.selected_range       = range;
    lease.anchor               = range.start;
    lease.extent               = range.end;
    return lease;
}

Terminal_render_snapshot_request Terminal_session::make_render_snapshot_request(
    std::uint64_t                      sequence,
    Terminal_render_snapshot_purpose   purpose,
    Terminal_public_scroll_diagnostics public_scroll_diagnostics) const
{
    Terminal_render_snapshot_request request;
    request.sequence                         = sequence;
    request.publication_generation           = m_render_snapshot_generation + 1U;
    request.processed_backend_callback_epoch =
        m_last_processed_backend_callback_epoch;
    request.basis                            = Terminal_render_snapshot_basis::LIVE_CONTENT;
    request.row_origin_generation            = m_row_origin_generation;
    request.purpose                          = purpose;
    request.public_scroll_diagnostics        = public_scroll_diagnostics;
    request.viewport                         = m_viewport_controller.state();
    request.backend_geometry_in_sync         = m_backend_geometry_in_sync;
    request.visual_bell_active               = m_visual_bell_active;
    request.ime_preedit                      = m_ime_preedit;
    if (m_render_snapshot_model_result.has_value()) {
        request.dirty_rows       = m_render_snapshot_model_result->dirty_rows;
        request.viewport_changed = m_render_snapshot_model_result->viewport_changed;
        request.mouse_reporting_mode_changed =
            m_render_snapshot_model_result->mouse_reporting_mode_changed;
    }
    request.viewport_changed =
        request.viewport_changed ||
        m_deferred_viewport_changed ||
        (purpose == Terminal_render_snapshot_purpose::CONTENT &&
            m_latest_render_snapshot != nullptr &&
            snapshot_is_public_projection_scroll(*m_latest_render_snapshot));
    const std::optional<terminal_selection_visual_lease_t>& visual_lease =
        m_selection.visual_lease();
    bool selection_requested = false;
    if (m_selection.has_internal_selection()                                       &&
        selection_state_allows_span_emission(m_selection.internal_state())         &&
        visual_lease.has_value()                                                   &&
        m_selection_buffer_id == m_screen_model->active_buffer_id()                &&
        selection_range_is_valid_for_active_model(m_selection.range()))
    {
        terminal_selection_source_identity_t source;
        source.source_content_basis = m_selection_content_basis;
        source.anchor_domain        =
            selection_anchor_domain_for_buffer(m_screen_model->active_buffer_id());
        source.session_epoch        = m_selection_session_epoch;
        source.buffer_id            = m_screen_model->active_buffer_id();
        source.grid_reflow_basis    = m_selection_content_basis.grid_reflow_generation;
        source.row_origin_generation = m_row_origin_generation;
        source.grid_size            = m_screen_model->grid_size();
        source.viewport_mapping     = m_viewport_controller.state();

        if (selection_lease_has_compatible_visual_source(
                *visual_lease,
                source,
                m_config.selection_viewport_projection_enabled)                 &&
            visual_lease->selected_range == m_selection.range()                 &&
            visual_lease->durable_payload_identity ==
                m_selection.durable_payload_identity()                          &&
            visual_lease->provisional_payload_identity ==
                m_selection.provisional_payload_identity())
        {
            request.selections.push_back({
                m_selection.range(),
                visual_lease->selected_lines,
            });
            selection_requested = true;
        }
    }
    if (selection_trace_requested(m_config.selection_trace_enabled) &&
        !selection_requested                                      &&
        m_selection.has_internal_selection())
    {
        const bool has_internal_selection = m_selection.has_internal_selection();
        const bool state_allows_span =
            has_internal_selection &&
            selection_state_allows_span_emission(m_selection.internal_state());
        const bool selection_buffer_matches =
            has_internal_selection &&
            m_selection_buffer_id == m_screen_model->active_buffer_id();
        const bool active_model_range_valid =
            has_internal_selection &&
            selection_range_is_valid_for_active_model(m_selection.range());

        bool lease_visual_source_match = false;
        bool range_match               = false;
        bool durable_identity_match    = false;
        bool provisional_identity_match = false;
        if (has_internal_selection && visual_lease.has_value()) {
            terminal_selection_source_identity_t source;
            source.source_content_basis = m_selection_content_basis;
            source.anchor_domain        =
                selection_anchor_domain_for_buffer(m_screen_model->active_buffer_id());
            source.session_epoch        = m_selection_session_epoch;
            source.buffer_id            = m_screen_model->active_buffer_id();
            source.grid_reflow_basis    = m_selection_content_basis.grid_reflow_generation;
            source.row_origin_generation = m_row_origin_generation;
            source.grid_size            = m_screen_model->grid_size();
            source.viewport_mapping     = m_viewport_controller.state();

            lease_visual_source_match =
                selection_lease_has_compatible_visual_source(
                    *visual_lease,
                    source,
                    m_config.selection_viewport_projection_enabled);
            range_match =
                visual_lease->selected_range == m_selection.range();
            durable_identity_match =
                visual_lease->durable_payload_identity == m_selection.durable_payload_identity();
            provisional_identity_match =
                visual_lease->provisional_payload_identity ==
                    m_selection.provisional_payload_identity();
        }

        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session render-request-selection-suppressed sequence=%1 "
                "has_internal=%2 internal_state=%3 state_allows_span=%4 "
                "visual_lease=%5 selection_buffer_matches=%6 active_model_range_valid=%7 "
                "lease_visual_source_match=%8 range_match=%9 durable_match=%10 "
                "provisional_match=%11 range=%12 lease=%13")
                .arg(static_cast<qulonglong>(sequence))
                .arg(selection_trace_bool(has_internal_selection))
                .arg(has_internal_selection
                    ? QString::number(static_cast<int>(m_selection.internal_state()))
                    : QStringLiteral("none"))
                .arg(selection_trace_bool(state_allows_span))
                .arg(selection_trace_bool(visual_lease.has_value()))
                .arg(selection_trace_bool(selection_buffer_matches))
                .arg(selection_trace_bool(active_model_range_valid))
                .arg(selection_trace_bool(lease_visual_source_match))
                .arg(selection_trace_bool(range_match))
                .arg(selection_trace_bool(durable_identity_match))
                .arg(selection_trace_bool(provisional_identity_match))
                .arg(has_internal_selection
                    ? selection_trace_range(m_selection.range())
                    : QStringLiteral("none"))
                .arg(selection_trace_visual_lease(visual_lease)));
    }
    return request;
}

void Terminal_session::record_snapshot_publication_queued_for_bridge(
    std::uint64_t publication_generation)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!m_profile_stats.enabled) {
        return;
    }

    if (m_render_snapshot_rendered_generation < m_render_snapshot_generation) {
        ++m_profile_stats.snapshots_superseded_before_render;
    }

    const std::uint64_t pending_generations =
        publication_generation - m_render_snapshot_rendered_generation;
    m_profile_stats.max_unrendered_snapshot_generations =
        std::max(
            m_profile_stats.max_unrendered_snapshot_generations,
            pending_generations);
#endif
}

Terminal_session_result Terminal_session::finalize_accepted_text_input_result(
    Terminal_session_result        result,
    std::uint64_t                  sequence,
    User_write_viewport_policy     viewport_policy)
{
    if (result.code != Terminal_session_result_code::ACCEPTED) {
        return result;
    }

    if (viewport_policy == User_write_viewport_policy::RETURN_TO_TAIL) {
        return_viewport_to_tail_after_user_input(sequence);
    }
    return result;
}

bool Terminal_session::selection_range_is_valid_for_active_model(
    const Terminal_selection_range& range) const
{
    Q_ASSERT(m_screen_model.has_value());

    const terminal_grid_size_t grid          = m_screen_model->grid_size();
    const Terminal_buffer_id   active_buffer = m_screen_model->active_buffer_id();

    const int max_row =
        active_buffer == Terminal_buffer_id::ALTERNATE
            ? grid.rows - 1
            : m_screen_model->scrollback_size() + grid.rows - 1;
    const auto position_is_valid = [grid, max_row](terminal_grid_position_t position) {
        return
            position.row    >= 0       &&
            position.row    <= max_row &&
            position.column >= 0       &&
            position.column <= grid.columns;
    };

    return
        range.mode != Terminal_selection_mode::NONE &&
        position_is_valid(range.start)              &&
        position_is_valid(range.end);
}

bool Terminal_session::public_projection_hold_active() const
{
    return
        m_screen_model.has_value()                                  &&
        !model_allows_render_snapshot(*m_screen_model)              &&
        m_public_viewport_controller.has_public_viewport();
}

Terminal_synchronized_output_scroll_policy
Terminal_session::effective_synchronized_output_scroll_policy() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_synchronized_output_hold_policy.value_or(
        m_config.synchronized_output_scroll_policy);
}

Terminal_synchronized_output_policy_change_event
Terminal_session::synchronized_output_policy_change_event() const
{
    return m_synchronized_output_policy_change_event;
}

bool Terminal_session::immediate_public_projection_policy_enabled() const
{
    return
        effective_synchronized_output_scroll_policy() ==
            Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
}

void Terminal_session::latch_synchronized_output_scroll_policy_for_new_hold()
{
    if (m_synchronized_output_hold_policy.has_value()) {
        return;
    }

    m_synchronized_output_hold_policy =
        m_config.synchronized_output_scroll_policy;
    m_synchronized_output_policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
}

void Terminal_session::reset_synchronized_output_policy_lifecycle()
{
    m_synchronized_output_hold_policy.reset();
    m_synchronized_output_policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
}

bool Terminal_session::capture_public_projection_from_latest_content_basis()
{
    if (m_latest_content_render_snapshot == nullptr ||
        !render_snapshot_can_advance_latest_content_snapshot(*m_latest_content_render_snapshot))
    {
        return false;
    }

    Terminal_render_snapshot safe_basis = *m_latest_content_render_snapshot;
    if (m_latest_render_snapshot != nullptr &&
        selection_snapshot_matches_safe_content_basis(*m_latest_render_snapshot, safe_basis))
    {
        safe_basis.selection_spans = m_latest_render_snapshot->selection_spans;
    }

    if (safe_basis.viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
        Terminal_public_projection unsupported_projection =
            Terminal_public_projection::capture_from_safe_model(
                m_next_public_projection_generation,
                safe_basis,
                m_latest_content_render_snapshot_content_basis,
                m_active_buffer_epoch);
        m_public_viewport_controller.initialize_from_projection(unsupported_projection);
        m_public_viewport_controller.invalidate(
            Terminal_public_projection_disable_reason::UNSUPPORTED_BUFFER);
        m_public_projection.reset();
        ++m_next_public_projection_generation;
        if (m_next_public_projection_generation == 0U) {
            m_next_public_projection_generation = 1U;
        }
        return false;
    }

    if (!m_screen_model.has_value()) {
        return false;
    }

    m_public_projection =
        Terminal_public_projection::capture_primary_full_rows_from_safe_model(
            m_next_public_projection_generation,
            safe_basis,
            m_latest_content_render_snapshot_content_basis,
            m_active_buffer_epoch,
            *m_screen_model);
    m_public_viewport_controller.initialize_from_projection(*m_public_projection);
    ++m_next_public_projection_generation;
    if (m_next_public_projection_generation == 0U) {
        m_next_public_projection_generation = 1U;
    }
    return true;
}

bool Terminal_session::capture_search_projection_from_safe_basis(
    const Terminal_render_snapshot&     safe_basis,
    terminal_selection_content_basis_t  content_basis)
{
    if (!m_screen_model.has_value() ||
        validate_render_snapshot(safe_basis).status != Terminal_render_snapshot_status::OK)
    {
        return false;
    }

    Terminal_public_projection projection;
    if (safe_basis.viewport.active_buffer == Terminal_buffer_id::PRIMARY) {
        projection =
            Terminal_public_projection::capture_primary_full_rows_from_safe_model(
                m_next_search_projection_generation,
                safe_basis,
                content_basis,
                m_active_buffer_epoch,
                *m_screen_model);
        if (projection.rows_are_safe_basis_viewport_only()) {
            return false;
        }
    }
    else {
        projection = Terminal_public_projection::capture_from_safe_model(
            m_next_search_projection_generation,
            safe_basis,
            content_basis,
            m_active_buffer_epoch);
        if (projection.rows().size() !=
            static_cast<std::size_t>(safe_basis.grid_size.rows))
        {
            return false;
        }
    }

    m_search_projection = std::move(projection);
    ++m_next_search_projection_generation;
    if (m_next_search_projection_generation == 0U) {
        m_next_search_projection_generation = 1U;
    }
    return true;
}

bool Terminal_session::refresh_search_from_current_public_source()
{
    if (m_search.query().isEmpty()) {
        return false;
    }

    const bool publication_blocked =
        m_screen_model.has_value() && !model_allows_render_snapshot(*m_screen_model);
    if (publication_blocked) {
        const auto source_is_complete = [](const Terminal_public_projection& source) {
            return source.active_buffer() == Terminal_buffer_id::ALTERNATE ||
                !source.rows_are_safe_basis_viewport_only();
        };
        if (!m_search_projection.has_value() &&
            m_public_projection.has_value() &&
            source_is_complete(*m_public_projection))
        {
            m_search_projection = *m_public_projection;
        }
        if (!m_search_projection.has_value() ||
            !source_is_complete(*m_search_projection))
        {
            m_search_projection.reset();
            m_search.set_source_unavailable(m_search.query());
            return false;
        }
    }
    else {
        if (m_latest_content_render_snapshot == nullptr ||
            !capture_search_projection_from_safe_basis(
                *m_latest_content_render_snapshot,
                m_latest_content_render_snapshot_content_basis))
        {
            invalidate_search_projection();
            return false;
        }
    }

    const Terminal_viewport_state preferred_viewport = m_latest_render_snapshot != nullptr
        ? m_latest_render_snapshot->viewport
        : m_search_projection->viewport();
    m_search.rebuild(
        m_search.query(),
        *m_search_projection,
        first_public_row_for_viewport(preferred_viewport));
    return true;
}

void Terminal_session::apply_search_matches_to_snapshot(
    Terminal_render_snapshot& snapshot,
    std::uint64_t             active_buffer_epoch) const
{
    snapshot.search_match_spans =
        m_search.spans_for_snapshot(snapshot, active_buffer_epoch);
    suppress_search_match_spans_without_valid_line_provenance(snapshot);
}

bool Terminal_session::reveal_current_search_match(QString message)
{
    const terminal_search_match_t* const match = m_search.current_match();
    if (match == nullptr || !m_screen_model.has_value()) {
        return false;
    }

    const bool publication_blocked = !model_allows_render_snapshot(*m_screen_model);
    if (publication_blocked) {
        if (!m_search_projection.has_value()) {
            return false;
        }
        if (m_search_projection->active_buffer() == Terminal_buffer_id::ALTERNATE) {
            return publish_search_overlay_from_latest_public_snapshot(std::move(message));
        }

        if (!m_public_projection.has_value() ||
            m_public_projection->generation() != m_search_projection->generation())
        {
            m_public_projection = *m_search_projection;
            m_public_viewport_controller.initialize_from_projection(*m_public_projection);
        }

        const Terminal_viewport_state viewport_before =
            m_public_viewport_controller.viewport();
        const int first_visible_row = first_public_row_for_viewport(viewport_before);
        int target_offset = viewport_before.offset_from_tail;
        if (match->public_row < first_visible_row) {
            target_offset = viewport_before.scrollback_rows -
                static_cast<int>(match->public_row);
        }
        else
        if (match->public_row >=
            first_visible_row + viewport_before.visible_rows)
        {
            const std::int64_t target_first_row =
                match->public_row - viewport_before.visible_rows + 1;
            target_offset = viewport_before.scrollback_rows -
                static_cast<int>(target_first_row);
        }

        const Terminal_public_viewport_controller controller_before =
            m_public_viewport_controller;
        const Terminal_public_viewport_scroll_result scroll_result =
            m_public_viewport_controller.scroll_to_offset_from_tail(target_offset);
        const bool viewport_moved =
            scroll_result.viewport_result.action ==
                Terminal_viewport_scroll_action::VIEWPORT_MOVED;
        if (!publish_public_projection_scroll_snapshot(
                next_sequence(),
                std::move(message),
                viewport_before,
                m_public_viewport_controller.viewport(),
                viewport_moved))
        {
            m_public_viewport_controller = controller_before;
            return false;
        }
        return true;
    }

    if (match->identity.active_buffer_epoch != m_active_buffer_epoch ||
        match->identity.buffer_id != m_screen_model->active_buffer_id())
    {
        invalidate_search_projection();
        return false;
    }

    Terminal_screen_model_result search_result;
    if (match->identity.buffer_id == Terminal_buffer_id::PRIMARY) {
        const Terminal_viewport_state viewport_before = m_viewport_controller.state();
        const int first_visible_row = first_public_row_for_viewport(viewport_before);
        int target_offset = viewport_before.offset_from_tail;
        if (match->public_row < first_visible_row) {
            target_offset = viewport_before.scrollback_rows -
                static_cast<int>(match->public_row);
        }
        else
        if (match->public_row >=
            first_visible_row + viewport_before.visible_rows)
        {
            const std::int64_t target_first_row =
                match->public_row - viewport_before.visible_rows + 1;
            target_offset = viewport_before.scrollback_rows -
                static_cast<int>(target_first_row);
        }

        const int bounded_target = std::clamp(
            target_offset,
            0,
            std::max(0, viewport_before.scrollback_rows));
        const int line_delta = bounded_target - viewport_before.offset_from_tail;
        if (line_delta != 0) {
            const Terminal_viewport_scroll_result scroll_result =
                m_viewport_controller.scroll_lines(line_delta);
            search_result.viewport_changed =
                scroll_result.action == Terminal_viewport_scroll_action::VIEWPORT_MOVED;
        }
    }

    m_render_snapshot_model_result = search_result;
    const std::uint64_t generation_before = m_render_snapshot_generation;
    publish_render_snapshot(
        next_sequence(),
        std::move(message),
        Terminal_render_snapshot_purpose::SEARCH_DERIVED);
    return m_render_snapshot_generation != generation_before;
}

bool Terminal_session::publish_search_overlay_from_latest_public_snapshot(
    QString message)
{
    if (m_latest_render_snapshot == nullptr) {
        return false;
    }

    Terminal_render_snapshot snapshot = *m_latest_render_snapshot;
    if (snapshot.basis == Terminal_render_snapshot_basis::LIVE_CONTENT) {
        snapshot.purpose = Terminal_render_snapshot_purpose::SEARCH_DERIVED;
        snapshot.dirty_row_ranges.clear();
    }
    snapshot.metadata.sequence                = next_sequence();
    snapshot.metadata.publication_generation = m_render_snapshot_generation + 1U;
    const std::uint64_t source_active_buffer_epoch = m_search_projection.has_value()
        ? m_search_projection->active_buffer_epoch()
        : m_active_buffer_epoch;
    apply_search_matches_to_snapshot(snapshot, source_active_buffer_epoch);
    if (validate_render_snapshot(snapshot).status != Terminal_render_snapshot_status::OK) {
        return false;
    }

    std::shared_ptr<const Terminal_render_snapshot> snapshot_handle =
        std::make_shared<const Terminal_render_snapshot>(std::move(snapshot));
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_snapshot(
            snapshot_handle->metadata.sequence,
            message,
            *snapshot_handle);
    }
#endif
    m_latest_render_snapshot = snapshot_handle;
    m_unrendered_render_snapshot_dirty_basis = snapshot_handle;
    record_snapshot_publication_queued_for_bridge(
        snapshot_handle->metadata.publication_generation);
    ++m_render_snapshot_generation;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_publications;
        ++m_profile_stats.full_snapshot_publications;
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#endif
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        snapshot_handle->metadata.sequence,
        std::move(message),
    });
    return true;
}

bool Terminal_session::publish_search_derived_snapshot(QString message)
{
    if (!m_screen_model.has_value()) {
        return false;
    }
    if (!model_allows_render_snapshot(*m_screen_model)) {
        return publish_search_overlay_from_latest_public_snapshot(std::move(message));
    }

    Terminal_screen_model_result search_result;
    m_render_snapshot_model_result = search_result;
    const std::uint64_t generation_before = m_render_snapshot_generation;
    publish_render_snapshot(
        next_sequence(),
        std::move(message),
        Terminal_render_snapshot_purpose::SEARCH_DERIVED);
    return m_render_snapshot_generation != generation_before;
}

void Terminal_session::invalidate_search_projection()
{
    m_search_projection.reset();
    if (!m_search.query().isEmpty()) {
        m_search.set_source_unavailable(m_search.query());
    }
}

std::optional<Terminal_public_scroll_diagnostics>
Terminal_session::reconcile_public_projection_release(
    const Terminal_public_release_intent& release_intent,
    Terminal_viewport_state               live_viewport_before_on_release)
{
    if (!m_screen_model.has_value() || !release_intent.has_public_viewport) {
        return std::nullopt;
    }

    Terminal_public_scroll_diagnostics diagnostics;
    diagnostics.effective_policy = effective_synchronized_output_scroll_policy();
    diagnostics.policy_change_event = synchronized_output_policy_change_event();
    diagnostics.diagnostic_reason = release_intent.diagnostic_reason;
    if (diagnostics.policy_change_event ==
            Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD &&
        diagnostics.diagnostic_reason == Terminal_public_scroll_diagnostic_reason::NONE)
    {
        diagnostics.diagnostic_reason =
            Terminal_public_scroll_diagnostic_reason::
                SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD;
    }
    diagnostics.public_projection_generation =
        release_intent.public_projection_generation;
    diagnostics.public_viewport_before = release_intent.public_viewport;
    diagnostics.public_viewport_after  = release_intent.public_viewport;
    diagnostics.live_viewport_before_on_release = live_viewport_before_on_release;
    diagnostics.live_content_publication_blocked = true;
    diagnostics.hidden_row_eligibility = release_intent.hidden_row_eligibility;
    diagnostics.hidden_row_clamp_reason = release_intent.hidden_row_clamp_reason;
    diagnostics.public_projection_disable_reason =
        release_intent.public_projection_disable_reason;

    const auto set_diagnostic_if_none = [&](
        Terminal_public_scroll_diagnostic_reason reason)
    {
        if (diagnostics.diagnostic_reason == Terminal_public_scroll_diagnostic_reason::NONE) {
            diagnostics.diagnostic_reason = reason;
        }
    };
    const auto set_target_offset = [&](int target_offset) -> std::optional<int> {
        const Terminal_viewport_state viewport = m_viewport_controller.state();
        if (viewport.active_buffer != Terminal_buffer_id::PRIMARY) {
            return std::nullopt;
        }

        const int clamped_target =
            std::clamp(target_offset, 0, std::max(0, viewport.scrollback_rows));
        if (clamped_target != target_offset) {
            diagnostics.hidden_row_clamp_reason =
                Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY;
        }
        const int line_delta = clamped_target - viewport.offset_from_tail;
        if (line_delta != 0) {
            (void)m_viewport_controller.scroll_lines(line_delta);
        }
        return clamped_target;
    };
    const auto offset_for_logical_row = [&](
        int logical_row,
        int anchor_viewport_row)
    {
        const int target_first_visible_logical_row =
            logical_row - anchor_viewport_row;
        return live_viewport_before_on_release.scrollback_rows -
            target_first_visible_logical_row;
    };
    const auto record_incompatible_buffer = [&](
        Terminal_public_scroll_diagnostic_reason reason)
    {
        diagnostics.diagnostic_reason = reason;
        diagnostics.release_reconciliation_result =
            Terminal_release_reconciliation_result::INCOMPATIBLE_BUFFER;
    };
    const auto apply_deferred_offset = [&]() {
        if (!release_intent.deferred_intent_recorded ||
            !release_intent.deferred_offset_from_tail.has_value())
        {
            return false;
        }

        const std::optional<int> clamped_target =
            set_target_offset(*release_intent.deferred_offset_from_tail);
        if (!clamped_target.has_value()) {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED);
            return true;
        }

        diagnostics.release_reconciliation_result =
            *clamped_target >= live_viewport_before_on_release.scrollback_rows
                ? Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE
                : Terminal_release_reconciliation_result::DEFERRED_OFFSET;
        return true;
    };
    const auto apply_public_viewport_offset_fallback = [&]() {
        const std::optional<int> clamped_target =
            set_target_offset(release_intent.public_viewport.offset_from_tail);
        if (!clamped_target.has_value()) {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED);
            return;
        }

        set_diagnostic_if_none(
            Terminal_public_scroll_diagnostic_reason::
                PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT);
        diagnostics.release_reconciliation_result =
            *clamped_target >= live_viewport_before_on_release.scrollback_rows
                ? Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE
                : Terminal_release_reconciliation_result::DEFERRED_OFFSET;
        diagnostics.hidden_row_eligibility =
            Terminal_hidden_row_eligibility::INELIGIBLE;
    };

    if (release_intent.sticky_tail) {
        if (release_intent.public_viewport.active_buffer !=
            live_viewport_before_on_release.active_buffer)
        {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED);
        }
        else
        if (release_intent.active_buffer_epoch != m_active_buffer_epoch) {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::SCREEN_BUFFER_EPOCH_CHANGED);
        }
        else {
            m_viewport_controller.return_to_tail();
            diagnostics.release_reconciliation_result =
                Terminal_release_reconciliation_result::STICKY_TAIL;
        }
    }
    else
    if (release_intent.detached_anchor.has_value()) {
        const Terminal_public_viewport_anchor& anchor =
            *release_intent.detached_anchor;
        const bool buffer_compatible =
            anchor.active_buffer == live_viewport_before_on_release.active_buffer;
        const bool epoch_compatible =
            anchor.active_buffer_epoch == m_active_buffer_epoch;
        const bool geometry_compatible =
            m_latest_content_render_snapshot != nullptr &&
            !release_intent.geometry_invalidated &&
            anchor.geometry_generation ==
                m_selection_content_basis.grid_reflow_generation &&
            grid_sizes_match(
                m_latest_content_render_snapshot->grid_size,
                m_screen_model->grid_size());

        if (!buffer_compatible) {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED);
        }
        else
        if (!epoch_compatible) {
            record_incompatible_buffer(
                    Terminal_public_scroll_diagnostic_reason::SCREEN_BUFFER_EPOCH_CHANGED);
        }
        else
        if (!anchor.visual_fragment_index_is_exact) {
            apply_public_viewport_offset_fallback();
        }
        else {
            Terminal_retained_line_lookup_result lookup;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_session::reconcile_public_projection_release::retained_line_lookup");
                lookup = m_screen_model->retained_line_lookup(
                    anchor.active_buffer,
                    anchor.history_handle);
            }
            if (epoch_compatible && geometry_compatible && lookup.exact_match) {
                (void)set_target_offset(
                    offset_for_logical_row(
                        lookup.exact_logical_row +
                            std::max(0, anchor.visual_fragment_index),
                        anchor.viewport_row));
                diagnostics.release_reconciliation_result =
                    Terminal_release_reconciliation_result::EXACT_ANCHOR;
                diagnostics.hidden_row_eligibility =
                    Terminal_hidden_row_eligibility::ELIGIBLE;
            }
            else
            if (!geometry_compatible && lookup.exact_match) {
                diagnostics.diagnostic_reason =
                    Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_GEOMETRY_CHANGED;
                (void)set_target_offset(
                    offset_for_logical_row(
                        lookup.exact_logical_row +
                            std::max(0, anchor.visual_fragment_index),
                        anchor.viewport_row));
                diagnostics.release_reconciliation_result =
                    Terminal_release_reconciliation_result::RETAINED_ID_BEST_EFFORT;
                diagnostics.hidden_row_eligibility =
                    Terminal_hidden_row_eligibility::ELIGIBLE;
            }
            else {
                if (lookup.retained_line_content_generation_mismatch) {
                    set_diagnostic_if_none(
                        Terminal_public_scroll_diagnostic_reason::
                            DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED);
                }
                else
                if (!lookup.retained_line_id_found) {
                    set_diagnostic_if_none(
                        Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_NOT_RETAINED);
                }

            if (lookup.nearest_successor) {
                (void)set_target_offset(
                    offset_for_logical_row(
                        lookup.nearest_successor_logical_row,
                        anchor.viewport_row));
                diagnostics.release_reconciliation_result =
                    Terminal_release_reconciliation_result::NEAREST_SUCCESSOR;
                diagnostics.hidden_row_eligibility =
                    Terminal_hidden_row_eligibility::ELIGIBLE;
            }
            else
            if (lookup.nearest_predecessor) {
                (void)set_target_offset(
                    offset_for_logical_row(
                        lookup.nearest_predecessor_logical_row,
                        anchor.viewport_row));
                diagnostics.release_reconciliation_result =
                    Terminal_release_reconciliation_result::NEAREST_PREDECESSOR;
                diagnostics.hidden_row_eligibility =
                    Terminal_hidden_row_eligibility::ELIGIBLE;
            }
            else {
                set_diagnostic_if_none(
                    Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_NOT_RETAINED);
                (void)set_target_offset(live_viewport_before_on_release.scrollback_rows);
                diagnostics.release_reconciliation_result =
                    Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE;
                diagnostics.hidden_row_eligibility =
                    Terminal_hidden_row_eligibility::INELIGIBLE;
                diagnostics.hidden_row_clamp_reason =
                    Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY;
            }
            }
        }
    }
    else
    if (!apply_deferred_offset()) {
        if (live_viewport_before_on_release.active_buffer == Terminal_buffer_id::PRIMARY) {
            (void)set_target_offset(live_viewport_before_on_release.scrollback_rows);
            diagnostics.release_reconciliation_result =
                Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE;
            diagnostics.hidden_row_clamp_reason =
                Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY;
        }
        else {
            record_incompatible_buffer(
                Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED);
        }
    }

    diagnostics.live_viewport_after_on_release = m_viewport_controller.state();
    diagnostics.public_viewport_after = diagnostics.live_viewport_after_on_release;
    diagnostics.visible_scroll_applied =
        !viewport_states_match(
            diagnostics.live_viewport_before_on_release,
            diagnostics.live_viewport_after_on_release);
    return diagnostics;
}

std::optional<Terminal_public_scroll_diagnostics>
Terminal_session::synchronized_output_policy_change_diagnostics() const
{
    if (synchronized_output_policy_change_event() ==
        Terminal_synchronized_output_policy_change_event::NONE)
    {
        return std::nullopt;
    }

    Terminal_public_scroll_diagnostics diagnostics;
    diagnostics.effective_policy = effective_synchronized_output_scroll_policy();
    diagnostics.policy_change_event = synchronized_output_policy_change_event();
    diagnostics.diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::
            SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD;
    diagnostics.live_content_publication_blocked = true;
    diagnostics.live_viewport_before_on_release = m_viewport_controller.state();
    diagnostics.live_viewport_after_on_release = m_viewport_controller.state();
    diagnostics.public_viewport_before = m_viewport_controller.state();
    diagnostics.public_viewport_after = m_viewport_controller.state();
    diagnostics.release_reconciliation_result =
        Terminal_release_reconciliation_result::NONE;
    return diagnostics;
}

void Terminal_session::reset_public_projection_lifecycle()
{
    m_public_projection.reset();
    m_public_viewport_controller.reset();
}

void Terminal_session::invalidate_public_projection(
    Terminal_public_projection_disable_reason reason,
    Terminal_public_scroll_diagnostic_reason  diagnostic_reason)
{
    if (reason == Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED ||
        reason == Terminal_public_projection_disable_reason::MEMORY_PRESSURE       ||
        reason == Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED)
    {
        invalidate_search_projection();
    }

    if (!m_public_viewport_controller.has_public_viewport()) {
        return;
    }

    m_public_viewport_controller.invalidate(reason, diagnostic_reason);
    m_public_projection.reset();
}

bool Terminal_session::publish_public_projection_scroll_snapshot(
    std::uint64_t                  sequence,
    QString                        message,
    const Terminal_viewport_state& public_viewport_before,
    const Terminal_viewport_state& public_viewport_after,
    bool                           visible_scroll_applied)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_session::publish_public_projection_scroll_snapshot");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.public_projection_scroll_requests;
    }
#endif
    if (!m_public_projection.has_value()) {
        return false;
    }

    Terminal_public_scroll_diagnostics diagnostics;
    diagnostics.effective_policy =
        effective_synchronized_output_scroll_policy();
    diagnostics.policy_change_event =
        synchronized_output_policy_change_event();
    diagnostics.diagnostic_reason =
        diagnostics.policy_change_event ==
                Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD
            ? Terminal_public_scroll_diagnostic_reason::
                SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD
            : Terminal_public_scroll_diagnostic_reason::NONE;
    diagnostics.public_projection_generation = m_public_projection->generation();
    diagnostics.public_viewport_before       = public_viewport_before;
    diagnostics.public_viewport_after        = public_viewport_after;
    diagnostics.visible_scroll_applied       = visible_scroll_applied;
    diagnostics.live_content_publication_blocked = true;
    diagnostics.release_reconciliation_result =
        Terminal_release_reconciliation_result::NONE;
    diagnostics.hidden_row_eligibility =
        Terminal_hidden_row_eligibility::INELIGIBLE;
    diagnostics.hidden_row_clamp_reason =
        Terminal_hidden_row_clamp_reason::NONE;
    diagnostics.public_projection_disable_reason =
        Terminal_public_projection_disable_reason::NONE;

    std::optional<Terminal_render_snapshot> snapshot =
        public_projection_scroll_snapshot_from_projection(
            *m_public_projection,
            public_viewport_after,
            sequence,
            diagnostics);
    if (!snapshot.has_value()) {
        return false;
    }
    apply_search_matches_to_snapshot(
        *snapshot,
        m_public_projection->active_buffer_epoch());
    if (validate_render_snapshot(*snapshot).status != Terminal_render_snapshot_status::OK) {
        return false;
    }
    snapshot->metadata.publication_generation = m_render_snapshot_generation + 1U;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshots_constructed;
    }
#endif

    std::shared_ptr<const Terminal_render_snapshot> snapshot_handle =
        std::make_shared<const Terminal_render_snapshot>(std::move(*snapshot));
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_snapshot(
            sequence,
            message,
            *snapshot_handle);
    }
#endif
    m_latest_render_snapshot = snapshot_handle;
    m_unrendered_render_snapshot_dirty_basis = snapshot_handle;
    record_snapshot_publication_queued_for_bridge(
        snapshot_handle->metadata.publication_generation);
    ++m_render_snapshot_generation;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_publications;
        ++m_profile_stats.full_snapshot_publications;
        ++m_profile_stats.public_projection_scroll_publications;
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#endif
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        sequence,
        std::move(message),
    });
    return true;
}

void Terminal_session::publish_render_snapshot(
    std::uint64_t  sequence,
    QString        message,
    Terminal_render_snapshot_purpose
                  purpose,
    Terminal_public_scroll_diagnostics public_scroll_diagnostics)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_render_snapshot");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_requests;
    }
#endif
    Terminal_render_snapshot_request request =
        make_render_snapshot_request(sequence, purpose, public_scroll_diagnostics);
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-render-snapshot begin sequence=%1 message=\"%2\" "
                "generation=%3 basis={%4} selection_count=%5 ranges=%6")
                .arg(static_cast<qulonglong>(sequence))
                .arg(message)
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(selection_trace_content_basis(m_selection_content_basis))
                .arg(static_cast<qulonglong>(request.selections.size()))
                .arg(selection_trace_selection_requests(request.selections)));
    }
    Terminal_render_snapshot snapshot = m_screen_model->render_snapshot(request);
    if (!m_search.query().isEmpty()) {
        const bool search_source_matches_snapshot =
            m_search_projection.has_value()                                   &&
            m_search_projection->content_basis() == m_selection_content_basis &&
            m_search_projection->active_buffer_epoch() == m_active_buffer_epoch &&
            m_search_projection->active_buffer() == snapshot.viewport.active_buffer &&
            grid_sizes_match(m_search_projection->grid_size(), snapshot.grid_size) &&
            m_search_projection->metadata().row_origin_generation ==
                snapshot.metadata.row_origin_generation;
        if (!search_source_matches_snapshot) {
            const terminal_search_match_t* const previous_match =
                m_search.current_match();
            const std::int64_t preferred_public_row = previous_match != nullptr
                ? previous_match->public_row
                : render_snapshot_first_visible_logical_row(snapshot);
            if (capture_search_projection_from_safe_basis(
                    snapshot,
                    m_selection_content_basis))
            {
                m_search.rebuild(
                    m_search.query(),
                    *m_search_projection,
                    preferred_public_row);
            }
            else {
                invalidate_search_projection();
            }
        }
    }
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshots_constructed;
        if (snapshot.dirty_row_ranges.empty()) {
            ++m_profile_stats.zero_dirty_snapshot_publications;
        }
    }
#endif
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-render-snapshot constructed requested_sequence=%1 actual_sequence=%2 "
                "generation=%3 selection_span_count=%4 selection_spans=%5 dirty_range_count=%6")
                .arg(static_cast<qulonglong>(sequence))
                .arg(static_cast<qulonglong>(snapshot.metadata.sequence))
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(static_cast<qulonglong>(snapshot.selection_spans.size()))
                .arg(selection_trace_selection_spans(snapshot.selection_spans))
                .arg(static_cast<qulonglong>(snapshot.dirty_row_ranges.size())));
    }
    const Terminal_render_snapshot* dirty_coalescing_snapshot =
        m_unrendered_render_snapshot_dirty_basis != nullptr
            ? m_unrendered_render_snapshot_dirty_basis.get()
            : purpose == Terminal_render_snapshot_purpose::CONTENT
                ? m_latest_content_render_snapshot.get()
                : m_latest_render_snapshot.get();
    if (dirty_coalescing_snapshot           != nullptr &&
        m_render_snapshot_rendered_generation < m_render_snapshot_generation)
    {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.dirty_coalescing_attempts;
        }
#endif
        snapshot = snapshot_with_coalesced_dirty_rows(
            *dirty_coalescing_snapshot,
            std::move(snapshot));
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.dirty_coalescing_applied;
        }
#endif
    }
    suppress_selection_spans_without_valid_line_provenance(snapshot);
    apply_search_matches_to_snapshot(snapshot, m_active_buffer_epoch);
    sync_viewport_controller_to_snapshot(m_viewport_controller, snapshot.viewport);
    std::shared_ptr<const Terminal_render_snapshot> snapshot_handle = std::make_shared<const Terminal_render_snapshot>(
        std::move(snapshot));
    const bool has_live_visible_basis =
        snapshot_handle->basis == Terminal_render_snapshot_basis::LIVE_CONTENT;
    if (!has_live_visible_basis) {
        // PUBLIC_PROJECTION-basis scroll snapshots use a separate non-advancing
        // publication path. Ordinary viewport scroll uses this live path.
        Q_ASSERT(has_live_visible_basis);
        return;
    }
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_snapshot(
            sequence,
            message,
            *snapshot_handle);
    }
#endif
    const bool advances_latest_content =
        render_snapshot_can_advance_latest_content_snapshot(*snapshot_handle);

    m_latest_render_snapshot = snapshot_handle;
    if (advances_latest_content) {
        m_latest_content_render_snapshot               = snapshot_handle;
        m_latest_content_render_snapshot_content_basis = m_selection_content_basis;
    }
    m_deferred_viewport_changed      = false;
    m_visual_bell_active             = false;
    m_unrendered_render_snapshot_dirty_basis = snapshot_handle;
    record_snapshot_publication_queued_for_bridge(
        snapshot_handle->metadata.publication_generation);
    ++m_render_snapshot_generation;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_publications;
        ++m_profile_stats.full_snapshot_publications;
        switch (purpose) {
            case Terminal_render_snapshot_purpose::CONTENT:
                ++m_profile_stats.content_snapshot_publications;
                break;
            case Terminal_render_snapshot_purpose::SELECTION_DERIVED:
                ++m_profile_stats.selection_snapshot_publications;
                break;
            case Terminal_render_snapshot_purpose::SEARCH_DERIVED:
                break;
            case Terminal_render_snapshot_purpose::GEOMETRY_DERIVED:
                ++m_profile_stats.geometry_snapshot_publications;
                break;
            case Terminal_render_snapshot_purpose::SCROLL:
                break;
        }
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#endif
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-render-snapshot end sequence=%1 generation=%2 snapshot_sequence=%3 basis={%4}")
                .arg(static_cast<qulonglong>(sequence))
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(static_cast<qulonglong>(m_latest_render_snapshot->metadata.sequence))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        sequence,
        std::move(message),
    });
}

void Terminal_session::publish_synchronized_resize_snapshot(
    std::uint64_t  sequence,
    QString        message)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_synchronized_resize_snapshot");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_requests;
    }
#endif
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-synchronized-resize-snapshot begin sequence=%1 message=\"%2\" "
                "generation=%3 basis={%4} selection_count=0 ranges=none")
                .arg(static_cast<qulonglong>(sequence))
                .arg(message)
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    const Terminal_render_snapshot* public_snapshot = m_latest_content_render_snapshot.get();
    Terminal_render_snapshot empty_public_snapshot;
    if (public_snapshot == nullptr) {
        empty_public_snapshot =
            make_empty_render_snapshot(
                m_grid_size,
                viewport_adapted_to_grid({}, m_grid_size),
                sequence);
        public_snapshot = &empty_public_snapshot;
    }

    Terminal_render_snapshot snapshot =
        geometry_snapshot_from_public_snapshot(
            *public_snapshot,
            m_grid_size,
            sequence,
            m_backend_geometry_in_sync,
            &m_profile_stats);
    snapshot.metadata.row_origin_generation              = m_row_origin_generation;
    snapshot.metadata.publication_generation             = m_render_snapshot_generation + 1U;
    m_latest_render_snapshot =
        std::make_shared<const Terminal_render_snapshot>(std::move(snapshot));
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshots_constructed;
    }
#endif
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (m_config.transcript_recorder != nullptr) {
        (void)m_config.transcript_recorder->record_snapshot(
            sequence,
            message,
            *m_latest_render_snapshot);
    }
#endif
    m_unrendered_render_snapshot_dirty_basis = m_latest_render_snapshot;
    record_snapshot_publication_queued_for_bridge(
        m_latest_render_snapshot->metadata.publication_generation);
    ++m_render_snapshot_generation;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_publications;
        ++m_profile_stats.full_snapshot_publications;
        ++m_profile_stats.geometry_snapshot_publications;
        update_retained_render_snapshot_profile_stats(
            m_profile_stats,
            m_latest_render_snapshot,
            m_latest_content_render_snapshot);
    }
#endif
    if (selection_trace_requested(m_config.selection_trace_enabled)) {
        write_selection_trace(m_config.selection_trace_enabled,
            QStringLiteral(
                "session publish-synchronized-resize-snapshot end sequence=%1 generation=%2 snapshot_sequence=%3 basis={%4}")
                .arg(static_cast<qulonglong>(sequence))
                .arg(static_cast<qulonglong>(m_render_snapshot_generation))
                .arg(static_cast<qulonglong>(m_latest_render_snapshot->metadata.sequence))
                .arg(selection_trace_content_basis(m_selection_content_basis)));
    }
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        sequence,
        std::move(message),
    });
}

void Terminal_session::advance_ime_preedit_generation()
{
    ++m_ime_preedit_generation;
}

void Terminal_session::record_result(Terminal_session_result result)
{
    if (m_result_capture_sequence != 0U && result.sequence == m_result_capture_sequence) {
        m_captured_result = result;
    }

    if (result.sequence == 0U || m_config.trace_result_limit == 0U) {
        return;
    }

    for (Terminal_session_result& existing : m_results) {
        if (existing.sequence == result.sequence) {
            existing = std::move(result);
            return;
        }
    }

    push_bounded(m_results, std::move(result), m_config.trace_result_limit);
}

Terminal_session_result Terminal_session::result_for_sequence(
    std::uint64_t              sequence,
    Terminal_session_result    fallback) const
{
    for (const Terminal_session_result& result : m_results) {
        if (result.sequence == sequence) {
            return result;
        }
    }

    return fallback;
}

Terminal_session_result Terminal_session::result_after_processing(
    std::uint64_t              sequence,
    Terminal_session_result    enqueue_result) const
{
    Terminal_session_result result =
        (m_captured_result.has_value() && m_captured_result->sequence == sequence)
            ? *m_captured_result
            : result_for_sequence(sequence, enqueue_result);
    result.high_water_reached =
        result.high_water_reached || enqueue_result.high_water_reached;
    return result;
}

std::uint64_t Terminal_session::bell_clock_milliseconds() const
{
    if (m_config.bell_clock_ms) {
        return m_config.bell_clock_ms();
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void Terminal_session::begin_result_capture(std::uint64_t sequence)
{
    m_result_capture_sequence = sequence;
    m_captured_result.reset();
}

void Terminal_session::end_result_capture()
{
    m_result_capture_sequence = 0U;
    m_captured_result.reset();
}

Terminal_session_result Terminal_session::make_rejected_result(
    std::uint64_t                  sequence,
    Terminal_session_result_code   code,
    Terminal_backend_error         error) const
{
    return {
        code,
        sequence,
        false,
        std::move(error),
    };
}

Terminal_session_result Terminal_session::make_accepted_result(
    std::uint64_t                  sequence) const
{
    return {
        Terminal_session_result_code::ACCEPTED,
        sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::make_backend_rejected_result(
    std::uint64_t                          sequence,
    std::optional<Terminal_backend_error>  error) const
{
    return {
        Terminal_session_result_code::BACKEND_REJECTED,
        sequence,
        false,
        std::move(error),
    };
}

std::uint64_t Terminal_session::next_sequence()
{
    const std::uint64_t sequence = m_next_sequence++;
    if (m_next_sequence == 0U) {
        m_next_sequence = 1U;
    }
    return sequence;
}

std::uint64_t Terminal_session::next_resize_id()
{
    const std::uint64_t id = m_next_resize_id++;
    if (m_next_resize_id == 0U) {
        m_next_resize_id = 1U;
    }
    return id;
}

Terminal_session::Queue_category Terminal_session::queue_category_for(
    Terminal_session_command_kind kind) const
{
    switch (kind) {
        case Terminal_session_command_kind::BACKEND_OUTPUT:
            return Queue_category::OUTPUT;
        case Terminal_session_command_kind::USER_WRITE:
        case Terminal_session_command_kind::USER_PASTE:
        case Terminal_session_command_kind::USER_MESSAGE:
        case Terminal_session_command_kind::TERMINAL_REPLY:
            return Queue_category::WRITE;
        case Terminal_session_command_kind::START:
        case Terminal_session_command_kind::INTERRUPT:
        case Terminal_session_command_kind::TERMINATE:
        case Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT:
        case Terminal_session_command_kind::BACKEND_EXIT:
        case Terminal_session_command_kind::BACKEND_ERROR:
        case Terminal_session_command_kind::RESIZE:
            return Queue_category::NONE;
    }

    return Queue_category::NONE;
}

Bounded_terminal_command_queue& Terminal_session::queue_for(Queue_category category)
{
    if (category == Queue_category::OUTPUT) {
        return m_output_queue;
    }

    return m_write_queue;
}

const Bounded_terminal_command_queue& Terminal_session::queue_for(
    Queue_category category) const
{
    if (category == Queue_category::OUTPUT) {
        return m_output_queue;
    }

    return m_write_queue;
}

bool Terminal_session::is_session_writable() const
{
    return
        m_backend != nullptr                               &&
        m_process_state == Terminal_process_state::RUNNING &&
        !m_stop_requested;
}

Terminal_queue_result Terminal_session::would_accept_command(
    Queue_category category,
    std::size_t    byte_count,
    std::size_t    command_count) const
{
    if (category == Queue_category::NONE) {
        return {Terminal_queue_result_code::ACCEPTED, false};
    }

    return queue_for(category).would_accept(byte_count, command_count);
}

void Terminal_session::add_to_queue_state(
    Queue_category category,
    std::size_t    byte_count)
{
    if (category == Queue_category::NONE) {
        return;
    }

    (void)queue_for(category).reserve(byte_count);
}

void Terminal_session::remove_from_queue_state(
    Queue_category category,
    std::size_t    byte_count,
    std::size_t    command_count)
{
    if (category == Queue_category::NONE) {
        return;
    }

    queue_for(category).release(byte_count, command_count);
}

bool Terminal_session::queue_high_water_reached(Queue_category category) const
{
    if (category == Queue_category::NONE) {
        return false;
    }

    return queue_for(category).high_water_reached();
}

void Terminal_session::set_output_backpressure_active(
    bool           active,
    std::uint64_t  sequence)
{
    const bool backend_can_accept_pause =
        m_backend != nullptr &&
        (m_process_state == Terminal_process_state::RUNNING ||
         m_process_state == Terminal_process_state::STARTING) &&
        !m_stop_requested;

    if (m_output_backpressure_active == active) {
        return;
    }

    if (!active) {
        m_output_backpressure_active = false;
        record_notification({
            Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED,
            sequence,
            QStringLiteral("output backpressure released"),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            false,
        });

        if (backend_can_accept_pause) {
            const Terminal_backend_result pause_result = m_backend->set_output_paused(false);
            if (is_backend_rejection(pause_result)) {
                if (!m_output_backpressure_active) {
                    m_output_backpressure_active = true;
                    record_notification({
                        Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED,
                        sequence,
                        QStringLiteral("output backpressure active"),
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        true,
                    });
                }
                record_backend_error(sequence, *pause_result.error);
            }
        }
        return;
    }

    if (backend_can_accept_pause)
    {
        const Terminal_backend_result pause_result = m_backend->set_output_paused(true);
        if (is_backend_rejection(pause_result)) {
            record_backend_error(sequence, *pause_result.error);
            return;
        }
    }

    m_output_backpressure_active = true;
    record_notification({
        Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED,
        sequence,
        QStringLiteral("output backpressure active"),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        true,
    });
}

Terminal_session_result Terminal_session::handle_output_overflow(
    std::uint64_t  sequence,
    QString        message)
{
    Terminal_backend_error error = make_backend_error(
        Terminal_backend_error_code::OUTPUT_OVERFLOW,
        std::move(message));
    record_backend_error(sequence, error);
    terminate_after_output_overflow(sequence);
    return make_rejected_result(
        sequence,
        Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED,
        std::move(error));
}

void Terminal_session::terminate_after_output_overflow(std::uint64_t sequence)
{
    m_stop_requested = true;
    if (m_stop_requested_sequence == 0U || sequence < m_stop_requested_sequence) {
        m_stop_requested_sequence = sequence;
    }
    m_backend_ready = false;
    m_callback_lifetime->stop_backend_output();

    if (m_backend           == nullptr ||
        (m_process_state != Terminal_process_state::RUNNING &&
         m_process_state != Terminal_process_state::STARTING))
    {
        return;
    }

    const Terminal_backend_result terminate_result = m_backend->terminate();
    if (is_backend_rejection(terminate_result)) {
        record_backend_error(sequence, *terminate_result.error);
    }
}

bool Terminal_session::should_ignore_backend_output_after_stop(
    std::uint64_t sequence) const
{
    return m_stop_requested_sequence != 0U && sequence > m_stop_requested_sequence;
}

}
