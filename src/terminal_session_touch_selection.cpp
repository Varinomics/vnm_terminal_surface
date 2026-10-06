#include "vnm_terminal/internal/terminal_session.h"

#include "vnm_terminal/internal/terminal_search_row_text.h"
#include <QTextBoundaryFinder>
#include <algorithm>
#include <mutex>

namespace vnm_terminal::internal {

namespace {

terminal_selection_source_t touch_source(
    const terminal_selection_source_identity_t& source)
{
    return {
        source.session_epoch,
        source.buffer_id == Terminal_buffer_id::ALTERNATE
            ? Terminal_selection_buffer::ALTERNATE_SCREEN
            : Terminal_selection_buffer::PRIMARY_SCREEN,
        source.grid_reflow_basis,
        source.grid_size.rows,
        source.grid_size.columns,
    };
}

std::optional<Terminal_selection_range> word_range(
    const Terminal_render_snapshot& snapshot, int visual_row, int logical_row, int column)
{
    QString text;
    std::vector<terminal_grid_position_t> columns;
    int next_column = 0;
    int touched_unit = -1;
    const auto append = [&text, &columns, &touched_unit, column](
        const QString& value, int first, int end)
    {
        if (column >= first && column < end) {
            touched_unit = (int)text.size();
        }
        text += value;
        columns.insert(columns.end(), (std::size_t)value.size(), {first, end});
    };
    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row != visual_row || cell.wide_continuation) {
            continue;
        }
        while (next_column < cell.position.column) {
            append(QStringLiteral(" "), next_column, next_column + 1);
            ++next_column;
        }
        QString value;
        cell.text.append_to(value);
        if (value.isEmpty()) {
            value = QStringLiteral(" ");
        }
        append(value, cell.position.column, cell.position.column + cell.display_width);
        next_column = cell.position.column + cell.display_width;
    }
    while (next_column < snapshot.grid_size.columns) {
        append(QStringLiteral(" "), next_column, next_column + 1);
        ++next_column;
    }
    if (touched_unit < 0 || columns.empty()) {
        return std::nullopt;
    }
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    finder.setPosition(touched_unit);
    int first = finder.isAtBoundary() ? touched_unit : finder.toPreviousBoundary();
    finder.setPosition(touched_unit);
    int end = finder.toNextBoundary();
    first = std::max(0, first);
    end   = std::clamp(end, first + 1, (int)columns.size());
    if (text.mid(first, end - first).trimmed().isEmpty()) {
        return std::nullopt;
    }
    return Terminal_selection_range{
        {logical_row, columns[(std::size_t)first].row},
        {logical_row, columns[(std::size_t)(end - 1)].column},
        Terminal_selection_mode::WORD,
    };
}

} // namespace

void Terminal_session::populate_touch_selection_projection(Terminal_render_snapshot& snapshot) const
{
    if (snapshot.visible_line_provenance.size() != (std::size_t)snapshot.grid_size.rows) {
        snapshot.touch_selection.reset();
        return;
    }
    Terminal_canvas_selection projection;
    projection.word_query_version = k_terminal_word_query_version;
    projection.source = {
        m_selection_session_epoch,
        snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
            ? Terminal_selection_buffer::ALTERNATE_SCREEN
            : Terminal_selection_buffer::PRIMARY_SCREEN,
        m_selection_content_basis.grid_reflow_generation,
        snapshot.grid_size.rows,
        snapshot.grid_size.columns,
    };
    projection.selection_generation = m_selection.generation();
    projection.has_selection = m_selection.has_selection();
    projection.touch_handles_visible = m_touch_handles_visible &&
        m_touch_selection_generation == m_selection.generation() &&
        m_selection.visual_lease().has_value();
    for (const Terminal_render_line_provenance& row : snapshot.visible_line_provenance) {
        projection.visible_rows.push_back({row.retained_line_id, row.content_generation, 0});
    }
    const Terminal_selection_range& range = m_selection.range();
    for (int row = 0; row < (int)snapshot.visible_line_provenance.size(); ++row) {
        const auto logical_row = snapshot.visible_line_provenance[(std::size_t)row].logical_row;
        if (logical_row == range.start.row) {
            projection.start = {row, range.start.column, projection.has_selection};
        }
        if (logical_row == range.end.row) {
            projection.end = {row, range.end.column, projection.has_selection};
        }
    }
    for (const Terminal_render_selection_span& span : snapshot.selection_spans) {
        projection.spans.push_back({span.row, span.first_column, span.column_count});
    }
    if (snapshot.selection_spans.empty()) {
        projection.start.visible = false;
        projection.end.visible   = false;
    }
    snapshot.touch_selection = std::move(projection);
}

