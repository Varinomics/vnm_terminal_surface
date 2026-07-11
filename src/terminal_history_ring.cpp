#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr std::uint32_t k_record_magic = 0x56485231U;
constexpr std::uint32_t k_footer_magic = 0x56484631U;
constexpr std::uint16_t k_record_version = 1U;

struct ring_record_header_t
{
    std::uint32_t magic         = k_record_magic;
    std::uint16_t version       = k_record_version;
    std::uint16_t header_bytes  = k_terminal_history_ring_record_header_bytes;
    std::uint32_t record_bytes  = 0U;
    std::uint32_t payload_bytes = 0U;
    std::uint64_t byte_sequence = 0U;
};

struct ring_record_footer_t
{
    std::uint64_t byte_sequence = 0U;
    std::uint32_t record_bytes  = 0U;
    std::uint32_t magic         = k_footer_magic;
};

static_assert(
    sizeof(ring_record_header_t) == k_terminal_history_ring_record_header_bytes);
static_assert(
    sizeof(ring_record_footer_t) == k_terminal_history_ring_record_footer_bytes);

template <typename T>
void write_plain(std::span<std::byte> target, const T& value)
{
    std::memcpy(target.data(), &value, sizeof(T));
}

template <typename T>
T read_plain(std::span<const std::byte> source)
{
    T value;
    std::memcpy(&value, source.data(), sizeof(T));
    return value;
}

std::size_t effective_alignment(std::size_t alignment_bytes)
{
    if (alignment_bytes == 0U) {
        alignment_bytes = terminal_history_ring_backend_alignment_bytes();
    }

    if (alignment_bytes == 0U) {
        return 0U;
    }

    const std::size_t octile_count = 8U;
    const std::size_t divisor      = std::gcd(alignment_bytes, octile_count);
    if (alignment_bytes > std::numeric_limits<std::size_t>::max() / octile_count) {
        return 0U;
    }

    return (alignment_bytes / divisor) * octile_count;
}

bool add_overflows(std::uint64_t left, std::uint64_t right)
{
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

}

std::size_t terminal_history_ring_backend_alignment_bytes()
{
#ifdef _WIN32
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    if (system_info.dwAllocationGranularity != 0U) {
        return static_cast<std::size_t>(system_info.dwAllocationGranularity);
    }
    if (system_info.dwPageSize != 0U) {
        return static_cast<std::size_t>(system_info.dwPageSize);
    }
    return 4096U;
#else
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        return static_cast<std::size_t>(page_size);
    }
    return 4096U;
#endif
}

std::size_t terminal_history_ring_aligned_capacity(
    std::size_t requested_capacity_bytes,
    std::size_t alignment_bytes)
{
    if (requested_capacity_bytes == 0U) {
        return 0U;
    }

    const std::size_t alignment = effective_alignment(alignment_bytes);
    if (alignment == 0U) {
        return 0U;
    }

    const std::size_t remainder = requested_capacity_bytes % alignment;
    if (remainder == 0U) {
        return requested_capacity_bytes;
    }

    const std::size_t padding = alignment - remainder;
    if (padding > std::numeric_limits<std::size_t>::max() - requested_capacity_bytes) {
        return 0U;
    }

    return requested_capacity_bytes + padding;
}

Terminal_history_ring_status terminal_history_ring_status_from_backend_snapshot(
    Terminal_history_ring_backend_snapshot_status backend_status)
{
    switch (backend_status) {
        case Terminal_history_ring_backend_snapshot_status::OK:
            return Terminal_history_ring_status::OK;
        case Terminal_history_ring_backend_snapshot_status::STALE:
            return Terminal_history_ring_status::SNAPSHOT_STALE;
        case Terminal_history_ring_backend_snapshot_status::RETRY:
            return Terminal_history_ring_status::SNAPSHOT_RETRY;
    }

    return Terminal_history_ring_status::SNAPSHOT_RETRY;
}

Terminal_history_ring_record_reservation::Terminal_history_ring_record_reservation(
    Terminal_history_ring_status status)
