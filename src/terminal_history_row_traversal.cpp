#include "vnm_terminal/internal/terminal_history_row_traversal.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"

#include <limits>
#include <utility>

namespace vnm_terminal::internal {

namespace {

bool add_overflows(std::uint64_t left, std::uint64_t right)
{
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

bool cache_entry_ring_status(Terminal_history_ring_status status)
{
    return
        status == Terminal_history_ring_status::OUT_OF_LIVE_RANGE ||
        status == Terminal_history_ring_status::NOT_RECORD_BOUNDARY;
}

bool cache_entry_codec_status(Terminal_history_row_record_codec_status status)
{
    return
        status == Terminal_history_row_record_codec_status::EPOCH_MISMATCH ||
        status == Terminal_history_row_record_codec_status::BYTE_SEQUENCE_MISMATCH ||
        status == Terminal_history_row_record_codec_status::ROW_SEQUENCE_MISMATCH ||
        status == Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH ||
        status == Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH;
}

Terminal_history_row_traversal_result make_traversal_result(
    Terminal_history_row_traversal_status status)
{
    Terminal_history_row_traversal_result result;
    result.status = status;
    return result;
}

Terminal_history_row_traversal_result make_ring_failure_result(
    Terminal_history_ring_status ring_status,
    bool                         cache_entry)
{
    Terminal_history_row_traversal_result result;
    result.ring_status = ring_status;
    result.status = cache_entry && cache_entry_ring_status(ring_status)
        ? Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID
        : Terminal_history_row_traversal_status::RING_READ_FAILED;
    return result;
}

Terminal_history_row_traversal_result make_codec_failure_result(
    Terminal_history_row_record_decode_result row,
    bool                                      cache_entry)
{
    Terminal_history_row_traversal_result result;
    result.ring_status  = row.ring_status;
    result.codec_status = row.status;
    result.status = cache_entry && cache_entry_codec_status(row.status)
        ? Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID
        : Terminal_history_row_traversal_status::ROW_DECODE_FAILED;
    result.row = std::move(row);
    return result;
}

Terminal_history_row_traversal_result make_rebuild_failure_result(
    Terminal_history_row_traversal_rebuild_result rebuild)
{
    Terminal_history_row_traversal_result result;
    result.status       = rebuild.status;
    result.ring_status  = rebuild.ring_status;
    result.codec_status = rebuild.codec_status;
    return result;
}

Terminal_history_row_traversal_rebuild_result make_rebuild_failure(
    Terminal_history_row_traversal_status    status,
    Terminal_history_ring_status             ring_status,
    Terminal_history_row_record_codec_status codec_status,
    std::size_t                              row_count)
{
    Terminal_history_row_traversal_rebuild_result result;
    result.status       = status;
    result.ring_status  = ring_status;
    result.codec_status = codec_status;
    result.row_count    = row_count;
    return result;
}

}

Terminal_history_row_traversal::Terminal_history_row_traversal(
    Terminal_history_ring& ring)
:
    m_ring(ring)
{}

void Terminal_history_row_traversal::discard_directory_cache()
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_history_row_traversal::discard_directory_cache");

    m_directory.clear();
    m_directory_valid = false;
    m_cached_oldest_live_byte_sequence = 0U;
    m_cached_head_byte_sequence        = 0U;
}

Terminal_history_row_traversal_rebuild_result
Terminal_history_row_traversal::rebuild_directory()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::rebuild_directory");

    std::map<std::uint64_t, terminal_history_handle_t> rebuilt_directory;
    std::uint64_t sequence = m_ring.oldest_live_byte_sequence();
    const std::uint64_t head = m_ring.head_byte_sequence();

    bool have_previous = false;
    terminal_history_handle_t previous_handle;

