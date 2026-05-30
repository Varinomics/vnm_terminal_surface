#pragma once

#include "vnm_terminal/internal/terminal_history_row_record_codec.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace vnm_terminal::internal {

enum class Terminal_history_row_traversal_status
{
    OK,
    EMPTY,
    NOT_FOUND,
    SEQUENCE_OVERFLOW,
    RING_READ_FAILED,
    ROW_DECODE_FAILED,
    CACHE_ENTRY_INVALID,
    ROW_LINK_MISMATCH,
};

struct Terminal_history_row_traversal_result
{
    Terminal_history_row_traversal_status
                                   status = Terminal_history_row_traversal_status::NOT_FOUND;
    Terminal_history_ring_status   ring_status = Terminal_history_ring_status::OK;
    Terminal_history_row_record_codec_status
                                   codec_status = Terminal_history_row_record_codec_status::OK;
    Terminal_history_row_record_decode_result
                                   row;
};

struct Terminal_history_row_traversal_rebuild_result
{
    Terminal_history_row_traversal_status
                                   status = Terminal_history_row_traversal_status::OK;
    Terminal_history_ring_status   ring_status = Terminal_history_ring_status::OK;
    Terminal_history_row_record_codec_status
                                   codec_status = Terminal_history_row_record_codec_status::OK;
    std::size_t                    row_count = 0U;
};

/**
 * @brief Rebuildable traversal cache over live terminal-history row records.
 *
 * The row directory is an acceleration hint only. Every directory hit is read
 * back from the ring and validated against the live row-record identity before
 * a row is returned.
 */
class Terminal_history_row_traversal
{
public:
    explicit Terminal_history_row_traversal(Terminal_history_ring& ring);

    void discard_directory_cache();
    std::size_t directory_cache_size() const { return m_directory.size(); }

    Terminal_history_row_traversal_rebuild_result rebuild_directory();

    Terminal_history_row_traversal_result oldest_live_row();
    Terminal_history_row_traversal_result latest_live_row();
    Terminal_history_row_traversal_result next_row_after(
        terminal_history_handle_t current_handle);
    Terminal_history_row_traversal_result previous_row_before(
        terminal_history_handle_t current_handle);
    Terminal_history_row_traversal_result find_row_sequence(std::uint64_t row_sequence);
    Terminal_history_row_traversal_result resolve_cached_row(
        terminal_history_handle_t cached_handle);

private:
    Terminal_history_row_traversal_rebuild_result ensure_directory();
    Terminal_history_row_traversal_result decode_live_row(
        std::uint64_t                            byte_sequence,
        std::optional<terminal_history_handle_t> expected_handle);
    Terminal_history_row_traversal_result decode_previous_link(
        const Terminal_history_row_record_decode_result& current_row);
    void remember_row(const Terminal_history_row_record_decode_result& row);
    bool directory_fresh() const;

    Terminal_history_ring&                    m_ring;
    std::map<std::uint64_t, terminal_history_handle_t>
                                             m_directory;
    bool                                     m_directory_valid = false;
    std::uint64_t                            m_cached_oldest_live_byte_sequence = 0U;
    std::uint64_t                            m_cached_head_byte_sequence = 0U;
};

}