:
    m_status(status)
{}

Terminal_history_ring_record_reservation::Terminal_history_ring_record_reservation(
    Terminal_history_ring*     owner,
    std::uint64_t              byte_sequence,
    std::uint32_t              record_bytes,
    std::uint32_t              payload_bytes,
    std::vector<std::byte>     bytes)
:
    m_owner(owner),
    m_status(Terminal_history_ring_status::OK),
    m_active(true),
    m_byte_sequence(byte_sequence),
    m_record_bytes(record_bytes),
    m_payload_bytes(payload_bytes),
    m_bytes(std::move(bytes))
{}

Terminal_history_ring_record_reservation::~Terminal_history_ring_record_reservation()
{
    release();
}

Terminal_history_ring_record_reservation::Terminal_history_ring_record_reservation(
    Terminal_history_ring_record_reservation&& other) noexcept
:
    m_owner(other.m_owner),
    m_status(other.m_status),
    m_active(other.m_active),
    m_byte_sequence(other.m_byte_sequence),
    m_record_bytes(other.m_record_bytes),
    m_payload_bytes(other.m_payload_bytes),
    m_bytes(std::move(other.m_bytes))
{
    other.clear_without_release();
}

Terminal_history_ring_record_reservation& Terminal_history_ring_record_reservation::operator=(
    Terminal_history_ring_record_reservation&& other) noexcept
{
    if (this != &other) {
        release();
        m_owner         = other.m_owner;
        m_status        = other.m_status;
        m_active        = other.m_active;
        m_byte_sequence = other.m_byte_sequence;
        m_record_bytes  = other.m_record_bytes;
        m_payload_bytes = other.m_payload_bytes;
        m_bytes         = std::move(other.m_bytes);
        other.clear_without_release();
    }

    return *this;
}

std::span<std::byte> Terminal_history_ring_record_reservation::payload()
{
    if (!ok()) {
        return {};
    }

    return {
        m_bytes.data() + k_terminal_history_ring_record_header_bytes,
        m_payload_bytes,
    };
}

std::span<const std::byte> Terminal_history_ring_record_reservation::payload() const
{
    if (!ok()) {
        return {};
    }

    return {
        m_bytes.data() + k_terminal_history_ring_record_header_bytes,
        m_payload_bytes,
    };
}

void Terminal_history_ring_record_reservation::release() noexcept
{
    if (m_active && m_owner != nullptr) {
        m_owner->release_reservation();
    }

    clear_without_release();
}

void Terminal_history_ring_record_reservation::clear_without_release() noexcept
{
    m_owner         = nullptr;
    m_active        = false;
    m_status        = Terminal_history_ring_status::INVALID_RESERVATION;
    m_byte_sequence = 0U;
    m_record_bytes  = 0U;
    m_payload_bytes = 0U;
}

Terminal_history_ring_read_scope::Terminal_history_ring_read_scope(
    Terminal_history_ring_status status)
:
    m_status(status)
{}

Terminal_history_ring_read_scope::Terminal_history_ring_read_scope(
    terminal_history_ring_record_descriptor_t descriptor,
    std::vector<std::byte>                    bytes)
:
    m_status(Terminal_history_ring_status::OK),
    m_descriptor(descriptor),
    m_bytes(std::move(bytes))
{}

std::span<const std::byte> Terminal_history_ring_read_scope::record() const
{
    if (!ok()) {
        return {};
    }

    return m_bytes;
}

std::span<const std::byte> Terminal_history_ring_read_scope::payload() const
{
    if (!ok()) {
        return {};
    }

    return {
        m_bytes.data() + k_terminal_history_ring_record_header_bytes,
        m_descriptor.payload_bytes,
    };
}