    while (sequence < head) {
        const Terminal_history_ring_read_scope read = m_ring.read_record(sequence);
        if (!read.ok()) {
            discard_directory_cache();
            return make_rebuild_failure(
                Terminal_history_row_traversal_status::RING_READ_FAILED,
                read.status(),
                Terminal_history_row_record_codec_status::OK,
                rebuilt_directory.size());
        }

        Terminal_history_row_record_decode_result decoded =
            decode_terminal_history_row_record(read);
        if (decoded.status != Terminal_history_row_record_codec_status::OK) {
            const Terminal_history_row_record_codec_status codec_status = decoded.status;
            discard_directory_cache();
            return make_rebuild_failure(
                Terminal_history_row_traversal_status::ROW_DECODE_FAILED,
                decoded.ring_status,
                codec_status,
                rebuilt_directory.size());
        }

        if (have_previous) {
            if (decoded.history_handle.row_sequence <= previous_handle.row_sequence) {
                discard_directory_cache();
                return make_rebuild_failure(
                    Terminal_history_row_traversal_status::ROW_LINK_MISMATCH,
                    Terminal_history_ring_status::OK,
                    Terminal_history_row_record_codec_status::OK,
                    rebuilt_directory.size());
            }

            if (decoded.previous_row_byte_sequence != previous_handle.byte_sequence ||
                decoded.previous_row_sequence      != previous_handle.row_sequence)
            {
                discard_directory_cache();
                return make_rebuild_failure(
                    Terminal_history_row_traversal_status::ROW_LINK_MISMATCH,
                    Terminal_history_ring_status::OK,
                    Terminal_history_row_record_codec_status::OK,
                    rebuilt_directory.size());
            }
        }

        const bool inserted = rebuilt_directory.emplace(
            decoded.history_handle.row_sequence,
            decoded.history_handle).second;
        if (!inserted) {
            discard_directory_cache();
            return make_rebuild_failure(
                Terminal_history_row_traversal_status::ROW_LINK_MISMATCH,
                Terminal_history_ring_status::OK,
                Terminal_history_row_record_codec_status::OK,
                rebuilt_directory.size());
        }

        previous_handle = decoded.history_handle;
        have_previous   = true;

        if (add_overflows(sequence, read.record_bytes())) {
            discard_directory_cache();
            return make_rebuild_failure(
                Terminal_history_row_traversal_status::SEQUENCE_OVERFLOW,
                Terminal_history_ring_status::OK,
                Terminal_history_row_record_codec_status::OK,
                rebuilt_directory.size());
        }

        sequence += read.record_bytes();
    }

    m_directory                        = std::move(rebuilt_directory);
    m_directory_valid                  = true;
    m_cached_oldest_live_byte_sequence = m_ring.oldest_live_byte_sequence();
    m_cached_head_byte_sequence        = head;

    Terminal_history_row_traversal_rebuild_result result;
    result.row_count = m_directory.size();
    return result;
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::oldest_live_row()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::oldest_live_row");

    const Terminal_history_row_traversal_rebuild_result rebuild = ensure_directory();
    if (rebuild.status != Terminal_history_row_traversal_status::OK) {
        return make_rebuild_failure_result(rebuild);
    }
    if (m_directory.empty()) {
        return make_traversal_result(Terminal_history_row_traversal_status::EMPTY);
    }

    return resolve_cached_row(m_directory.begin()->second);
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::latest_live_row()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::latest_live_row");

    const Terminal_history_row_traversal_rebuild_result rebuild = ensure_directory();
    if (rebuild.status != Terminal_history_row_traversal_status::OK) {
        return make_rebuild_failure_result(rebuild);
    }
    if (m_directory.empty()) {
        return make_traversal_result(Terminal_history_row_traversal_status::EMPTY);
    }

