#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr std::uint32_t k_row_record_magic = 0x56524852U;
constexpr std::uint32_t k_row_footer_magic = 0x56524652U;
constexpr std::uint16_t k_row_record_version = 1U;
constexpr std::uint16_t k_row_record_kind_row = 1U;
constexpr std::uint32_t k_row_record_header_bytes = 108U;
constexpr std::uint32_t k_row_record_footer_bytes = 32U;
constexpr std::uint32_t k_row_record_cell_bytes = 24U;
constexpr std::uint32_t k_row_record_hyperlink_bytes = 12U;
constexpr std::uint32_t k_cell_flag_occupied = 1U << 0U;
constexpr std::uint32_t k_cell_flag_wide_continuation = 1U << 1U;
constexpr std::uint32_t k_cell_flag_mask =
    k_cell_flag_occupied | k_cell_flag_wide_continuation;

struct Encoded_record_parts
{
    std::vector<QByteArray>        cell_text_bytes;
    std::size_t                   payload_bytes = 0U;
};

struct row_record_header_t
{
    std::uint32_t                 magic = k_row_record_magic;
    std::uint16_t                 version = k_row_record_version;
    std::uint16_t                 kind = k_row_record_kind_row;
    std::uint32_t                 header_bytes = k_row_record_header_bytes;
    std::uint32_t                 payload_bytes = 0U;
    std::uint32_t                 record_bytes = 0U;
    std::uint32_t                 flags = 0U;
    std::uint64_t                 epoch = 0U;
    std::uint64_t                 byte_sequence = 0U;
    std::uint64_t                 row_sequence = 0U;
    std::uint64_t                 previous_row_byte_sequence = 0U;
    std::uint64_t                 previous_row_sequence = 0U;
    std::uint64_t                 content_generation = 0U;
    std::uint64_t                 retained_line_id = 0U;
    std::uint64_t                 retained_line_content_generation = 0U;
    std::uint32_t                 source_width = 0U;
    std::uint32_t                 cell_count = 0U;
    std::uint32_t                 hyperlink_count = 0U;
    std::uint16_t                 style_lifetime = 0U;
    std::uint16_t                 wrap_state = 0U;
    std::uint16_t                 provenance_source = 0U;
    std::uint16_t                 reserved = 0U;
};

struct row_record_footer_t
{
    std::uint64_t                 byte_sequence = 0U;
    std::uint64_t                 row_sequence = 0U;
    std::uint32_t                 record_bytes = 0U;
    std::uint32_t                 payload_bytes = 0U;
    std::uint32_t                 reserved = 0U;
    std::uint32_t                 magic = k_row_footer_magic;
};

bool checked_add(std::size_t& total, std::size_t value)
{
    if (value > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }

    total += value;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& product)
{
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }

    product = left * right;
    return true;
}

bool size_fits_u32(std::size_t value)
{
    return value <= std::numeric_limits<std::uint32_t>::max();
}

bool size_fits_qsizetype(std::size_t value)
{
    return value <= static_cast<std::size_t>(std::numeric_limits<qsizetype>::max());
}

bool count_fits_u32(std::size_t value)
{
    return value <= std::numeric_limits<std::uint32_t>::max();
}

class Byte_writer
{
public:
    explicit Byte_writer(std::span<std::byte> bytes)
    :
        m_bytes(bytes)
    {}

    bool write_u16(std::uint16_t value)
    {
        return write_unsigned(value, 2U);
    }

    bool write_u32(std::uint32_t value)
    {
        return write_unsigned(value, 4U);
    }

    bool write_u64(std::uint64_t value)
    {
        return write_unsigned(value, 8U);
    }

    bool write_nonnegative_i32(int value)
    {
        if (value < 0) {
            return false;
        }

        return write_u32(static_cast<std::uint32_t>(value));
    }

    bool write_bytes(const QByteArray& bytes)
    {
        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        if (!can_write(byte_count)) {
            return false;
        }

        std::memcpy(m_bytes.data() + m_offset, bytes.constData(), byte_count);
        m_offset += byte_count;
        return true;
    }