Terminal_history_ring::Terminal_history_ring(terminal_history_ring_config_t config)
{
    m_capacity_bytes = terminal_history_ring_aligned_capacity(
        config.requested_capacity_bytes,
        config.alignment_bytes);

    if (m_capacity_bytes == 0U) {
        return;
    }

    if (m_capacity_bytes / 8U < terminal_history_ring_record_overhead_bytes()) {
        return;
    }

    m_storage.resize(m_capacity_bytes);
    m_status = Terminal_history_ring_status::OK;
}

std::size_t Terminal_history_ring::max_payload_bytes() const
{
    if (!ok()) {
        return 0U;
    }

    return max_record_bytes() - terminal_history_ring_record_overhead_bytes();
}

std::uint64_t Terminal_history_ring::oldest_live_byte_sequence() const
{
    return m_oldest_live_byte_sequence.load(std::memory_order_acquire);
}

std::uint64_t Terminal_history_ring::head_byte_sequence() const
{
    return m_head_byte_sequence.load(std::memory_order_acquire);
}

Terminal_history_ring_record_reservation Terminal_history_ring::reserve_record(
    std::size_t payload_bytes)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::reserve_record");

    if (!ok()) {
        return Terminal_history_ring_record_reservation(m_status);
    }

    if (m_reservation_open) {
        return Terminal_history_ring_record_reservation(
            Terminal_history_ring_status::RESERVATION_IN_PROGRESS);
    }

    if (payload_bytes > max_payload_bytes()) {
        return Terminal_history_ring_record_reservation(
            Terminal_history_ring_status::OVERSIZE_RECORD);
    }

    if (payload_bytes >
        std::numeric_limits<std::uint32_t>::max() -
            terminal_history_ring_record_overhead_bytes())
    {
        return Terminal_history_ring_record_reservation(
            Terminal_history_ring_status::OVERSIZE_RECORD);
    }

    const std::uint64_t byte_sequence = head_byte_sequence();
    const std::uint32_t payload_count = static_cast<std::uint32_t>(payload_bytes);
    const std::uint32_t record_bytes =
        payload_count + terminal_history_ring_record_overhead_bytes();

    if (add_overflows(byte_sequence, record_bytes)) {
        return Terminal_history_ring_record_reservation(
            Terminal_history_ring_status::SEQUENCE_OVERFLOW);
    }

    std::vector<std::byte> bytes(record_bytes);

    const ring_record_header_t header = {
        k_record_magic,
        k_record_version,
        k_terminal_history_ring_record_header_bytes,
        record_bytes,
        payload_count,
        byte_sequence,
    };
    const ring_record_footer_t footer = {
        byte_sequence,
        record_bytes,
        k_footer_magic,
    };

    write_plain<ring_record_header_t>(
        std::span<std::byte>(bytes).first(sizeof(header)),
        header);
    write_plain<ring_record_footer_t>(
        std::span<std::byte>(bytes).subspan(record_bytes - sizeof(footer), sizeof(footer)),
        footer);

    m_reservation_open = true;
    return Terminal_history_ring_record_reservation(
        this,
        byte_sequence,
        record_bytes,
        payload_count,
        std::move(bytes));
}