    return resolve_cached_row(m_directory.rbegin()->second);
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::next_row_after(
    terminal_history_handle_t current_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::next_row_after");

    Terminal_history_row_traversal_result current = resolve_cached_row(current_handle);
    if (current.status != Terminal_history_row_traversal_status::OK) {
        return current;
    }

    if (add_overflows(
            current.row.history_handle.byte_sequence,
            current.row.history_handle.record_bytes))
    {
        return make_traversal_result(
            Terminal_history_row_traversal_status::SEQUENCE_OVERFLOW);
    }

    const std::uint64_t next_byte_sequence =
        current.row.history_handle.byte_sequence + current.row.history_handle.record_bytes;
    if (next_byte_sequence >= m_ring.head_byte_sequence()) {
        return make_traversal_result(Terminal_history_row_traversal_status::NOT_FOUND);
    }

    Terminal_history_row_traversal_result next =
        decode_live_row(next_byte_sequence, std::nullopt);
    if (next.status != Terminal_history_row_traversal_status::OK) {
        return next;
    }

    if (next.row.history_handle.row_sequence <= current.row.history_handle.row_sequence) {
        return make_traversal_result(
            Terminal_history_row_traversal_status::ROW_LINK_MISMATCH);
    }

    if (next.row.previous_row_byte_sequence != current.row.history_handle.byte_sequence ||
        next.row.previous_row_sequence      != current.row.history_handle.row_sequence)
    {
        return make_traversal_result(
            Terminal_history_row_traversal_status::ROW_LINK_MISMATCH);
    }

    remember_row(next.row);
    return next;
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::previous_row_before(
    terminal_history_handle_t current_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_history_row_traversal::previous_row_before");

    Terminal_history_row_traversal_result current = resolve_cached_row(current_handle);
    if (current.status != Terminal_history_row_traversal_status::OK) {
        return current;
    }

    return decode_previous_link(current.row);
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::find_row_sequence(
    std::uint64_t row_sequence)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::find_row_sequence");

    Terminal_history_row_traversal_rebuild_result rebuild = ensure_directory();
    if (rebuild.status != Terminal_history_row_traversal_status::OK) {
        return make_rebuild_failure_result(rebuild);
    }

    auto row_it = m_directory.find(row_sequence);
    if (row_it == m_directory.end()) {
        return make_traversal_result(Terminal_history_row_traversal_status::NOT_FOUND);
    }

    Terminal_history_row_traversal_result row = resolve_cached_row(row_it->second);
    if (row.status == Terminal_history_row_traversal_status::OK) {
        return row;
    }

    if (row.status != Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID) {
        return row;
    }

    m_directory.erase(row_it);
    m_directory_valid = false;

    rebuild = rebuild_directory();
    if (rebuild.status != Terminal_history_row_traversal_status::OK) {
        return make_rebuild_failure_result(rebuild);
    }

    row_it = m_directory.find(row_sequence);
    if (row_it == m_directory.end()) {
        return make_traversal_result(Terminal_history_row_traversal_status::NOT_FOUND);
    }

    return resolve_cached_row(row_it->second);
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::resolve_cached_row(
    terminal_history_handle_t cached_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::resolve_cached_row");

    return decode_live_row(cached_handle.byte_sequence, cached_handle);
}

Terminal_history_row_traversal_rebuild_result
Terminal_history_row_traversal::ensure_directory()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::ensure_directory");

    if (directory_fresh()) {
        Terminal_history_row_traversal_rebuild_result result;
        result.row_count = m_directory.size();
        return result;
    }

    return rebuild_directory();
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::decode_live_row(
    std::uint64_t                            byte_sequence,
    std::optional<terminal_history_handle_t> expected_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::decode_live_row");

    const Terminal_history_ring_read_scope read = m_ring.read_record(byte_sequence);
    if (!read.ok()) {
        return make_ring_failure_result(read.status(), expected_handle.has_value());
    }

    Terminal_history_row_record_decode_result decoded =
        decode_terminal_history_row_record(read, expected_handle);
    if (decoded.status != Terminal_history_row_record_codec_status::OK) {
        return make_codec_failure_result(std::move(decoded), expected_handle.has_value());
    }

    Terminal_history_row_traversal_result result;
    result.status = Terminal_history_row_traversal_status::OK;
    result.row = std::move(decoded);
    return result;
}

Terminal_history_row_traversal_result Terminal_history_row_traversal::decode_previous_link(
    const Terminal_history_row_record_decode_result& current_row)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_history_row_traversal::decode_previous_link");

    if (current_row.previous_row_byte_sequence == 0U &&
        current_row.previous_row_sequence      == 0U)
    {
        return make_traversal_result(Terminal_history_row_traversal_status::NOT_FOUND);
    }

    const Terminal_history_ring_read_scope read =
        m_ring.read_record(current_row.previous_row_byte_sequence);
    if (!read.ok()) {
        if (read.status() == Terminal_history_ring_status::OUT_OF_LIVE_RANGE) {
            Terminal_history_row_traversal_result result =
                make_traversal_result(Terminal_history_row_traversal_status::NOT_FOUND);
            result.ring_status = read.status();
            return result;
        }

        return make_ring_failure_result(read.status(), false);
    }

    Terminal_history_row_record_decode_result previous =
        decode_terminal_history_row_record(read);
    if (previous.status != Terminal_history_row_record_codec_status::OK) {
        return make_codec_failure_result(std::move(previous), false);
    }

    if (previous.history_handle.byte_sequence != current_row.previous_row_byte_sequence ||
        previous.history_handle.row_sequence  != current_row.previous_row_sequence)
    {
        return make_traversal_result(
            Terminal_history_row_traversal_status::ROW_LINK_MISMATCH);
    }

    Terminal_history_row_traversal_result result;
    result.status = Terminal_history_row_traversal_status::OK;
    result.row = std::move(previous);
    remember_row(result.row);
    return result;
}

void Terminal_history_row_traversal::remember_row(
    const Terminal_history_row_record_decode_result& row)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_traversal::remember_row");

    if (!directory_fresh()) {
        return;
    }

    m_directory[row.history_handle.row_sequence] = row.history_handle;
}

bool Terminal_history_row_traversal::directory_fresh() const
{
    return
        m_directory_valid &&
        m_cached_oldest_live_byte_sequence == m_ring.oldest_live_byte_sequence() &&
        m_cached_head_byte_sequence        == m_ring.head_byte_sequence();
}

}
