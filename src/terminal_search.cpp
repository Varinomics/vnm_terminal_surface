#include "vnm_terminal/internal/terminal_search.h"

#include <Qt>

#include <algorithm>
#include <map>
#include <utility>

namespace vnm_terminal::internal {

namespace {

struct Searchable_projection_row
{
    QString             text;
    std::vector<int>    first_columns;
    std::vector<int>    end_columns;
};

Searchable_projection_row searchable_projection_row(
    const Terminal_public_projection_row& row)
{
    Searchable_projection_row searchable;
    int                        next_column = 0;

    for (const Terminal_render_cell& cell : row.cells) {
        if (cell.wide_continuation || cell.text.is_empty()) {
            continue;
        }

        while (next_column < cell.position.column) {
            searchable.text += QLatin1Char(' ');
            searchable.first_columns.push_back(next_column);
            searchable.end_columns.push_back(next_column + 1);
            ++next_column;
        }

        const QString cell_text = cell.text.to_qstring();
        const int first_column  = cell.position.column;
        const int end_column    = first_column + std::max(1, cell.display_width);
        searchable.text += cell_text;
        for (qsizetype index = 0; index < cell_text.size(); ++index) {
            searchable.first_columns.push_back(first_column);
            searchable.end_columns.push_back(end_column);
        }
        next_column = std::max(next_column, end_column);
    }

    return searchable;
}

using Retained_match_ordinal_key = std::pair<std::uint64_t, std::uint64_t>;

Retained_match_ordinal_key retained_match_ordinal_key(
    const Terminal_public_projection_row& row)
{
    return {
        row.provenance.retained_line_id,
        row.provenance.content_generation,
    };
}

terminal_history_handle_t public_search_history_handle(
    const Terminal_public_projection_row& row)
{
    return terminal_history_handle_from_retained_identity(
        row.provenance.retained_line_id,
        row.provenance.content_generation);
}

}

void Terminal_search_controller::clear()
{
    m_query.clear();
    m_matches.clear();
    m_current_match_index.reset();
    m_source_available = false;
}

void Terminal_search_controller::set_source_unavailable(QString query)
{
    m_query = std::move(query);
    m_matches.clear();
    m_current_match_index.reset();
    m_source_available = false;
}

void Terminal_search_controller::rebuild(
    QString                           query,
    const Terminal_public_projection& source,
    std::int64_t                      preferred_public_row)
{
    const bool preserve_current_identity = query == m_query;
    const std::optional<terminal_search_match_identity_t> previous_identity =
        preserve_current_identity && current_match() != nullptr
            ? std::optional<terminal_search_match_identity_t>(current_match()->identity)
            : std::nullopt;

    m_query = std::move(query);
    m_matches.clear();
    m_current_match_index.reset();
    m_source_available = true;

    if (m_query.isEmpty()) {
        return;
    }

    std::map<Retained_match_ordinal_key, int> next_match_ordinals;
    for (const Terminal_public_projection_row& row : source.rows()) {
        const Searchable_projection_row searchable = searchable_projection_row(row);
        qsizetype search_from = 0;
        while (search_from <= searchable.text.size() - m_query.size()) {
            const qsizetype found = searchable.text.indexOf(
                m_query,
                search_from,
                Qt::CaseSensitive);
            if (found < 0) {
                break;
            }

            const qsizetype last = found + m_query.size() - 1;
            if (last < static_cast<qsizetype>(searchable.end_columns.size())) {
                const Retained_match_ordinal_key ordinal_key =
                    retained_match_ordinal_key(row);
                const int match_ordinal = next_match_ordinals[ordinal_key]++;
                const int first_column =
                    searchable.first_columns[static_cast<std::size_t>(found)];
                const int end_column =
                    searchable.end_columns[static_cast<std::size_t>(last)];
                m_matches.push_back({
                    {
                        source.active_buffer(),
                        source.active_buffer_epoch(),
                        public_search_history_handle(row),
                        match_ordinal,
                    },
                    row.public_row,
                    row.visual_fragment_index,
                    first_column,
                    std::max(1, end_column - first_column),
                });
            }

            search_from = found + std::max<qsizetype>(1, m_query.size());
        }
    }

    if (previous_identity.has_value()) {
        const auto current = std::find_if(
            m_matches.begin(),
            m_matches.end(),
            [&](const terminal_search_match_t& match) {
                return match.identity == *previous_identity;
            });
        if (current != m_matches.end()) {
            m_current_match_index = static_cast<std::size_t>(
                std::distance(m_matches.begin(), current));
            return;
        }
    }

    select_initial_match(preferred_public_row);
}

terminal_search_result_state_t Terminal_search_controller::result_state() const
{
    Terminal_search_result_status status = Terminal_search_result_status::INACTIVE;
    if (!m_query.isEmpty()) {
        status = !m_source_available
            ? Terminal_search_result_status::SOURCE_UNAVAILABLE
            : m_matches.empty()
                ? Terminal_search_result_status::NO_MATCH
                : Terminal_search_result_status::MATCH;
    }

    return {
        status,
        static_cast<int>(m_matches.size()),
        m_current_match_index.has_value()
            ? static_cast<int>(*m_current_match_index) + 1
            : 0,
    };
}

const terminal_search_match_t* Terminal_search_controller::current_match() const
{
    return m_current_match_index.has_value() && *m_current_match_index < m_matches.size()
        ? &m_matches[*m_current_match_index]
        : nullptr;
}

bool Terminal_search_controller::select_next()
{
    if (m_matches.empty()) {
        return false;
    }

    m_current_match_index = m_current_match_index.has_value()
        ? (*m_current_match_index + 1U) % m_matches.size()
        : 0U;
    return true;
}

bool Terminal_search_controller::select_previous()
{
    if (m_matches.empty()) {
        return false;
    }

    m_current_match_index = m_current_match_index.has_value()
        ? (*m_current_match_index + m_matches.size() - 1U) % m_matches.size()
        : m_matches.size() - 1U;
    return true;
}

std::vector<Terminal_render_search_match_span>
Terminal_search_controller::spans_for_snapshot(
    const Terminal_render_snapshot& snapshot,
    std::uint64_t                   active_buffer_epoch) const
{
    std::vector<Terminal_render_search_match_span> spans;
    if (m_matches.empty() ||
        snapshot.viewport.active_buffer != m_matches.front().identity.buffer_id ||
        active_buffer_epoch != m_matches.front().identity.active_buffer_epoch)
    {
        return spans;
    }

    for (int viewport_row = 0;
         viewport_row < static_cast<int>(snapshot.visible_line_provenance.size());
         ++viewport_row)
    {
        const Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(viewport_row)];
        const terminal_history_handle_t history_handle =
            terminal_history_handle_from_retained_identity(
                provenance.retained_line_id,
                provenance.content_generation);
        auto match = std::lower_bound(
            m_matches.begin(),
            m_matches.end(),
            provenance.logical_row,
            [](const terminal_search_match_t& candidate, std::int64_t public_row) {
                return candidate.public_row < public_row;
            });
        while (match != m_matches.end() && match->public_row == provenance.logical_row) {
            if (match->identity.history_handle == history_handle) {
                const std::size_t match_index = static_cast<std::size_t>(
                    std::distance(m_matches.begin(), match));
                spans.push_back({
                    viewport_row,
                    match->first_column,
                    match->column_count,
                    m_current_match_index == match_index,
                });
            }
            ++match;
        }
    }
    return spans;
}

void Terminal_search_controller::select_initial_match(
    std::int64_t preferred_public_row)
{
    if (m_matches.empty()) {
        return;
    }

    const auto preferred = std::lower_bound(
        m_matches.begin(),
        m_matches.end(),
        preferred_public_row,
        [](const terminal_search_match_t& match, std::int64_t public_row) {
            return match.public_row < public_row;
        });
    m_current_match_index = preferred != m_matches.end()
        ? static_cast<std::size_t>(std::distance(m_matches.begin(), preferred))
        : 0U;
}

}