terminal_history_ring_commit_result_t Terminal_history_ring::commit(
    Terminal_history_ring_record_reservation&& reservation)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::commit");

    terminal_history_ring_commit_result_t result;
    result.status = Terminal_history_ring_status::INVALID_RESERVATION;

    if (!ok()) {
        result.status = m_status;
        reservation.release();
        return result;
    }

    if (!reservation.ok() || reservation.m_owner != this || !reservation.m_active) {
        reservation.release();
        return result;
    }

    terminal_history_ring_record_descriptor_t descriptor;
    const Terminal_history_ring_status validation_status = validate_record_bytes(
        reservation.m_bytes,
        reservation.m_byte_sequence,
        &descriptor);
    if (validation_status != Terminal_history_ring_status::OK) {
        result.status = validation_status;
        reservation.release();
        return result;
    }

    std::size_t   records_to_discard            = 0U;
    std::uint64_t new_oldest_live_byte_sequence = oldest_live_byte_sequence();
    const Terminal_history_ring_status room_status = plan_room_for(
        reservation.m_record_bytes,
        records_to_discard,
        new_oldest_live_byte_sequence);
    if (room_status != Terminal_history_ring_status::OK) {
        result.status = room_status;
        reservation.release();
        return result;
    }

    if (std::exchange(m_fail_next_record_descriptor_allocation_for_testing, false)) {
        throw std::bad_alloc();
    }
    m_records.push_back(descriptor);

    for (std::size_t record_index = 0U; record_index < records_to_discard; ++record_index) {
        m_records.pop_front();
    }
    if (records_to_discard > 0U) {
        m_oldest_live_byte_sequence.store(
            new_oldest_live_byte_sequence,
            std::memory_order_release);
    }

    write_record_bytes(reservation.m_byte_sequence, reservation.m_bytes);

    const std::uint64_t new_head =
        reservation.m_byte_sequence + reservation.m_record_bytes;
    m_record_index_valid = true;
    m_head_byte_sequence.store(new_head, std::memory_order_release);

    result.status                    = Terminal_history_ring_status::OK;
    result.byte_sequence             = reservation.m_byte_sequence;
    result.record_bytes              = reservation.m_record_bytes;
    result.oldest_live_byte_sequence = oldest_live_byte_sequence();
    result.head_byte_sequence        = new_head;
    result.tail_advanced             = records_to_discard > 0U;

    reservation.release();
    return result;
}

terminal_history_ring_discard_result_t Terminal_history_ring::discard_oldest_records(
    std::size_t record_count)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::discard_oldest_records");

    terminal_history_ring_discard_result_t result;

    if (!ok()) {
        result.status = m_status;
        return result;
    }

    const Terminal_history_ring_status index_status = ensure_record_index();
    if (index_status != Terminal_history_ring_status::OK) {
        result.status = index_status;
        return result;
    }

    const std::size_t records_to_discard = std::min(record_count, m_records.size());
    for (std::size_t index = 0U; index < records_to_discard; ++index) {
        const terminal_history_ring_record_descriptor_t oldest = m_records.front();
        m_records.pop_front();
        m_oldest_live_byte_sequence.store(
            oldest.byte_sequence + oldest.record_bytes,
            std::memory_order_release);
    }

    m_record_index_valid = true;

    result.status                    = Terminal_history_ring_status::OK;
    result.discarded_records         = records_to_discard;
    result.oldest_live_byte_sequence = oldest_live_byte_sequence();
    result.head_byte_sequence        = head_byte_sequence();
    return result;
}

Terminal_history_ring_read_scope Terminal_history_ring::read_record(std::uint64_t byte_sequence)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::read_record");

    if (!ok()) {
        return Terminal_history_ring_read_scope(m_status);
    }

    const Terminal_history_ring_status index_status = ensure_record_index();
    if (index_status != Terminal_history_ring_status::OK) {
        return Terminal_history_ring_read_scope(index_status);
    }

    const std::uint64_t tail = oldest_live_byte_sequence();
    const std::uint64_t head = head_byte_sequence();
    if (byte_sequence < tail || byte_sequence >= head) {
        return Terminal_history_ring_read_scope(Terminal_history_ring_status::OUT_OF_LIVE_RANGE);
    }

    const auto descriptor_it = std::lower_bound(
        m_records.begin(),
        m_records.end(),
        byte_sequence,
        [](terminal_history_ring_record_descriptor_t descriptor, std::uint64_t sequence) {
            return descriptor.byte_sequence < sequence;
        });

    if (descriptor_it == m_records.end() || descriptor_it->byte_sequence != byte_sequence) {
        return Terminal_history_ring_read_scope(Terminal_history_ring_status::NOT_RECORD_BOUNDARY);
    }

    std::vector<std::byte> bytes =
        copy_record_bytes(byte_sequence, descriptor_it->record_bytes);

    terminal_history_ring_record_descriptor_t descriptor;
    const Terminal_history_ring_status validation_status =
        validate_record_bytes(bytes, byte_sequence, &descriptor);
    if (validation_status != Terminal_history_ring_status::OK) {
        return Terminal_history_ring_read_scope(validation_status);
    }

    return Terminal_history_ring_read_scope(descriptor, std::move(bytes));
}

