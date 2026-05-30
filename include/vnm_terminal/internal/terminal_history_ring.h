#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace vnm_terminal::internal {

constexpr std::uint32_t k_terminal_history_ring_record_header_bytes = 24U;
constexpr std::uint32_t k_terminal_history_ring_record_footer_bytes = 16U;

constexpr std::uint32_t terminal_history_ring_record_overhead_bytes()
{
    return
        k_terminal_history_ring_record_header_bytes +
        k_terminal_history_ring_record_footer_bytes;
}

enum class Terminal_history_ring_status
{
    OK,
    INVALID_CAPACITY,
    OVERSIZE_RECORD,
    SEQUENCE_OVERFLOW,
    RESERVATION_IN_PROGRESS,
    INVALID_RESERVATION,
    OUT_OF_LIVE_RANGE,
    NOT_RECORD_BOUNDARY,
    PARTIAL_RECORD,
    INVALID_RECORD,
    SNAPSHOT_STALE,
    SNAPSHOT_RETRY,
};

enum class Terminal_history_ring_backend_snapshot_status
{
    OK,
    STALE,
    RETRY,
};

struct terminal_history_ring_config_t
{
    std::size_t requested_capacity_bytes = 0U;
    std::size_t alignment_bytes          = 0U;
};

struct terminal_history_ring_record_descriptor_t
{
    std::uint64_t byte_sequence = 0U;
    std::uint32_t record_bytes  = 0U;
    std::uint32_t payload_bytes = 0U;
};

struct terminal_history_ring_commit_result_t
{
    Terminal_history_ring_status status                    = Terminal_history_ring_status::OK;
    std::uint64_t                byte_sequence             = 0U;
    std::uint32_t                record_bytes              = 0U;
    std::uint64_t                oldest_live_byte_sequence = 0U;
    std::uint64_t                head_byte_sequence        = 0U;
    bool                         tail_advanced             = false;
};

struct terminal_history_ring_discard_result_t
{
    Terminal_history_ring_status status                    = Terminal_history_ring_status::OK;
    std::size_t                  discarded_records         = 0U;
    std::uint64_t                oldest_live_byte_sequence = 0U;
    std::uint64_t                head_byte_sequence        = 0U;
};

struct Terminal_history_ring_record_index_result
{
    Terminal_history_ring_status                         status = Terminal_history_ring_status::OK;
    std::vector<terminal_history_ring_record_descriptor_t> records;
};

std::size_t terminal_history_ring_backend_alignment_bytes();
std::size_t terminal_history_ring_aligned_capacity(
    std::size_t requested_capacity_bytes,
    std::size_t alignment_bytes = 0U);

Terminal_history_ring_status terminal_history_ring_status_from_backend_snapshot(
    Terminal_history_ring_backend_snapshot_status backend_status);

class Terminal_history_ring;

class Terminal_history_ring_record_reservation
{
public:
    Terminal_history_ring_record_reservation() = default;
    ~Terminal_history_ring_record_reservation();

    Terminal_history_ring_record_reservation(
        const Terminal_history_ring_record_reservation&) = delete;
    Terminal_history_ring_record_reservation& operator=(
        const Terminal_history_ring_record_reservation&) = delete;

    Terminal_history_ring_record_reservation(
        Terminal_history_ring_record_reservation&& other) noexcept;
    Terminal_history_ring_record_reservation& operator=(
        Terminal_history_ring_record_reservation&& other) noexcept;

    Terminal_history_ring_status status() const { return m_status; }
    bool ok() const { return m_status == Terminal_history_ring_status::OK; }

    std::uint64_t byte_sequence() const { return m_byte_sequence; }
    std::uint32_t record_bytes()  const { return m_record_bytes;  }
    std::uint32_t payload_bytes() const { return m_payload_bytes; }

    std::span<std::byte>       payload();
    std::span<const std::byte> payload() const;

private:
    friend class Terminal_history_ring;

    explicit Terminal_history_ring_record_reservation(Terminal_history_ring_status status);
    Terminal_history_ring_record_reservation(
        Terminal_history_ring* owner,
        std::uint64_t          byte_sequence,
        std::uint32_t          record_bytes,
        std::uint32_t          payload_bytes,
        std::vector<std::byte> bytes);

