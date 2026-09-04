#pragma once

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_search_row_text.h"

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_search_result_status
{
    INACTIVE,
    SOURCE_UNAVAILABLE,
    NO_MATCH,
    MATCH,
    SEARCHING,
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

struct terminal_search_source_identity_t
{
    terminal_selection_content_basis_t content_basis;
    Terminal_buffer_id                  active_buffer = Terminal_buffer_id::PRIMARY;
    std::uint64_t                       active_buffer_epoch = 0U;
    std::uint64_t                       row_origin_generation = 0U;
    terminal_grid_size_t                grid_size;

    friend bool operator==(
        const terminal_search_source_identity_t& left,
        const terminal_search_source_identity_t& right)
    {
        return
            left.content_basis         == right.content_basis         &&
            left.active_buffer         == right.active_buffer         &&
            left.active_buffer_epoch   == right.active_buffer_epoch   &&
            left.row_origin_generation == right.row_origin_generation &&
            grid_sizes_match(left.grid_size, right.grid_size);
    }
};

struct Terminal_search_source_row
{
    std::uint64_t            retained_line_id = 0U;
    std::uint64_t            content_generation = 0U;
    std::uint64_t            retained_ordinal = 0U;
    int                      active_grid_row = -1;
    Terminal_search_row_text text;
};

struct Terminal_search_source_update
{
    terminal_search_source_identity_t       identity;
    std::uint64_t                           revision = 0U;
    std::uint64_t                           first_retained_ordinal = 0U;
    std::uint64_t                           end_retained_ordinal = 0U;
    bool                                    reset_retained_rows = false;
    bool                                    reset_active_rows = false;
    bool                                    rescan_all_rows = false;
    std::vector<Terminal_search_source_row> retained_rows;
    std::vector<Terminal_search_source_row> active_rows;
};

class Terminal_search_controller
{
public:
    explicit Terminal_search_controller(
        std::function<void()> completion_notifier = {});
    ~Terminal_search_controller();

    Terminal_search_controller(const Terminal_search_controller&)            = delete;
    Terminal_search_controller& operator=(const Terminal_search_controller&) = delete;

    void clear();
    void set_source_unavailable(QString query);
    void set_query(QString query, std::int64_t preferred_public_row);
    void update_source(Terminal_search_source_update update);
    bool process_completion();
    bool wait_for_idle_for_testing(std::chrono::milliseconds timeout);
    bool wait_for_completion_for_testing(std::chrono::milliseconds timeout);

    const QString& query() const { return m_query; }
    terminal_search_result_state_t result_state() const;
    std::optional<terminal_search_match_t> current_match() const;
    std::optional<std::vector<terminal_history_handle_t>> source_row_handles(
        std::int64_t first_public_row,
        int          row_count) const;

    bool select_next();
    bool select_previous();

    std::vector<Terminal_render_search_match_span> spans_for_snapshot(
        const Terminal_render_snapshot& snapshot,
        std::uint64_t                   active_buffer_epoch) const;

private:
    struct Shared_state;

    void request_current_query();

    std::shared_ptr<Shared_state>              m_shared;
    QString                                    m_query;
    terminal_search_source_identity_t          m_source_identity;
    std::uint64_t                              m_query_generation = 0U;
    std::uint64_t                              m_source_revision = 0U;
    std::int64_t                               m_preferred_public_row = 0;
    bool                                       m_source_available = false;
    bool                                       m_searching = false;
};

}