void Terminal_history_ring::discard_record_index_cache()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::discard_record_index_cache");

    m_records.clear();
    m_record_index_valid = false;
}

Terminal_history_ring_status Terminal_history_ring::rebuild_record_index()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::rebuild_record_index");

    if (!ok()) {
        return m_status;
    }

    std::deque<terminal_history_ring_record_descriptor_t> rebuilt_records;
    std::uint64_t sequence = oldest_live_byte_sequence();
    const std::uint64_t head = head_byte_sequence();

    while (sequence < head) {
        const std::uint64_t remaining = head - sequence;
        if (remaining < terminal_history_ring_record_overhead_bytes()) {
            m_records.clear();
            m_record_index_valid = false;
            return Terminal_history_ring_status::PARTIAL_RECORD;
        }

        std::vector<std::byte> header_bytes = copy_record_bytes(
            sequence,
            k_terminal_history_ring_record_header_bytes);
        const ring_record_header_t header = read_plain<ring_record_header_t>(
            std::span<const std::byte>(header_bytes));

        if (header.magic        != k_record_magic ||
            header.version      != k_record_version ||
            header.header_bytes != k_terminal_history_ring_record_header_bytes ||
            header.byte_sequence != sequence)
        {
            m_records.clear();
            m_record_index_valid = false;
            return Terminal_history_ring_status::INVALID_RECORD;
        }

        if (header.record_bytes < terminal_history_ring_record_overhead_bytes()) {
            m_records.clear();
            m_record_index_valid = false;
            return Terminal_history_ring_status::INVALID_RECORD;
        }
        if (header.record_bytes > max_record_bytes()) {
            m_records.clear();
            m_record_index_valid = false;
            return Terminal_history_ring_status::INVALID_RECORD;
        }
        if (header.record_bytes > remaining) {
            m_records.clear();
            m_record_index_valid = false;
            return Terminal_history_ring_status::PARTIAL_RECORD;
        }

        std::vector<std::byte> record_bytes = copy_record_bytes(
            sequence,
            header.record_bytes);

        terminal_history_ring_record_descriptor_t descriptor;
        const Terminal_history_ring_status validation_status =
            validate_record_bytes(record_bytes, sequence, &descriptor);
        if (validation_status != Terminal_history_ring_status::OK) {
            m_records.clear();
            m_record_index_valid = false;
            return validation_status;
        }

        rebuilt_records.push_back(descriptor);
        sequence += descriptor.record_bytes;
    }

    m_records            = std::move(rebuilt_records);
    m_record_index_valid = true;
    return Terminal_history_ring_status::OK;
}

Terminal_history_ring_record_index_result Terminal_history_ring::live_record_descriptors()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::live_record_descriptors");

    Terminal_history_ring_record_index_result result;
    result.status = ensure_record_index();
    if (result.status != Terminal_history_ring_status::OK) {
        return result;
    }

    result.records.assign(m_records.begin(), m_records.end());
    return result;
}

void Terminal_history_ring::release_reservation() noexcept
{
    m_reservation_open = false;
}

Terminal_history_ring_status Terminal_history_ring::plan_room_for(
    std::uint32_t  record_bytes,
    std::size_t&   out_records_to_discard,
    std::uint64_t& out_new_oldest_live_byte_sequence)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::plan_room_for");

    out_records_to_discard            = 0U;
    out_new_oldest_live_byte_sequence = oldest_live_byte_sequence();

    if (record_bytes > max_record_bytes()) {
        return Terminal_history_ring_status::OVERSIZE_RECORD;
    }

    const Terminal_history_ring_status index_status = ensure_record_index();
    if (index_status != Terminal_history_ring_status::OK) {
        return index_status;
    }

    const std::uint64_t head = head_byte_sequence();
    if (add_overflows(head, record_bytes)) {
        return Terminal_history_ring_status::SEQUENCE_OVERFLOW;
    }

    const std::uint64_t required_head = head + record_bytes;
    while (required_head - out_new_oldest_live_byte_sequence > m_capacity_bytes) {
        if (out_records_to_discard >= m_records.size()) {
            return Terminal_history_ring_status::INVALID_RECORD;
        }

        const terminal_history_ring_record_descriptor_t oldest =
            m_records[out_records_to_discard];
        out_new_oldest_live_byte_sequence = oldest.byte_sequence + oldest.record_bytes;
        ++out_records_to_discard;
    }

    return Terminal_history_ring_status::OK;
}