    std::size_t offset() const { return m_offset; }

private:
    bool can_write(std::size_t byte_count) const
    {
        return byte_count <= m_bytes.size() - m_offset;
    }

    bool write_unsigned(std::uint64_t value, std::size_t byte_count)
    {
        if (!can_write(byte_count)) {
            return false;
        }

        for (std::size_t i = 0U; i < byte_count; ++i) {
            m_bytes[m_offset + i] = static_cast<std::byte>(
                (value >> (i * 8U)) & 0xffU);
        }
        m_offset += byte_count;
        return true;
    }

    std::span<std::byte>        m_bytes;
    std::size_t                 m_offset = 0U;
};

class Byte_reader
{
public:
    explicit Byte_reader(std::span<const std::byte> bytes)
    :
        m_bytes(bytes)
    {}

    bool read_u16(std::uint16_t& value)
    {
        std::uint64_t raw = 0U;
        if (!read_unsigned(2U, raw)) {
            return false;
        }

        value = static_cast<std::uint16_t>(raw);
        return true;
    }

    bool read_u32(std::uint32_t& value)
    {
        std::uint64_t raw = 0U;
        if (!read_unsigned(4U, raw)) {
            return false;
        }

        value = static_cast<std::uint32_t>(raw);
        return true;
    }

    bool read_u64(std::uint64_t& value)
    {
        return read_unsigned(8U, value);
    }

    bool read_nonnegative_i32(int& value)
    {
        std::uint32_t raw = 0U;
        if (!read_u32(raw)) {
            return false;
        }
        if (raw > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return false;
        }

        value = static_cast<int>(raw);
        return true;
    }

    bool read_bytes(std::uint32_t byte_count, QByteArray& bytes)
    {
        if (!size_fits_qsizetype(byte_count) || !can_read(byte_count)) {
            return false;
        }

        bytes = QByteArray(
            reinterpret_cast<const char*>(m_bytes.data() + m_offset),
            static_cast<qsizetype>(byte_count));
        m_offset += byte_count;
        return true;
    }

    std::size_t offset() const { return m_offset; }

private:
    bool can_read(std::size_t byte_count) const
    {
        return byte_count <= m_bytes.size() - m_offset;
    }

    bool read_unsigned(std::size_t byte_count, std::uint64_t& value)
    {
        if (!can_read(byte_count)) {
            return false;
        }

        value = 0U;
        for (std::size_t i = 0U; i < byte_count; ++i) {
            value |=
                static_cast<std::uint64_t>(m_bytes[m_offset + i]) << (i * 8U);
        }
        m_offset += byte_count;
        return true;
    }

    std::span<const std::byte>  m_bytes;
    std::size_t                 m_offset = 0U;
};

std::optional<std::uint16_t> provenance_source_code(
    Terminal_retained_line_provenance_source source)
{
    switch (source) {
    case Terminal_retained_line_provenance_source::TERMINAL_STORAGE:
        return 0U;
    case Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT:
        return 1U;
    }

    return std::nullopt;
}