Terminal_touch_selection_result Terminal_session::apply_touch_selection(
    const Terminal_touch_selection_request& request)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Input_frontier_scope frontier(*this);
    process_backend_callback_events_to_current_epoch();
    Terminal_touch_selection_result result;
    result.request_id = request.request_id;
    result.action     = request.action;
    result.selection_generation = m_selection.generation();
    const auto fail = [this, &result](Terminal_touch_selection_status status, const char* reason) {
        result.selection_generation = m_selection.generation();
        result.status = status;
        result.reason = QString::fromLatin1(reason);
        return result;
    };
    if (request.request_id == 0U || request.gesture_id.isEmpty() ||
        request.gesture_id.size() > 128 || request.target.row.visual_fragment_index != 0)
    {
        return fail(Terminal_touch_selection_status::INVALID_REQUEST, "Invalid touch selection request.");
    }
    const auto source = published_selection_source_identity_unlocked();
    if (!source || !m_screen_model || touch_source(*source) != request.source) {
        return fail(Terminal_touch_selection_status::STALE_SOURCE, "The terminal display changed.");
    }
    using Action = Terminal_touch_selection_action;
    if (request.action != Action::SELECT_WORD && request.action != Action::CANCEL_GESTURE &&
        request.expected_selection_generation != m_selection.generation())
    {
        return fail(Terminal_touch_selection_status::STALE_SELECTION, "The selection changed.");
    }
    if (request.action == Action::COPY) {
        const auto selected = m_selection.selected_text();
        if (selected.code != Terminal_selection_result_code::OK) {
            return fail(Terminal_touch_selection_status::UNAVAILABLE, "No selected text is available.");
        }
        if (selected.text.toUtf8().size() > k_terminal_touch_selection_max_text_utf8_bytes) {
            return fail(Terminal_touch_selection_status::OVER_LIMIT, "The selected text exceeds the copy limit.");
        }
        result.text   = selected.text;
        result.status = Terminal_touch_selection_status::OK;
        return result;
    }
    if (request.action == Action::CLEAR) {
        m_touch_handles_visible = false;
        m_touch_gesture_id.clear();
        clear_selection();
        result.selection_generation = m_selection.generation();
        result.status = Terminal_touch_selection_status::OK;
        return result;
    }
    if (request.action == Action::END_GESTURE || request.action == Action::CANCEL_GESTURE) {
        if (request.gesture_id != m_touch_gesture_id) {
            return fail(Terminal_touch_selection_status::STALE_SELECTION, "The touch gesture ended.");
        }
        m_touch_gesture_id.clear();
        if (request.action == Action::CANCEL_GESTURE) {
            m_touch_handles_visible = false;
            publish_selection_snapshot(next_sequence(), QStringLiteral("touch selection gesture cancelled"), true);
        }
        result.status = Terminal_touch_selection_status::OK;
        return result;
    }
    if (request.action == Action::BEGIN_HANDLE) {
        if (!m_selection.visual_lease() || !m_selection.has_selection()) {
            return fail(Terminal_touch_selection_status::UNAVAILABLE, "The selection is no longer attached.");
        }
        m_touch_gesture_id = request.gesture_id;
        m_touch_handle    = request.handle;
        result.status = Terminal_touch_selection_status::OK;
        return result;
    }
    if (request.action != Action::SELECT_WORD && request.action != Action::INSPECT_WORD &&
        (request.gesture_id != m_touch_gesture_id || !m_selection.visual_lease() ||
            m_touch_selection_generation != m_selection.generation()))
    {
        return fail(Terminal_touch_selection_status::STALE_SELECTION, "The touch selection changed.");
    }
    if (public_projection_hold_active()) {
        return fail(Terminal_touch_selection_status::UNAVAILABLE, "Terminal selection is temporarily unavailable.");
    }
    int logical_row = -1;
    int visual_row = -1;
    const Terminal_render_snapshot* snapshot = m_latest_render_snapshot.get();
    if (snapshot == nullptr || snapshot->visible_line_provenance.size() !=
            (std::size_t)snapshot->grid_size.rows)
    {
        return fail(Terminal_touch_selection_status::UNAVAILABLE, "The displayed terminal rows are unavailable.");
    }
    int column = request.target.column;
    if (column < 0 || column > source->grid_size.columns) {
        return fail(Terminal_touch_selection_status::INVALID_REQUEST, "Invalid terminal column.");
    }
    if (request.action == Action::SCROLL_EXTEND) {
        if (request.scroll_lines == 0 || request.scroll_lines < -64 || request.scroll_lines > 64) {
            return fail(Terminal_touch_selection_status::INVALID_REQUEST, "Invalid selection scroll distance.");
        }
        const auto before = snapshot->viewport.offset_from_tail;
        (void)scroll_published_viewport_lines(request.scroll_lines);
        snapshot = m_latest_render_snapshot.get();
        if (snapshot == nullptr || snapshot->visible_line_provenance.size() !=
                (std::size_t)snapshot->grid_size.rows)
        {
            return fail(Terminal_touch_selection_status::UNAVAILABLE, "The scrolled terminal rows are unavailable.");
        }
        if (snapshot->viewport.offset_from_tail == before && render_publication_blocked()) {
            return fail(Terminal_touch_selection_status::UNAVAILABLE, "Terminal scrolling is temporarily unavailable.");
        }
        visual_row = request.scroll_lines > 0 ? 0 : snapshot->grid_size.rows - 1;
        logical_row = (int)snapshot->visible_line_provenance[(std::size_t)visual_row].logical_row;
    }
    else {
        for (int row = 0; row < (int)snapshot->visible_line_provenance.size(); ++row) {
            const auto& candidate = snapshot->visible_line_provenance[(std::size_t)row];
            if (candidate.retained_line_id == request.target.row.retained_line_id &&
                candidate.content_generation == request.target.row.content_generation)
            {
                visual_row = row;
                logical_row = (int)candidate.logical_row;
                break;
            }
        }
        if (logical_row < 0 && !render_publication_blocked()) {
            const auto lookup = m_screen_model->retained_line_lookup(
                source->buffer_id,
                terminal_history_handle_from_retained_identity(
                    request.target.row.retained_line_id, request.target.row.content_generation));
            if (lookup.exact_match) {
                logical_row = lookup.exact_logical_row;
            }
        }
        if (logical_row < 0) {
            return fail(Terminal_touch_selection_status::STALE_SOURCE, "The touched terminal row changed or expired.");
        }
    }
    Terminal_selection_range requested_range;
    if (request.action == Action::SELECT_WORD || request.action == Action::INSPECT_WORD) {
        if (column == source->grid_size.columns) {
            return fail(Terminal_touch_selection_status::INVALID_REQUEST, "Invalid word selection column.");
        }
        if (visual_row < 0) {
            return fail(Terminal_touch_selection_status::STALE_SOURCE, "The touched row is unavailable.");
        }
        const auto range = word_range(*snapshot, visual_row, logical_row, column);
        if (request.action == Action::INSPECT_WORD) {
            result.status = Terminal_touch_selection_status::OK;
            result.word_available = range.has_value();
            return result;
        }
        if (!range) {
            return fail(Terminal_touch_selection_status::UNAVAILABLE, "There is no word at the touched position.");
        }
        requested_range = *range;
        m_touch_gesture_id = request.gesture_id;
    }
    else
    if (request.action == Action::MOVE_HANDLE || request.action == Action::SCROLL_EXTEND) {
        requested_range = m_selection.range();
        requested_range.mode = Terminal_selection_mode::NORMAL;
        if (m_touch_handle == Terminal_selection_handle::START) {
            requested_range.start = {logical_row, column};
        }
        else {
            requested_range.end = {logical_row, column};
        }
    }
    else {
        return fail(Terminal_touch_selection_status::INVALID_REQUEST, "Unknown touch selection action.");
    }
    const auto before_generation = m_selection.generation();
    set_selection_range_from_published_source_locked(
        requested_range, published_selection_source_identity_unlocked(), false);
    if (!m_selection.has_selection() || !m_selection.visual_lease() || m_selection.range() != requested_range) {
        if (m_selection.generation() != before_generation) {
            publish_selection_snapshot(next_sequence(), QStringLiteral("touch selection unavailable"), true);
        }
        return fail(Terminal_touch_selection_status::STALE_SOURCE, "The selected terminal rows changed.");
    }
    const bool handles_changed = m_selection.generation() != before_generation || !m_touch_handles_visible ||
        m_touch_selection_generation != m_selection.generation();
    m_touch_handles_visible = true;
    m_touch_selection_generation = m_selection.generation();
    if (handles_changed) {
        publish_selection_snapshot(next_sequence(), QStringLiteral("touch selection handles changed"), true);
    }
    result.selection_generation = m_selection.generation();
    result.status = Terminal_touch_selection_status::OK;
    return result;
}

} // namespace vnm_terminal::internal