Terminal_history_ring_status Terminal_history_ring::ensure_record_index()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::ensure_record_index");

    if (m_record_index_valid) {
        return Terminal_history_ring_status::OK;
    }

    return rebuild_record_index();
}

Terminal_history_ring_status Terminal_history_ring::validate_record_bytes(
    std::span<const std::byte>                    bytes,
    std::uint64_t                                 expected_byte_sequence,
    terminal_history_ring_record_descriptor_t*    out_descriptor) const
{
    if (bytes.size() < terminal_history_ring_record_overhead_bytes()) {
        return Terminal_history_ring_status::PARTIAL_RECORD;
    }
    if (bytes.size() > max_record_bytes()) {
        return Terminal_history_ring_status::OVERSIZE_RECORD;
    }

    const ring_record_header_t header = read_plain<ring_record_header_t>(
        bytes.first(sizeof(ring_record_header_t)));
    if (header.magic         != k_record_magic ||
        header.version       != k_record_version ||
        header.header_bytes  != k_terminal_history_ring_record_header_bytes ||
        header.record_bytes  != bytes.size() ||
        header.byte_sequence != expected_byte_sequence)
    {
        return Terminal_history_ring_status::INVALID_RECORD;
    }

    if (header.payload_bytes + terminal_history_ring_record_overhead_bytes() !=
        header.record_bytes)
    {
        return Terminal_history_ring_status::INVALID_RECORD;
    }

    const ring_record_footer_t footer = read_plain<ring_record_footer_t>(
        bytes.subspan(bytes.size() - sizeof(ring_record_footer_t), sizeof(ring_record_footer_t)));
    if (footer.magic         != k_footer_magic ||
        footer.record_bytes  != header.record_bytes ||
        footer.byte_sequence != expected_byte_sequence)
    {
        return Terminal_history_ring_status::INVALID_RECORD;
    }

    if (out_descriptor != nullptr) {
        *out_descriptor = {
            expected_byte_sequence,
            header.record_bytes,
            header.payload_bytes,
        };
    }

    return Terminal_history_ring_status::OK;
}

void Terminal_history_ring::write_record_bytes(
    std::uint64_t              byte_sequence,
    std::span<const std::byte> bytes) noexcept
{
    const std::size_t start = static_cast<std::size_t>(byte_sequence % m_capacity_bytes);
    const std::size_t first = std::min(bytes.size(), m_capacity_bytes - start);

    std::memcpy(m_storage.data() + start, bytes.data(), first);
    if (first < bytes.size()) {
        std::memcpy(m_storage.data(), bytes.data() + first, bytes.size() - first);
    }
}

std::vector<std::byte> Terminal_history_ring::copy_record_bytes(
    std::uint64_t byte_sequence,
    std::uint32_t record_bytes) const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_ring::copy_record_bytes");

    std::vector<std::byte> bytes(record_bytes);
    const std::size_t start = static_cast<std::size_t>(byte_sequence % m_capacity_bytes);
    const std::size_t first = std::min<std::size_t>(record_bytes, m_capacity_bytes - start);

    std::memcpy(bytes.data(), m_storage.data() + start, first);
    if (first < record_bytes) {
        std::memcpy(bytes.data() + first, m_storage.data(), record_bytes - first);
    }

    return bytes;
}

}
