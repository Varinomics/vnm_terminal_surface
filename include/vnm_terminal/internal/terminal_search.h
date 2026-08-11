#pragma once

#include "vnm_terminal/internal/terminal_public_projection.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_search_result_status
{
    INACTIVE,
    SOURCE_UNAVAILABLE,
    NO_MATCH,
    MATCH,
};

struct terminal_search_match_identity_t
{
    Terminal_buffer_id          buffer_id = Terminal_buffer_id::PRIMARY;
    std::uint64_t               active_buffer_epoch = 0U;
    terminal_history_handle_t   history_handle;
    int                         match_ordinal_in_retained_line = 0;

    friend bool operator==(
        const terminal_search_match_identity_t&,
        const terminal_search_match_identity_t&) = default;
};

struct terminal_search_match_t
{
    terminal_search_match_identity_t identity;
    std::int64_t                     public_row = 0;
    int                              visual_fragment_index = 0;
    int                              first_column = 0;
    int                              column_count = 0;
};

struct terminal_search_result_state_t
{
    Terminal_search_result_status status = Terminal_search_result_status::INACTIVE;
    int                           match_count = 0;
    // One-based for direct host display; zero means that no current match exists.
    int                           current_match = 0;

    friend bool operator==(
        const terminal_search_result_state_t&,
        const terminal_search_result_state_t&) = default;
};

class Terminal_search_controller
{
public:
    void clear();
    void set_source_unavailable(QString query);
    void rebuild(
        QString                           query,
        const Terminal_public_projection& source,
        std::int64_t                      preferred_public_row);

    const QString& query() const { return m_query; }
    terminal_search_result_state_t result_state() const;
    const std::vector<terminal_search_match_t>& matches() const { return m_matches; }
    const terminal_search_match_t* current_match() const;

    bool select_next();
    bool select_previous();

    std::vector<Terminal_render_search_match_span> spans_for_snapshot(
        const Terminal_render_snapshot& snapshot,
        std::uint64_t                   active_buffer_epoch) const;

private:
    void select_initial_match(std::int64_t preferred_public_row);

    QString                                m_query;
    std::vector<terminal_search_match_t>   m_matches;
    std::optional<std::size_t>             m_current_match_index;
    bool                                   m_source_available = false;
};

}