std::optional<Terminal_retained_line_provenance_source> provenance_source_from_code(
    std::uint16_t code)
{
    switch (code) {
    case 0U:
        return Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    case 1U:
        return Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint16_t> style_lifetime_code(Terminal_retained_row_style_lifetime lifetime)
{
    switch (lifetime) {
    case Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID:
        return 0U;
    }

    return std::nullopt;
}

std::optional<Terminal_retained_row_style_lifetime> style_lifetime_from_code(
    std::uint16_t code)
{
    switch (code) {
    case 0U:
        return Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint16_t> wrap_state_code(Terminal_retained_row_wrap_state wrap_state)
{
    switch (wrap_state) {
    case Terminal_retained_row_wrap_state::HARD_BOUNDARY:
        return 0U;
    }

    return std::nullopt;
}

std::optional<Terminal_retained_row_wrap_state> wrap_state_from_code(std::uint16_t code)
{
    switch (code) {
    case 0U:
        return Terminal_retained_row_wrap_state::HARD_BOUNDARY;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint32_t> nonnegative_i32_code(int value)
{
    if (value < 0) {
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(value);
}

Terminal_history_row_record_codec_status prepare_encoded_record_parts(
    const Terminal_history_row_record& record,
    Encoded_record_parts&              parts)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_history_row_record_codec::prepare_encode_parts");

    if (!count_fits_u32(record.cells.size()) ||
        !count_fits_u32(record.hyperlink_identity_keys.size()))
    {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    if (!style_lifetime_code(record.metadata.style_lifetime).has_value() ||
        !wrap_state_code(record.metadata.wrap_state).has_value() ||
        !provenance_source_code(record.provenance.source).has_value() ||
        !nonnegative_i32_code(record.metadata.source_width).has_value())
    {
        return Terminal_history_row_record_codec_status::INVALID_ENUM;
    }

    for (const Terminal_history_row_cell& cell : record.cells) {
        if (cell.hyperlink_id != 0U &&
            record.hyperlink_identity_keys.find(cell.hyperlink_id) ==
                record.hyperlink_identity_keys.end())
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
    }

    std::size_t payload_bytes = k_row_record_header_bytes;
    parts.cell_text_bytes.clear();
    parts.cell_text_bytes.reserve(record.cells.size());

    for (const Terminal_history_row_cell& cell : record.cells) {
        if (!nonnegative_i32_code(cell.display_width).has_value()) {
            return Terminal_history_row_record_codec_status::INVALID_ARGUMENT;
        }

        QByteArray text_bytes = cell.text.toUtf8();
        const std::size_t text_byte_count = static_cast<std::size_t>(text_bytes.size());
        if (!size_fits_u32(text_byte_count)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }

        if (!checked_add(payload_bytes, k_row_record_cell_bytes) ||
            !checked_add(payload_bytes, text_byte_count))
        {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }

        parts.cell_text_bytes.push_back(std::move(text_bytes));
    }

    for (const auto& hyperlink : record.hyperlink_identity_keys) {
        const std::size_t key_bytes = static_cast<std::size_t>(hyperlink.second.size());
        if (!size_fits_u32(key_bytes)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }

        if (!checked_add(payload_bytes, k_row_record_hyperlink_bytes) ||
            !checked_add(payload_bytes, key_bytes))
        {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }
    }

    if (!checked_add(payload_bytes, k_row_record_footer_bytes) ||
        !size_fits_u32(payload_bytes))
    {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    parts.payload_bytes = payload_bytes;
    return Terminal_history_row_record_codec_status::OK;
}

std::uint32_t cell_flags(const Terminal_history_row_cell& cell)
{
    std::uint32_t flags = 0U;
    if (cell.occupied) {
        flags |= k_cell_flag_occupied;
    }
    if (cell.wide_continuation) {
        flags |= k_cell_flag_wide_continuation;
    }
    return flags;
}

terminal_history_handle_t history_handle_from_header(const row_record_header_t& header)
{
    return {
        header.epoch,
        header.byte_sequence,
        header.row_sequence,
        header.record_bytes,
        header.content_generation,
    };
}

Terminal_history_row_record_codec_status validate_identity(
    terminal_history_row_record_identity_t identity)
{
    if (identity.epoch == 0U || identity.row_sequence == 0U) {
        return Terminal_history_row_record_codec_status::INVALID_ARGUMENT;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status handle_match_status(
    terminal_history_handle_t actual,
    terminal_history_handle_t expected)
{
    if (actual.epoch != expected.epoch) {
        return Terminal_history_row_record_codec_status::EPOCH_MISMATCH;
    }
    if (actual.byte_sequence != expected.byte_sequence) {
        return Terminal_history_row_record_codec_status::BYTE_SEQUENCE_MISMATCH;
    }
    if (actual.row_sequence != expected.row_sequence) {
        return Terminal_history_row_record_codec_status::ROW_SEQUENCE_MISMATCH;
    }
    if (actual.record_bytes != expected.record_bytes) {
        return Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH;
    }
    if (actual.content_generation != expected.content_generation) {
        return Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH;
    }

    return Terminal_history_row_record_codec_status::OK;
}

bool write_header(Byte_writer& writer, const row_record_header_t& header)
{
    return
        writer.write_u32(header.magic) &&
        writer.write_u16(header.version) &&
        writer.write_u16(header.kind) &&
        writer.write_u32(header.header_bytes) &&
        writer.write_u32(header.payload_bytes) &&
        writer.write_u32(header.record_bytes) &&
        writer.write_u32(header.flags) &&
        writer.write_u64(header.epoch) &&
        writer.write_u64(header.byte_sequence) &&
        writer.write_u64(header.row_sequence) &&
        writer.write_u64(header.previous_row_byte_sequence) &&
        writer.write_u64(header.previous_row_sequence) &&
        writer.write_u64(header.content_generation) &&
        writer.write_u64(header.retained_line_id) &&
        writer.write_u64(header.retained_line_content_generation) &&
        writer.write_u32(header.source_width) &&
        writer.write_u32(header.cell_count) &&
        writer.write_u32(header.hyperlink_count) &&
        writer.write_u16(header.style_lifetime) &&
        writer.write_u16(header.wrap_state) &&
        writer.write_u16(header.provenance_source) &&
        writer.write_u16(header.reserved);
}

bool read_header(Byte_reader& reader, row_record_header_t& header)
{
    return
        reader.read_u32(header.magic) &&
        reader.read_u16(header.version) &&
        reader.read_u16(header.kind) &&
        reader.read_u32(header.header_bytes) &&
        reader.read_u32(header.payload_bytes) &&
        reader.read_u32(header.record_bytes) &&
        reader.read_u32(header.flags) &&
        reader.read_u64(header.epoch) &&
        reader.read_u64(header.byte_sequence) &&
        reader.read_u64(header.row_sequence) &&
        reader.read_u64(header.previous_row_byte_sequence) &&
        reader.read_u64(header.previous_row_sequence) &&
        reader.read_u64(header.content_generation) &&
        reader.read_u64(header.retained_line_id) &&
        reader.read_u64(header.retained_line_content_generation) &&
        reader.read_u32(header.source_width) &&
        reader.read_u32(header.cell_count) &&
        reader.read_u32(header.hyperlink_count) &&
        reader.read_u16(header.style_lifetime) &&
        reader.read_u16(header.wrap_state) &&
        reader.read_u16(header.provenance_source) &&
        reader.read_u16(header.reserved);
}

bool write_footer(Byte_writer& writer, const row_record_footer_t& footer)
{
    return
        writer.write_u64(footer.byte_sequence) &&
        writer.write_u64(footer.row_sequence) &&
        writer.write_u32(footer.record_bytes) &&
        writer.write_u32(footer.payload_bytes) &&
        writer.write_u32(footer.reserved) &&
        writer.write_u32(footer.magic);
}

bool read_footer(Byte_reader& reader, row_record_footer_t& footer)
{
    return
        reader.read_u64(footer.byte_sequence) &&
        reader.read_u64(footer.row_sequence) &&
        reader.read_u32(footer.record_bytes) &&
        reader.read_u32(footer.payload_bytes) &&
        reader.read_u32(footer.reserved) &&
        reader.read_u32(footer.magic);
}

Terminal_history_row_record_codec_status write_row_record_payload(
    std::span<std::byte>                    target,
    const Terminal_history_row_record&      record,
    terminal_history_row_record_identity_t  identity,
    std::uint64_t                           byte_sequence,
    std::uint32_t                           record_bytes,
    const Encoded_record_parts&             parts)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_record_codec::write_payload");

    const Terminal_history_row_record_codec_status identity_status =
        validate_identity(identity);
    if (identity_status != Terminal_history_row_record_codec_status::OK) {
        return identity_status;
    }

    if (target.size() != parts.payload_bytes ||
        record_bytes != target.size() + terminal_history_ring_record_overhead_bytes())
    {
        return Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH;
    }

    const std::optional<std::uint16_t> provenance =
        provenance_source_code(record.provenance.source);
    const std::optional<std::uint16_t> style =
        style_lifetime_code(record.metadata.style_lifetime);
    const std::optional<std::uint16_t> wrap =
        wrap_state_code(record.metadata.wrap_state);
    const std::optional<std::uint32_t> source_width =
        nonnegative_i32_code(record.metadata.source_width);
    if (!provenance.has_value() || !style.has_value() ||
        !wrap.has_value() || !source_width.has_value())
    {
        return Terminal_history_row_record_codec_status::INVALID_ENUM;
    }

    row_record_header_t header;
    header.payload_bytes = static_cast<std::uint32_t>(target.size());
    header.record_bytes = record_bytes;
    header.epoch = identity.epoch;
    header.byte_sequence = byte_sequence;
    header.row_sequence = identity.row_sequence;
    header.previous_row_byte_sequence = identity.previous_row_byte_sequence;
    header.previous_row_sequence = identity.previous_row_sequence;
    header.content_generation = record.provenance.content_generation;
    header.retained_line_id = record.provenance.retained_line_id;
    header.retained_line_content_generation = record.provenance.content_generation;
    header.source_width = *source_width;
    header.cell_count = static_cast<std::uint32_t>(record.cells.size());
    header.hyperlink_count = static_cast<std::uint32_t>(
        record.hyperlink_identity_keys.size());
    header.style_lifetime = *style;
    header.wrap_state = *wrap;
    header.provenance_source = *provenance;

    Byte_writer writer(target);
    if (!write_header(writer, header)) {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::write_payload::cells");

        for (std::size_t i = 0U; i < record.cells.size(); ++i) {
            const Terminal_history_row_cell& cell = record.cells[i];
            const QByteArray& text_bytes = parts.cell_text_bytes[i];
            if (!writer.write_u32(static_cast<std::uint32_t>(text_bytes.size())) ||
                !writer.write_nonnegative_i32(cell.display_width) ||
                !writer.write_u32(cell.style_id) ||
                !writer.write_u32(cell_flags(cell)) ||
                !writer.write_u64(cell.hyperlink_id) ||
                !writer.write_bytes(text_bytes))
            {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::write_payload::hyperlinks");

        for (const auto& hyperlink : record.hyperlink_identity_keys) {
            if (!writer.write_u64(hyperlink.first) ||
                !writer.write_u32(static_cast<std::uint32_t>(hyperlink.second.size())) ||
                !writer.write_bytes(hyperlink.second))
            {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
        }
    }

    const row_record_footer_t footer = {
        byte_sequence,
        identity.row_sequence,
        record_bytes,
        static_cast<std::uint32_t>(target.size()),
        0U,
        k_row_footer_magic,
    };
    if (!write_footer(writer, footer) || writer.offset() != target.size()) {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status validate_header(
    const row_record_header_t&                  header,
    terminal_history_row_record_payload_view_t  payload_view)
{
    if (header.magic != k_row_record_magic ||
        header.version != k_row_record_version ||
        header.kind != k_row_record_kind_row ||
        header.header_bytes != k_row_record_header_bytes ||
        header.flags != 0U ||
        header.reserved != 0U)
    {
        return Terminal_history_row_record_codec_status::INVALID_HEADER;
    }

    if (header.payload_bytes != payload_view.payload.size() ||
        header.record_bytes != payload_view.record_bytes)
    {
        return Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH;
    }

    if (header.byte_sequence != payload_view.byte_sequence) {
        return Terminal_history_row_record_codec_status::BYTE_SEQUENCE_MISMATCH;
    }

    if (header.retained_line_content_generation != header.content_generation) {
        return Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status validate_payload_counts(
    const row_record_header_t& header)
{
    std::size_t minimum_payload_bytes = 0U;
    std::size_t cell_bytes = 0U;
    std::size_t hyperlink_bytes = 0U;
    if (!checked_add(minimum_payload_bytes, k_row_record_header_bytes) ||
        !checked_add(minimum_payload_bytes, k_row_record_footer_bytes) ||
        !checked_multiply(header.cell_count, k_row_record_cell_bytes, cell_bytes) ||
        !checked_add(minimum_payload_bytes, cell_bytes) ||
        !checked_multiply(
            header.hyperlink_count,
            k_row_record_hyperlink_bytes,
            hyperlink_bytes) ||
        !checked_add(minimum_payload_bytes, hyperlink_bytes))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (minimum_payload_bytes > header.payload_bytes) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status validate_footer(
    const row_record_footer_t& footer,
    const row_record_header_t& header)
{
    if (footer.magic != k_row_footer_magic || footer.reserved != 0U) {
        return Terminal_history_row_record_codec_status::INVALID_FOOTER;
    }
    if (footer.byte_sequence != header.byte_sequence) {
        return Terminal_history_row_record_codec_status::BYTE_SEQUENCE_MISMATCH;
    }
    if (footer.row_sequence != header.row_sequence) {
        return Terminal_history_row_record_codec_status::ROW_SEQUENCE_MISMATCH;
    }
    if (footer.record_bytes != header.record_bytes ||
        footer.payload_bytes != header.payload_bytes)
    {
        return Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH;
    }

    return Terminal_history_row_record_codec_status::OK;
}

}

Terminal_history_row_record_append_result encode_terminal_history_row_record_to_ring(
    Terminal_history_ring&                      ring,
    const Terminal_history_row_record&          record,
    terminal_history_row_record_identity_t      identity)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_record_codec::encode_to_ring");

    Terminal_history_row_record_append_result result;

    Encoded_record_parts parts;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::encode_to_ring::prepare");
        result.status = prepare_encoded_record_parts(record, parts);
    }
    if (result.status != Terminal_history_row_record_codec_status::OK) {
        return result;
    }

    Terminal_history_ring_record_reservation reservation;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::encode_to_ring::reserve");
        reservation = ring.reserve_record(parts.payload_bytes);
    }
    result.ring_status = reservation.status();
    if (!reservation.ok()) {
        result.status = Terminal_history_row_record_codec_status::RING_RESERVE_FAILED;
        return result;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::encode_to_ring::materialize_payload");
        result.status = write_row_record_payload(
            reservation.payload(),
            record,
            identity,
            reservation.byte_sequence(),
            reservation.record_bytes(),
            parts);
    }
    if (result.status != Terminal_history_row_record_codec_status::OK) {
        return result;
    }

    result.history_handle = {
        identity.epoch,
        reservation.byte_sequence(),
        identity.row_sequence,
        reservation.record_bytes(),
        record.provenance.content_generation,
    };

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::encode_to_ring::commit");
        result.commit = ring.commit(std::move(reservation));
    }
    result.ring_status = result.commit.status;
    if (result.commit.status != Terminal_history_ring_status::OK) {
        result.status = Terminal_history_row_record_codec_status::RING_COMMIT_FAILED;
        result.history_handle = {};
        return result;
    }

    result.status = Terminal_history_row_record_codec_status::OK;
    return result;
}

Terminal_history_row_record_decode_result decode_terminal_history_row_record(
    const Terminal_history_ring_read_scope&  read_scope,
    std::optional<terminal_history_handle_t> expected_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_record_codec::decode_from_ring_read");

    Terminal_history_row_record_decode_result result;
    result.ring_status = read_scope.status();
    if (!read_scope.ok()) {
        result.status = Terminal_history_row_record_codec_status::RING_READ_FAILED;
        return result;
    }

    return decode_terminal_history_row_record_payload(
        {
            read_scope.byte_sequence(),
            read_scope.record_bytes(),
            read_scope.payload(),
        },
        expected_handle);
}

Terminal_history_row_record_decode_result decode_terminal_history_row_record_payload(
    terminal_history_row_record_payload_view_t payload_view,
    std::optional<terminal_history_handle_t>   expected_handle)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_history_row_record_codec::decode_payload");

    Terminal_history_row_record_decode_result result;

    if (payload_view.payload.size() <
        k_row_record_header_bytes + k_row_record_footer_bytes)
    {
        result.status = Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        return result;
    }

    if (payload_view.record_bytes !=
        payload_view.payload.size() + terminal_history_ring_record_overhead_bytes())
    {
        result.status = Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH;
        return result;
    }

    Byte_reader reader(payload_view.payload);

    row_record_header_t header;
    if (!read_header(reader, header)) {
        result.status = Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        return result;
    }

    result.status = validate_header(header, payload_view);
    if (result.status != Terminal_history_row_record_codec_status::OK) {
        return result;
    }

    result.status = validate_payload_counts(header);
    if (result.status != Terminal_history_row_record_codec_status::OK) {
        return result;
    }

    const std::optional<Terminal_retained_line_provenance_source> provenance =
        provenance_source_from_code(header.provenance_source);
    const std::optional<Terminal_retained_row_style_lifetime> style =
        style_lifetime_from_code(header.style_lifetime);
    const std::optional<Terminal_retained_row_wrap_state> wrap =
        wrap_state_from_code(header.wrap_state);
    if (!provenance.has_value() || !style.has_value() || !wrap.has_value()) {
        result.status = Terminal_history_row_record_codec_status::INVALID_ENUM;
        return result;
    }

    Terminal_history_row_record record;
    record.provenance = {
        header.retained_line_id,
        header.retained_line_content_generation,
        *provenance,
    };
    record.metadata.source_width = static_cast<int>(header.source_width);
    record.metadata.style_lifetime = *style;
    record.metadata.wrap_state = *wrap;
    record.cells.reserve(header.cell_count);

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::decode_payload::materialize_cells");

        for (std::uint32_t i = 0U; i < header.cell_count; ++i) {
            std::uint32_t text_bytes = 0U;
            Terminal_history_row_cell cell;
            std::uint32_t flags = 0U;
            QByteArray encoded_text;

            if (!reader.read_u32(text_bytes) ||
                !reader.read_nonnegative_i32(cell.display_width) ||
                !reader.read_u32(cell.style_id) ||
                !reader.read_u32(flags) ||
                !reader.read_u64(cell.hyperlink_id) ||
                !reader.read_bytes(text_bytes, encoded_text))
            {
                result.status = Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
                return result;
            }

            if ((flags & ~k_cell_flag_mask) != 0U) {
                result.status = Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
                return result;
            }

            cell.occupied = (flags & k_cell_flag_occupied) != 0U;
            cell.wide_continuation =
                (flags & k_cell_flag_wide_continuation) != 0U;
            cell.text = QString::fromUtf8(encoded_text);
            record.cells.push_back(std::move(cell));
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::decode_payload::materialize_hyperlinks");

        for (std::uint32_t i = 0U; i < header.hyperlink_count; ++i) {
            std::uint64_t hyperlink_id = 0U;
            std::uint32_t identity_key_bytes = 0U;
            QByteArray identity_key;

            if (!reader.read_u64(hyperlink_id) ||
                !reader.read_u32(identity_key_bytes) ||
                !reader.read_bytes(identity_key_bytes, identity_key))
            {
                result.status = Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
                return result;
            }

            const bool inserted = record.hyperlink_identity_keys.emplace(
                hyperlink_id,
                std::move(identity_key)).second;
            if (!inserted) {
                result.status = Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
                return result;
            }
        }
    }

    for (const Terminal_history_row_cell& cell : record.cells) {
        if (cell.hyperlink_id != 0U &&
            record.hyperlink_identity_keys.find(cell.hyperlink_id) ==
                record.hyperlink_identity_keys.end())
        {
            result.status = Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            return result;
        }
    }

    if (reader.offset() != payload_view.payload.size() - k_row_record_footer_bytes) {
        result.status = Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        return result;
    }

    row_record_footer_t footer;
    if (!read_footer(reader, footer) || reader.offset() != payload_view.payload.size()) {
        result.status = Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        return result;
    }

    result.status = validate_footer(footer, header);
    if (result.status != Terminal_history_row_record_codec_status::OK) {
        return result;
    }

    const terminal_history_handle_t actual_handle = history_handle_from_header(header);
    if (expected_handle.has_value()) {
        result.status = handle_match_status(actual_handle, *expected_handle);
        if (result.status != Terminal_history_row_record_codec_status::OK) {
            return result;
        }
    }

    result.history_handle = actual_handle;
    result.previous_row_byte_sequence = header.previous_row_byte_sequence;
    result.previous_row_sequence = header.previous_row_sequence;
    result.record = std::move(record);
    result.status = Terminal_history_row_record_codec_status::OK;
    return result;
}

}