    void release() noexcept;
    void clear_without_release() noexcept;

    Terminal_history_ring*     m_owner         = nullptr;
    Terminal_history_ring_status m_status      = Terminal_history_ring_status::INVALID_RESERVATION;
    bool                       m_active        = false;
    std::uint64_t              m_byte_sequence = 0U;
    std::uint32_t              m_record_bytes  = 0U;
    std::uint32_t              m_payload_bytes = 0U;
    std::vector<std::byte>     m_bytes;
};

class Terminal_history_ring_read_scope
{
public:
    Terminal_history_ring_read_scope() = default;

    Terminal_history_ring_status status() const { return m_status; }
    bool ok() const { return m_status == Terminal_history_ring_status::OK; }

    terminal_history_ring_record_descriptor_t descriptor() const { return m_descriptor; }
    std::uint64_t byte_sequence() const { return m_descriptor.byte_sequence; }
    std::uint32_t record_bytes()  const { return m_descriptor.record_bytes;  }
    std::uint32_t payload_bytes() const { return m_descriptor.payload_bytes; }

    std::span<const std::byte> record()  const;
    std::span<const std::byte> payload() const;

private:
    friend class Terminal_history_ring;

    explicit Terminal_history_ring_read_scope(Terminal_history_ring_status status);
    Terminal_history_ring_read_scope(
        terminal_history_ring_record_descriptor_t descriptor,
        std::vector<std::byte>                    bytes);

    Terminal_history_ring_status              m_status = Terminal_history_ring_status::OUT_OF_LIVE_RANGE;
    terminal_history_ring_record_descriptor_t m_descriptor;
    std::vector<std::byte>                    m_bytes;
};

class Terminal_history_ring
{
public:
    explicit Terminal_history_ring(terminal_history_ring_config_t config);

    Terminal_history_ring(const Terminal_history_ring&) = delete;
    Terminal_history_ring& operator=(const Terminal_history_ring&) = delete;

    Terminal_history_ring_status status() const { return m_status; }
    bool ok() const { return m_status == Terminal_history_ring_status::OK; }

    std::size_t capacity_bytes() const { return m_capacity_bytes; }
    std::size_t max_record_bytes() const { return m_capacity_bytes / 8U; }
    std::size_t max_payload_bytes() const;

    std::uint64_t oldest_live_byte_sequence() const;
    std::uint64_t head_byte_sequence() const;

    Terminal_history_ring_record_reservation reserve_record(std::size_t payload_bytes);
    terminal_history_ring_commit_result_t commit(
        Terminal_history_ring_record_reservation&& reservation);
    terminal_history_ring_discard_result_t discard_oldest_records(
        std::size_t record_count);

    Terminal_history_ring_read_scope read_record(std::uint64_t byte_sequence);

    void discard_record_index_cache();
    Terminal_history_ring_status rebuild_record_index();
    Terminal_history_ring_record_index_result live_record_descriptors();

private:
    friend class Terminal_history_ring_record_reservation;

    void release_reservation() noexcept;

    Terminal_history_ring_status make_room_for(
        std::uint32_t record_bytes,
        bool&         out_tail_advanced);
    Terminal_history_ring_status ensure_record_index();
    Terminal_history_ring_status validate_record_bytes(
        std::span<const std::byte>                    bytes,
        std::uint64_t                                 expected_byte_sequence,
        terminal_history_ring_record_descriptor_t*    out_descriptor) const;

    void write_record_bytes(
        std::uint64_t              byte_sequence,
        std::span<const std::byte> bytes);
    std::vector<std::byte> copy_record_bytes(
        std::uint64_t byte_sequence,
        std::uint32_t record_bytes) const;

    std::size_t                    m_capacity_bytes = 0U;
    std::vector<std::byte>         m_storage;
    std::deque<terminal_history_ring_record_descriptor_t>
                                   m_records;
    std::atomic<std::uint64_t>     m_oldest_live_byte_sequence{0U};
    std::atomic<std::uint64_t>     m_head_byte_sequence{0U};
    Terminal_history_ring_status   m_status = Terminal_history_ring_status::INVALID_CAPACITY;
    bool                           m_record_index_valid = true;
    bool                           m_reservation_open   = false;
};

}
