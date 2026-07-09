#include "vnm_terminal/internal/terminal_history_row_record_codec.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/unicode_width.h"

#include <QByteArrayView>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr std::uint32_t k_row_record_magic = 0x56524852U;
constexpr std::uint16_t k_row_record_version = 2U;
constexpr std::uint16_t k_row_record_kind_row = 1U;
constexpr std::uint32_t k_row_record_header_bytes = 116U;
constexpr std::uint32_t k_payload_kind_mask = 0x0fU;
constexpr std::uint32_t k_payload_kind_generic_compact = 0U;
constexpr std::uint32_t k_payload_kind_prefix_plain_ascii = 1U;
constexpr std::size_t k_encoded_style_bytes = 16U;

constexpr std::uint8_t k_opcode_default_blank = 0x00U;
constexpr std::uint8_t k_opcode_wide_continuation = 0x01U;
constexpr std::uint8_t k_opcode_extended_cell = 0x02U;

constexpr std::uint8_t k_width_code_default = 0U;
constexpr std::uint8_t k_width_code_u8 = 1U;
constexpr std::uint8_t k_width_code_u16 = 2U;
constexpr std::uint8_t k_width_code_u32 = 3U;

constexpr std::uint8_t k_extended_kind_occupied = 0U;
constexpr std::uint8_t k_extended_kind_unoccupied_styled = 1U;
constexpr std::uint8_t k_extended_extra_display = 1U;
constexpr std::uint8_t k_extended_extra_hyperlink = 2U;
constexpr std::uint16_t k_terminal_palette_color_count = 256U;
constexpr std::uint16_t k_terminal_style_attribute_known_mask =
    terminal_style_attribute_mask(Terminal_style_attribute::BOLD)      |
    terminal_style_attribute_mask(Terminal_style_attribute::FAINT)     |
    terminal_style_attribute_mask(Terminal_style_attribute::ITALIC)    |
    terminal_style_attribute_mask(Terminal_style_attribute::UNDERLINE) |
    terminal_style_attribute_mask(Terminal_style_attribute::BLINK)     |
    terminal_style_attribute_mask(Terminal_style_attribute::INVERSE)   |
    terminal_style_attribute_mask(Terminal_style_attribute::INVISIBLE) |
    terminal_style_attribute_mask(Terminal_style_attribute::STRIKE);

struct Encoded_cell_part
{
    QByteArray                    text_bytes;
    std::uint8_t                  printable_ascii = 0U;
    bool                          default_printable_ascii = false;
};

struct Encoded_record_parts
{
    std::vector<Encoded_cell_part> cell_parts;
    std::size_t                   payload_bytes = 0U;
    std::uint32_t                 payload_kind = k_payload_kind_generic_compact;
    std::size_t                   prefix_plain_ascii_bytes = 0U;
};

struct row_record_header_t
{
    std::uint32_t                 magic = k_row_record_magic;
    std::uint16_t                 version = k_row_record_version;
    std::uint16_t                 kind = k_row_record_kind_row;
    std::uint32_t                 header_bytes = k_row_record_header_bytes;
    std::uint32_t                 payload_bytes = 0U;
    std::uint32_t                 record_bytes = 0U;
    std::uint32_t                 flags = k_payload_kind_generic_compact;
    std::uint64_t                 epoch = 0U;
    std::uint64_t                 byte_sequence = 0U;
    std::uint64_t                 row_sequence = 0U;
    std::uint64_t                 previous_row_byte_sequence = 0U;
    std::uint64_t                 previous_row_sequence = 0U;
    std::uint64_t                 content_generation = 0U;
    std::uint64_t                 retained_line_id = 0U;
    std::uint64_t                 retained_line_content_generation = 0U;
    std::uint64_t                 content_stamp_ms = 0U;
    std::uint32_t                 source_width = 0U;
    std::uint32_t                 cell_count = 0U;
    std::uint32_t                 hyperlink_count = 0U;
    std::uint16_t                 style_lifetime = 0U;
    std::uint16_t                 wrap_state = 0U;
    std::uint16_t                 provenance_source = 0U;
    std::uint16_t                 style_count = 0U;
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

bool count_fits_u16(std::size_t value)
{
    return value <= std::numeric_limits<std::uint16_t>::max();
}

bool count_fits_u32(std::size_t value)
{
    return value <= std::numeric_limits<std::uint32_t>::max();
}

bool source_width_is_supported(int source_width)
{
    return source_width > 0 && source_width <= k_terminal_screen_model_max_columns;
}

std::uint32_t payload_kind_from_flags(std::uint32_t flags)
{
    return flags & k_payload_kind_mask;
}

bool payload_kind_is_supported(std::uint32_t payload_kind)
{
    return
        payload_kind == k_payload_kind_generic_compact ||
        payload_kind == k_payload_kind_prefix_plain_ascii;
}

bool byte_is_printable_ascii(unsigned char value)
{
    return value >= 0x20U && value <= 0x7eU;
}

std::uint8_t shortest_width_code(std::uint32_t value)
{
    if (value == 0U) {
        return k_width_code_default;
    }
    if (value <= std::numeric_limits<std::uint8_t>::max()) {
        return k_width_code_u8;
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return k_width_code_u16;
    }
    return k_width_code_u32;
}

std::size_t width_code_bytes(std::uint8_t width_code)
{
    switch (width_code) {
        case k_width_code_default: return 0U;
        case k_width_code_u8:      return 1U;
        case k_width_code_u16:     return 2U;
        case k_width_code_u32:     return 4U;
        default:                   return std::numeric_limits<std::size_t>::max();
    }
}

bool width_code_is_valid(std::uint8_t width_code)
{
    return width_code <= k_width_code_u32;
}

bool width_code_is_shortest(std::uint32_t value, std::uint8_t width_code)
{
    return width_code == shortest_width_code(value);
}

bool color_ref_is_canonical(const Terminal_color_ref& color)
{
    switch (color.kind) {
        case Terminal_color_ref_kind::DEFAULT:
            return color.palette_index == 0U && color.rgba == 0U;
        case Terminal_color_ref_kind::PALETTE_INDEX:
            return color.palette_index < k_terminal_palette_color_count &&
                color.rgba == 0U;
        case Terminal_color_ref_kind::RGB:
            return color.palette_index == 0U;
    }

    return false;
}

bool text_style_is_canonical(const Terminal_text_style& style)
{
    return
        color_ref_is_canonical(style.foreground) &&
        color_ref_is_canonical(style.background) &&
        (style.attributes & ~k_terminal_style_attribute_known_mask) == 0U;
}

terminal_text_style_lookup_key_t color_ref_lookup_key(
    const Terminal_color_ref& color)
{
    terminal_text_style_lookup_key_t key{};
    key[0] = static_cast<std::uint64_t>(color.kind);
    switch (color.kind) {
        case Terminal_color_ref_kind::DEFAULT:
            break;
        case Terminal_color_ref_kind::PALETTE_INDEX:
            key[1] = color.palette_index;
            break;
        case Terminal_color_ref_kind::RGB:
            key[2] = color.rgba;
            break;
    }
    return key;
}

terminal_text_style_lookup_key_t terminal_text_style_lookup_key(
    const Terminal_text_style& style)
{
    const terminal_text_style_lookup_key_t foreground =
        color_ref_lookup_key(style.foreground);
    const terminal_text_style_lookup_key_t background =
        color_ref_lookup_key(style.background);
    return {
        foreground[0],
        foreground[1],
        foreground[2],
        background[0],
        background[1],
        background[2],
        style.attributes,
    };
}

bool style_table_entry_is_canonical(
    const Terminal_text_style&                  style,
    std::set<terminal_text_style_lookup_key_t>& style_keys)
{
    if (!text_style_is_canonical(style) ||
        style == make_default_terminal_text_style())
    {
        return false;
    }

    return style_keys.insert(terminal_text_style_lookup_key(style)).second;
}

class Byte_writer
{
public:
    explicit Byte_writer(std::span<std::byte> bytes)
    :
        m_bytes(bytes)
    {}

    bool write_u8(std::uint8_t value)
    {
        if (!can_write(1U)) {
            return false;
        }

        m_bytes[m_offset] = static_cast<std::byte>(value);
        ++m_offset;
        return true;
    }

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

    bool write_width_coded(std::uint32_t value, std::uint8_t width_code)
    {
        if (!width_code_is_valid(width_code)) {
            return false;
        }

        return write_unsigned(value, width_code_bytes(width_code));
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

    bool read_u8(std::uint8_t& value)
    {
        if (!can_read(1U)) {
            return false;
        }

        value = static_cast<std::uint8_t>(m_bytes[m_offset]);
        ++m_offset;
        return true;
    }

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

    bool read_width_coded(std::uint8_t width_code, std::uint32_t& value)
    {
        if (!width_code_is_valid(width_code)) {
            return false;
        }

        std::uint64_t raw = 0U;
        if (!read_unsigned(width_code_bytes(width_code), raw)) {
            return false;
        }

        value = static_cast<std::uint32_t>(raw);
        return true;
    }

    std::size_t offset() const { return m_offset; }
    std::size_t remaining() const { return m_bytes.size() - m_offset; }

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

std::optional<std::uint8_t> color_ref_kind_code(Terminal_color_ref_kind kind)
{
    switch (kind) {
        case Terminal_color_ref_kind::DEFAULT:
            return 0U;
        case Terminal_color_ref_kind::PALETTE_INDEX:
            return 1U;
        case Terminal_color_ref_kind::RGB:
            return 2U;
    }

    return std::nullopt;
}

std::optional<Terminal_color_ref_kind> color_ref_kind_from_code(std::uint8_t code)
{
    switch (code) {
        case 0U:
            return Terminal_color_ref_kind::DEFAULT;
        case 1U:
            return Terminal_color_ref_kind::PALETTE_INDEX;
        case 2U:
            return Terminal_color_ref_kind::RGB;
        default:
            return std::nullopt;
    }
}

bool write_color_ref(Byte_writer& writer, const Terminal_color_ref& color)
{
    const std::optional<std::uint8_t> kind = color_ref_kind_code(color.kind);
    return kind.has_value() &&
        writer.write_u8(*kind) &&
        writer.write_u16(color.palette_index) &&
        writer.write_u32(color.rgba);
}

Terminal_history_row_record_codec_status read_color_ref(
    Byte_reader&         reader,
    Terminal_color_ref&  color)
{
    std::uint8_t kind_code = 0U;
    std::uint16_t palette_index = 0U;
    std::uint32_t rgba = 0U;
    if (!reader.read_u8(kind_code) ||
        !reader.read_u16(palette_index) ||
        !reader.read_u32(rgba))
    {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    const std::optional<Terminal_color_ref_kind> kind =
        color_ref_kind_from_code(kind_code);
    if (!kind.has_value()) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    color = {*kind, palette_index, rgba};
    return Terminal_history_row_record_codec_status::OK;
}

bool write_text_style(Byte_writer& writer, const Terminal_text_style& style)
{
    if (!text_style_is_canonical(style)) {
        return false;
    }

    return
        write_color_ref(writer, style.foreground) &&
        write_color_ref(writer, style.background) &&
        writer.write_u16(style.attributes);
}

Terminal_history_row_record_codec_status read_text_style(
    Byte_reader&           reader,
    Terminal_text_style&   style)
{
    Terminal_text_style decoded;
    Terminal_history_row_record_codec_status status =
        read_color_ref(reader, decoded.foreground);
    if (status != Terminal_history_row_record_codec_status::OK) {
        return status;
    }

    status = read_color_ref(reader, decoded.background);
    if (status != Terminal_history_row_record_codec_status::OK) {
        return status;
    }

    if (!reader.read_u16(decoded.attributes)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    if (!text_style_is_canonical(decoded)) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    style = decoded;
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
        writer.write_u64(header.content_stamp_ms) &&
        writer.write_u32(header.source_width) &&
        writer.write_u32(header.cell_count) &&
        writer.write_u32(header.hyperlink_count) &&
        writer.write_u16(header.style_lifetime) &&
        writer.write_u16(header.wrap_state) &&
        writer.write_u16(header.provenance_source) &&
        writer.write_u16(header.style_count);
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
        reader.read_u64(header.content_stamp_ms) &&
        reader.read_u32(header.source_width) &&
        reader.read_u32(header.cell_count) &&
        reader.read_u32(header.hyperlink_count) &&
        reader.read_u16(header.style_lifetime) &&
        reader.read_u16(header.wrap_state) &&
        reader.read_u16(header.provenance_source) &&
        reader.read_u16(header.style_count);
}

bool write_table_length(Byte_writer& writer, std::uint32_t byte_count)
{
    const std::uint8_t width_code = shortest_width_code(byte_count);
    if (width_code == k_width_code_default) {
        return false;
    }

    return writer.write_u8(width_code) &&
        writer.write_width_coded(byte_count, width_code);
}

Terminal_history_row_record_codec_status read_table_length(
    Byte_reader&    reader,
    std::uint32_t&  byte_count)
{
    std::uint8_t width_code = 0U;
    if (!reader.read_u8(width_code)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    if (width_code == k_width_code_default || !width_code_is_valid(width_code)) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (!reader.read_width_coded(width_code, byte_count)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    if (byte_count == 0U || !width_code_is_shortest(byte_count, width_code)) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    return Terminal_history_row_record_codec_status::OK;
}

bool occupied_cell_has_printable_ascii_text(
    const Terminal_history_row_cell& cell,
    const QByteArray&                text_bytes)
{
    return
        cell.occupied            &&
        !cell.wide_continuation  &&
        cell.display_width == 1  &&
        text_bytes.size() == 1   &&
        byte_is_printable_ascii(static_cast<unsigned char>(text_bytes[0]));
}

bool cell_has_single_printable_ascii_text(
    const Terminal_history_row_cell& cell,
    std::uint8_t&                    ascii)
{
    if (cell.text.size() != 1) {
        return false;
    }

    const ushort code_unit = cell.text[0].unicode();
    if (code_unit > 0x7eU ||
        !byte_is_printable_ascii(static_cast<unsigned char>(code_unit)))
    {
        return false;
    }

    ascii = static_cast<std::uint8_t>(code_unit);
    return true;
}

bool cell_is_default_blank(const Terminal_history_row_cell& cell)
{
    return
        !cell.occupied           &&
        !cell.wide_continuation  &&
        cell.text == QStringLiteral(" ") &&
        cell.display_width == 1  &&
        cell.style_id == k_default_terminal_style_id &&
        cell.hyperlink_id == k_no_terminal_hyperlink_id;
}

bool cell_is_default_printable_ascii(
    const Terminal_history_row_cell& cell,
    std::uint8_t&                    ascii)
{
    return
        cell.occupied            &&
        !cell.wide_continuation  &&
        cell.display_width == 1  &&
        cell.style_id == k_default_terminal_style_id &&
        cell.hyperlink_id == k_no_terminal_hyperlink_id &&
        cell_has_single_printable_ascii_text(cell, ascii);
}

bool cell_is_default_printable_ascii(
    const Terminal_history_row_cell& cell,
    const QByteArray&                text_bytes)
{
    return
        occupied_cell_has_printable_ascii_text(cell, text_bytes) &&
        cell.style_id == k_default_terminal_style_id             &&
        cell.hyperlink_id == k_no_terminal_hyperlink_id;
}

Terminal_history_row_record_codec_status validate_cell_text(
    const Terminal_history_row_cell& cell,
    const QByteArray&                text_bytes)
{
    if (!cell.occupied || cell.wide_continuation) {
        return Terminal_history_row_record_codec_status::OK;
    }

    if (text_bytes.isEmpty()) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    const Terminal_utf8_width_result width =
        measure_utf8_width(QByteArrayView(text_bytes.constData(), text_bytes.size()));
    if (width.status != Terminal_unicode_width_status::OK ||
        width.cells != cell.display_width)
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status validate_cell_stream_shape(
    const Terminal_history_row_record&  record,
    const Encoded_record_parts&         parts,
    std::vector<bool>&                  referenced_style_refs,
    std::vector<bool>&                  referenced_hyperlink_refs)
{
    int remaining_wide_continuations = 0;
    Terminal_style_id inherited_style = k_default_terminal_style_id;
    Terminal_hyperlink_id inherited_hyperlink = k_no_terminal_hyperlink_id;
    const int source_width = record.metadata.source_width;
    for (std::size_t index = 0U; index < record.cells.size(); ++index) {
        const Terminal_history_row_cell& cell = record.cells[index];
        const Encoded_cell_part& cell_part = parts.cell_parts[index];
        const QByteArray& text_bytes = cell_part.text_bytes;
        const int column = static_cast<int>(index);

        if (cell.style_id >= referenced_style_refs.size() ||
            cell.hyperlink_id >= referenced_hyperlink_refs.size())
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        if (remaining_wide_continuations > 0) {
            if (!cell.wide_continuation ||
                !cell.occupied ||
                cell.display_width != 0 ||
                !cell.text.isEmpty() ||
                cell.style_id != inherited_style ||
                cell.hyperlink_id != inherited_hyperlink)
            {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }

            --remaining_wide_continuations;
            continue;
        }

        if (cell.wide_continuation) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        if (cell.occupied) {
            if (cell.display_width <= 0 ||
                cell.display_width > source_width - column)
            {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }

            if (!cell_part.default_printable_ascii) {
                const Terminal_history_row_record_codec_status text_status =
                    validate_cell_text(cell, text_bytes);
                if (text_status != Terminal_history_row_record_codec_status::OK) {
                    return text_status;
                }
            }

            if (cell.style_id != k_default_terminal_style_id) {
                referenced_style_refs[static_cast<std::size_t>(cell.style_id)] = true;
            }
            if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                referenced_hyperlink_refs[static_cast<std::size_t>(cell.hyperlink_id)] =
                    true;
            }

            if (cell.display_width > 1) {
                remaining_wide_continuations = cell.display_width - 1;
                inherited_style = cell.style_id;
                inherited_hyperlink = cell.hyperlink_id;
            }
            continue;
        }

        if (cell.text != QStringLiteral(" ") ||
            cell.display_width != 1 ||
            cell.hyperlink_id != k_no_terminal_hyperlink_id)
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        if (cell.style_id != k_default_terminal_style_id) {
            referenced_style_refs[static_cast<std::size_t>(cell.style_id)] = true;
        }
    }

    return remaining_wide_continuations == 0
        ? Terminal_history_row_record_codec_status::OK
        : Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
}

bool table_references_are_dense_from_one(const std::vector<bool>& refs)
{
    for (std::size_t index = 1U; index < refs.size(); ++index) {
        if (!refs[index]) {
            return false;
        }
    }
    return true;
}

Terminal_history_row_record_codec_status validate_and_measure_tables(
    const Terminal_history_row_record& record,
    std::size_t&                       payload_bytes)
{
    const std::size_t style_count = record.style_table.size();
    const std::size_t hyperlink_count = record.hyperlink_identity_keys.size();
    const std::size_t source_width =
        static_cast<std::size_t>(record.metadata.source_width);

    if (style_count > source_width || hyperlink_count > source_width) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (!count_fits_u16(style_count) || !count_fits_u32(hyperlink_count)) {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    std::size_t style_bytes = 0U;
    if (!checked_multiply(style_count, k_encoded_style_bytes, style_bytes) ||
        !checked_add(payload_bytes, style_bytes))
    {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    std::set<terminal_text_style_lookup_key_t> style_keys;
    for (const Terminal_text_style& style : record.style_table) {
        if (!style_table_entry_is_canonical(style, style_keys)) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
    }

    std::set<QByteArray> identity_keys;
    const Terminal_hyperlink_id last_hyperlink_ref =
        static_cast<Terminal_hyperlink_id>(hyperlink_count);
    for (Terminal_hyperlink_id ref = 1U; ref <= last_hyperlink_ref; ++ref) {
        const auto found = record.hyperlink_identity_keys.find(ref);
        if (found == record.hyperlink_identity_keys.end() ||
            found->second.isEmpty())
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        const bool unique_identity = identity_keys.insert(found->second).second;
        if (!unique_identity) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        const std::size_t key_bytes = static_cast<std::size_t>(found->second.size());
        if (!size_fits_u32(key_bytes)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }

        const std::uint8_t length_width =
            shortest_width_code(static_cast<std::uint32_t>(key_bytes));
        if (length_width == k_width_code_default ||
            !checked_add(payload_bytes, 1U) ||
            !checked_add(payload_bytes, width_code_bytes(length_width)) ||
            !checked_add(payload_bytes, key_bytes))
        {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }
    }

    return Terminal_history_row_record_codec_status::OK;
}

bool try_prepare_prefix_plain_ascii_stream(
    const Terminal_history_row_record& record,
    Encoded_record_parts&              parts,
    std::size_t&                       payload_bytes)
{
    if (!record.style_table.empty() || !record.hyperlink_identity_keys.empty()) {
        return false;
    }

    std::size_t prefix_bytes = 0U;
    bool in_default_blank_suffix = false;
    for (const Terminal_history_row_cell& cell : record.cells) {
        std::uint8_t printable_ascii = 0U;
        if (!in_default_blank_suffix &&
            cell_is_default_printable_ascii(cell, printable_ascii))
        {
            ++prefix_bytes;
            continue;
        }

        in_default_blank_suffix = true;
        if (!cell_is_default_blank(cell)) {
            return false;
        }
    }

    if (!checked_add(payload_bytes, prefix_bytes)) {
        return false;
    }

    parts.cell_parts.clear();
    parts.payload_kind = k_payload_kind_prefix_plain_ascii;
    parts.prefix_plain_ascii_bytes = prefix_bytes;
    return true;
}

std::size_t extended_cell_encoded_bytes(
    const Terminal_history_row_cell& cell,
    const QByteArray&                text_bytes)
{
    std::size_t byte_count = 2U;
    const bool occupied = cell.occupied;
    if (occupied) {
        const std::size_t text_byte_count = static_cast<std::size_t>(text_bytes.size());
        if (text_byte_count != 1U) {
            const std::uint8_t text_width =
                shortest_width_code(static_cast<std::uint32_t>(text_byte_count));
            byte_count += width_code_bytes(text_width);
        }
        if (cell.display_width != 1 ||
            cell.hyperlink_id != k_no_terminal_hyperlink_id)
        {
            ++byte_count;
        }
        if (cell.display_width != 1) {
            byte_count += width_code_bytes(
                shortest_width_code(static_cast<std::uint32_t>(cell.display_width)));
        }
    }

    if (cell.style_id != k_default_terminal_style_id) {
        byte_count += width_code_bytes(
            shortest_width_code(static_cast<std::uint32_t>(cell.style_id)));
    }
    if (occupied && cell.hyperlink_id != k_no_terminal_hyperlink_id) {
        byte_count += width_code_bytes(
            shortest_width_code(static_cast<std::uint32_t>(cell.hyperlink_id)));
    }
    if (occupied) {
        byte_count += static_cast<std::size_t>(text_bytes.size());
    }
    return byte_count;
}

Terminal_history_row_record_codec_status prepare_encoded_record_parts(
    const Terminal_history_row_record& record,
    Encoded_record_parts&              parts)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_history_row_record_codec::prepare_encode_parts");

    if (!source_width_is_supported(record.metadata.source_width) ||
        record.cells.size() != static_cast<std::size_t>(record.metadata.source_width) ||
        !count_fits_u32(record.cells.size()))
    {
        return Terminal_history_row_record_codec_status::INVALID_ARGUMENT;
    }

    if (!style_lifetime_code(record.metadata.style_lifetime).has_value() ||
        !wrap_state_code(record.metadata.wrap_state).has_value() ||
        !provenance_source_code(record.provenance.source).has_value())
    {
        return Terminal_history_row_record_codec_status::INVALID_ENUM;
    }

    std::size_t payload_bytes = k_row_record_header_bytes;
    const Terminal_history_row_record_codec_status table_status =
        validate_and_measure_tables(record, payload_bytes);
    if (table_status != Terminal_history_row_record_codec_status::OK) {
        return table_status;
    }

    parts = {};
    if (try_prepare_prefix_plain_ascii_stream(record, parts, payload_bytes)) {
        if (!size_fits_u32(payload_bytes)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }

        parts.payload_bytes = payload_bytes;
        return Terminal_history_row_record_codec_status::OK;
    }

    payload_bytes = k_row_record_header_bytes;
    const Terminal_history_row_record_codec_status generic_table_status =
        validate_and_measure_tables(record, payload_bytes);
    if (generic_table_status != Terminal_history_row_record_codec_status::OK) {
        return generic_table_status;
    }

    parts.cell_parts.clear();
    parts.cell_parts.reserve(record.cells.size());
    parts.payload_kind = k_payload_kind_generic_compact;
    parts.prefix_plain_ascii_bytes = 0U;

    std::vector<bool> referenced_style_refs(record.style_table.size() + 1U, false);
    std::vector<bool> referenced_hyperlink_refs(
        record.hyperlink_identity_keys.size() + 1U,
        false);

    for (const Terminal_history_row_cell& cell : record.cells) {
        Encoded_cell_part cell_part;

        if (cell_is_default_blank(cell) || cell.wide_continuation) {
            if (!checked_add(payload_bytes, 1U)) {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
        }
        else {
            std::uint8_t printable_ascii = 0U;
            if (cell_is_default_printable_ascii(cell, printable_ascii)) {
                cell_part.printable_ascii = printable_ascii;
                cell_part.default_printable_ascii = true;
                if (!checked_add(payload_bytes, 1U)) {
                    return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
                }
            }
            else {
                cell_part.text_bytes = cell.text.toUtf8();
                const std::size_t text_byte_count =
                    static_cast<std::size_t>(cell_part.text_bytes.size());
                if (!size_fits_u32(text_byte_count)) {
                    return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
                }

                if (!checked_add(
                        payload_bytes,
                        extended_cell_encoded_bytes(cell, cell_part.text_bytes)))
                {
                    return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
                }
            }
        }

        parts.cell_parts.push_back(std::move(cell_part));
    }

    const Terminal_history_row_record_codec_status shape_status =
        validate_cell_stream_shape(
            record,
            parts,
            referenced_style_refs,
            referenced_hyperlink_refs);
    if (shape_status != Terminal_history_row_record_codec_status::OK) {
        return shape_status;
    }

    if (!table_references_are_dense_from_one(referenced_style_refs) ||
        !table_references_are_dense_from_one(referenced_hyperlink_refs))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (!size_fits_u32(payload_bytes)) {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    parts.payload_bytes = payload_bytes;
    return Terminal_history_row_record_codec_status::OK;
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

std::uint8_t extended_extra_field_set(const Terminal_history_row_cell& cell)
{
    std::uint8_t extra = 0U;
    if (cell.occupied && cell.display_width != 1) {
        extra |= k_extended_extra_display;
    }
    if (cell.occupied && cell.hyperlink_id != k_no_terminal_hyperlink_id) {
        extra |= k_extended_extra_hyperlink;
    }
    return extra;
}

bool write_extended_cell(
    Byte_writer&                       writer,
    const Terminal_history_row_cell&   cell,
    const QByteArray&                  text_bytes)
{
    const std::uint8_t text_length_width =
        cell.occupied && text_bytes.size() != 1
            ? shortest_width_code(static_cast<std::uint32_t>(text_bytes.size()))
            : k_width_code_default;
    const std::uint8_t style_width =
        shortest_width_code(static_cast<std::uint32_t>(cell.style_id));
    const std::uint8_t kind = cell.occupied
        ? k_extended_kind_occupied
        : k_extended_kind_unoccupied_styled;
    const std::uint8_t extra = extended_extra_field_set(cell);
    const std::uint8_t tag0 =
        text_length_width |
        static_cast<std::uint8_t>(style_width << 2U) |
        static_cast<std::uint8_t>(kind << 4U) |
        static_cast<std::uint8_t>(extra << 6U);

    if (!writer.write_u8(k_opcode_extended_cell) ||
        !writer.write_u8(tag0))
    {
        return false;
    }

    std::uint8_t display_width = k_width_code_default;
    std::uint8_t hyperlink_width = k_width_code_default;
    if ((extra & k_extended_extra_display) != 0U) {
        display_width = shortest_width_code(
            static_cast<std::uint32_t>(cell.display_width));
    }
    if ((extra & k_extended_extra_hyperlink) != 0U) {
        hyperlink_width = shortest_width_code(
            static_cast<std::uint32_t>(cell.hyperlink_id));
    }
    if (extra != 0U) {
        const std::uint8_t tag1 =
            display_width |
            static_cast<std::uint8_t>(hyperlink_width << 2U);
        if (!writer.write_u8(tag1)) {
            return false;
        }
    }

    if (cell.occupied && text_length_width != k_width_code_default) {
        if (!writer.write_width_coded(
                static_cast<std::uint32_t>(text_bytes.size()),
                text_length_width))
        {
            return false;
        }
    }
    if ((extra & k_extended_extra_display) != 0U) {
        if (!writer.write_width_coded(
                static_cast<std::uint32_t>(cell.display_width),
                display_width))
        {
            return false;
        }
    }
    if (!writer.write_width_coded(static_cast<std::uint32_t>(cell.style_id), style_width)) {
        return false;
    }
    if ((extra & k_extended_extra_hyperlink) != 0U) {
        if (!writer.write_width_coded(
                static_cast<std::uint32_t>(cell.hyperlink_id),
                hyperlink_width))
        {
            return false;
        }
    }

    return !cell.occupied || writer.write_bytes(text_bytes);
}

Terminal_history_row_record_codec_status write_prefix_plain_ascii_stream(
    Byte_writer&                       writer,
    const Terminal_history_row_record& record,
    const Encoded_record_parts&        parts)
{
    for (std::size_t i = 0U; i < parts.prefix_plain_ascii_bytes; ++i) {
        std::uint8_t printable_ascii = 0U;
        if (!cell_is_default_printable_ascii(record.cells[i], printable_ascii)) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
        if (!writer.write_u8(printable_ascii)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status write_cell_stream(
    Byte_writer&                       writer,
    const Terminal_history_row_record& record,
    const Encoded_record_parts&        parts)
{
    for (std::size_t i = 0U; i < record.cells.size(); ++i) {
        const Terminal_history_row_cell& cell = record.cells[i];
        const Encoded_cell_part& cell_part = parts.cell_parts[i];
        const QByteArray& text_bytes = cell_part.text_bytes;

        if (cell_is_default_blank(cell)) {
            if (!writer.write_u8(k_opcode_default_blank)) {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
            continue;
        }

        if (cell.wide_continuation) {
            if (!writer.write_u8(k_opcode_wide_continuation)) {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
            continue;
        }

        if (cell_part.default_printable_ascii) {
            if (!writer.write_u8(cell_part.printable_ascii)) {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
            continue;
        }

        if (!write_extended_cell(writer, cell, text_bytes)) {
            return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
        }
    }

    return Terminal_history_row_record_codec_status::OK;
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
    if (!provenance.has_value() || !style.has_value() || !wrap.has_value()) {
        return Terminal_history_row_record_codec_status::INVALID_ENUM;
    }

    row_record_header_t header;
    header.payload_bytes = static_cast<std::uint32_t>(target.size());
    header.record_bytes = record_bytes;
    header.flags = parts.payload_kind;
    header.epoch = identity.epoch;
    header.byte_sequence = byte_sequence;
    header.row_sequence = identity.row_sequence;
    header.previous_row_byte_sequence = identity.previous_row_byte_sequence;
    header.previous_row_sequence = identity.previous_row_sequence;
    header.content_generation = record.provenance.content_generation;
    header.retained_line_id = record.provenance.retained_line_id;
    header.retained_line_content_generation = record.provenance.content_generation;
    header.content_stamp_ms = static_cast<std::uint64_t>(record.provenance.content_stamp_ms);
    header.source_width = static_cast<std::uint32_t>(record.metadata.source_width);
    header.cell_count = static_cast<std::uint32_t>(record.cells.size());
    header.hyperlink_count = static_cast<std::uint32_t>(
        record.hyperlink_identity_keys.size());
    header.style_lifetime = *style;
    header.wrap_state = *wrap;
    header.provenance_source = *provenance;
    header.style_count = static_cast<std::uint16_t>(record.style_table.size());

    Byte_writer writer(target);
    if (!write_header(writer, header)) {
        return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::write_payload::styles");

        for (const Terminal_text_style& style_entry : record.style_table) {
            if (!write_text_style(writer, style_entry)) {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::write_payload::hyperlinks");

        const Terminal_hyperlink_id last_hyperlink_ref =
            static_cast<Terminal_hyperlink_id>(header.hyperlink_count);
        for (Terminal_hyperlink_id ref = 1U; ref <= last_hyperlink_ref; ++ref) {
            const QByteArray& identity_key = record.hyperlink_identity_keys.at(ref);
            if (!write_table_length(writer, static_cast<std::uint32_t>(identity_key.size())) ||
                !writer.write_bytes(identity_key))
            {
                return Terminal_history_row_record_codec_status::SIZE_OVERFLOW;
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::write_payload::cells");

        const Terminal_history_row_record_codec_status stream_status =
            parts.payload_kind == k_payload_kind_prefix_plain_ascii
                ? write_prefix_plain_ascii_stream(writer, record, parts)
                : write_cell_stream(writer, record, parts);
        if (stream_status != Terminal_history_row_record_codec_status::OK) {
            return stream_status;
        }
    }

    if (writer.offset() != target.size()) {
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
        header.header_bytes != k_row_record_header_bytes)
    {
        return Terminal_history_row_record_codec_status::INVALID_HEADER;
    }

    if ((header.flags & ~k_payload_kind_mask) != 0U ||
        !payload_kind_is_supported(payload_kind_from_flags(header.flags)))
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
    const std::uint32_t payload_kind = payload_kind_from_flags(header.flags);
    if (header.source_width == 0U ||
        header.source_width > k_terminal_screen_model_max_columns ||
        header.cell_count != header.source_width ||
        header.style_count > header.source_width ||
        header.hyperlink_count > header.source_width)
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (payload_kind == k_payload_kind_prefix_plain_ascii &&
        (header.style_count != 0U || header.hyperlink_count != 0U))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    std::size_t minimum_payload_bytes = k_row_record_header_bytes;
    std::size_t style_bytes = 0U;
    if (!checked_multiply(header.style_count, k_encoded_style_bytes, style_bytes) ||
        !checked_add(minimum_payload_bytes, style_bytes))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    const std::size_t minimum_hyperlink_bytes =
        static_cast<std::size_t>(header.hyperlink_count) * 3U;
    if (!checked_add(minimum_payload_bytes, minimum_hyperlink_bytes))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (payload_kind == k_payload_kind_generic_compact &&
        !checked_add(minimum_payload_bytes, header.cell_count))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (minimum_payload_bytes > header.payload_bytes) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (payload_kind == k_payload_kind_prefix_plain_ascii &&
        header.payload_bytes - minimum_payload_bytes > header.source_width)
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_style_table(
    Byte_reader&                 reader,
    const row_record_header_t&    header,
    Terminal_history_row_record&  record)
{
    record.style_table.reserve(header.style_count);
    std::set<terminal_text_style_lookup_key_t> style_keys;
    for (std::uint32_t i = 0U; i < header.style_count; ++i) {
        Terminal_text_style style;
        const Terminal_history_row_record_codec_status status =
            read_text_style(reader, style);
        if (status != Terminal_history_row_record_codec_status::OK) {
            return status;
        }

        if (!style_table_entry_is_canonical(style, style_keys)) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        record.style_table.push_back(style);
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_hyperlink_table(
    Byte_reader&                 reader,
    const row_record_header_t&    header,
    Terminal_history_row_record&  record)
{
    std::set<QByteArray> identity_keys;
    const Terminal_hyperlink_id last_hyperlink_ref =
        static_cast<Terminal_hyperlink_id>(header.hyperlink_count);
    for (Terminal_hyperlink_id ref = 1U; ref <= last_hyperlink_ref; ++ref) {
        std::uint32_t identity_key_bytes = 0U;
        QByteArray identity_key;

        const Terminal_history_row_record_codec_status length_status =
            read_table_length(reader, identity_key_bytes);
        if (length_status != Terminal_history_row_record_codec_status::OK) {
            return length_status;
        }

        if (!reader.read_bytes(identity_key_bytes, identity_key)) {
            return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        }

        if (identity_key.isEmpty()) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        const bool unique_identity = identity_keys.insert(identity_key).second;
        if (!unique_identity) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        record.hyperlink_identity_keys.emplace(ref, std::move(identity_key));
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_prefix_plain_ascii_stream(
    Byte_reader&                 reader,
    const row_record_header_t&    header,
    Terminal_history_row_record&  record)
{
    if (header.style_count != 0U || header.hyperlink_count != 0U) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    const std::size_t prefix_bytes = reader.remaining();
    if (prefix_bytes > header.source_width) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    record.cells.reserve(header.cell_count);
    for (std::size_t i = 0U; i < prefix_bytes; ++i) {
        std::uint8_t printable_ascii = 0U;
        if (!reader.read_u8(printable_ascii)) {
            return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        }
        if (!byte_is_printable_ascii(printable_ascii)) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        Terminal_history_row_cell cell;
        cell.text = QString(QChar(static_cast<ushort>(printable_ascii)));
        cell.display_width = 1;
        cell.occupied = true;
        record.cells.push_back(std::move(cell));
    }

    record.cells.resize(header.cell_count);
    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_extended_value(
    Byte_reader&     reader,
    std::uint8_t     width_code,
    std::uint32_t&   value)
{
    if (!reader.read_width_coded(width_code, value)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    if (!width_code_is_shortest(value, width_code) ||
        (width_code != k_width_code_default && value == 0U))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_extended_cell(
    Byte_reader&                  reader,
    const row_record_header_t&     header,
    Terminal_history_row_cell&     cell,
    QByteArray&                    encoded_text)
{
    std::uint8_t tag0 = 0U;
    if (!reader.read_u8(tag0)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    const std::uint8_t text_length_width = tag0 & 0x03U;
    const std::uint8_t style_width       = (tag0 >> 2U) & 0x03U;
    const std::uint8_t cell_kind         = (tag0 >> 4U) & 0x03U;
    const std::uint8_t extra             = (tag0 >> 6U) & 0x03U;
    if (cell_kind != k_extended_kind_occupied &&
        cell_kind != k_extended_kind_unoccupied_styled)
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    std::uint8_t display_width_width = k_width_code_default;
    std::uint8_t hyperlink_width = k_width_code_default;
    if (extra != 0U) {
        std::uint8_t tag1 = 0U;
        if (!reader.read_u8(tag1)) {
            return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        }

        if ((tag1 & 0xf0U) != 0U) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        display_width_width = tag1 & 0x03U;
        hyperlink_width = (tag1 >> 2U) & 0x03U;
        const bool display_present = (extra & k_extended_extra_display) != 0U;
        const bool hyperlink_present = (extra & k_extended_extra_hyperlink) != 0U;
        if (display_present == (display_width_width == k_width_code_default) ||
            hyperlink_present == (hyperlink_width == k_width_code_default))
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
    }

    cell.occupied = cell_kind == k_extended_kind_occupied;
    if (!cell.occupied && (extra != 0U || text_length_width != k_width_code_default)) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    std::uint32_t text_byte_count = cell.occupied ? 1U : 0U;
    if (cell.occupied && text_length_width != k_width_code_default) {
        const Terminal_history_row_record_codec_status status =
            read_extended_value(reader, text_length_width, text_byte_count);
        if (status != Terminal_history_row_record_codec_status::OK) {
            return status;
        }

        if (text_byte_count == 1U) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
    }

    if ((extra & k_extended_extra_display) != 0U) {
        std::uint32_t display_width = 0U;
        const Terminal_history_row_record_codec_status status =
            read_extended_value(reader, display_width_width, display_width);
        if (status != Terminal_history_row_record_codec_status::OK) {
            return status;
        }

        if (display_width <= 1U ||
            display_width > header.source_width ||
            display_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
        cell.display_width = static_cast<int>(display_width);
    }
    else {
        cell.display_width = 1;
    }

    std::uint32_t style_ref = 0U;
    {
        const Terminal_history_row_record_codec_status status =
            read_extended_value(reader, style_width, style_ref);
        if (status != Terminal_history_row_record_codec_status::OK) {
            return status;
        }
    }
    if (style_ref > header.style_count) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }
    cell.style_id = style_ref;

    if ((extra & k_extended_extra_hyperlink) != 0U) {
        Terminal_hyperlink_id hyperlink_ref = k_no_terminal_hyperlink_id;
        const Terminal_history_row_record_codec_status status =
            read_extended_value(reader, hyperlink_width, hyperlink_ref);
        if (status != Terminal_history_row_record_codec_status::OK) {
            return status;
        }

        if (hyperlink_ref > header.hyperlink_count) {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }
        cell.hyperlink_id = hyperlink_ref;
    }

    if (!cell.occupied) {
        cell.text = QStringLiteral(" ");
        return cell.style_id != k_default_terminal_style_id
            ? Terminal_history_row_record_codec_status::OK
            : Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (!reader.read_bytes(text_byte_count, encoded_text)) {
        return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
    }

    if (cell_is_default_printable_ascii(cell, encoded_text)) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    const Terminal_history_row_record_codec_status text_status =
        validate_cell_text(cell, encoded_text);
    if (text_status != Terminal_history_row_record_codec_status::OK) {
        return text_status;
    }

    cell.text = QString::fromUtf8(encoded_text);
    return Terminal_history_row_record_codec_status::OK;
}

Terminal_history_row_record_codec_status read_cell_stream(
    Byte_reader&                  reader,
    const row_record_header_t&     header,
    Terminal_history_row_record&   record)
{
    record.cells.reserve(header.cell_count);
    std::vector<bool> referenced_style_refs(
        static_cast<std::size_t>(header.style_count) + 1U,
        false);
    std::vector<bool> referenced_hyperlink_refs(
        static_cast<std::size_t>(header.hyperlink_count) + 1U,
        false);

    int remaining_wide_continuations = 0;
    Terminal_style_id inherited_style = k_default_terminal_style_id;
    Terminal_hyperlink_id inherited_hyperlink = k_no_terminal_hyperlink_id;
    for (std::uint32_t column = 0U; column < header.cell_count; ++column) {
        std::uint8_t opcode = 0U;
        if (!reader.read_u8(opcode)) {
            return Terminal_history_row_record_codec_status::TRUNCATED_RECORD;
        }

        Terminal_history_row_cell cell;
        QByteArray encoded_text;
        if (opcode == k_opcode_default_blank) {
            if (remaining_wide_continuations > 0) {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }
            cell = {};
        }
        else
        if (opcode == k_opcode_wide_continuation) {
            if (remaining_wide_continuations <= 0) {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }

            cell.text.clear();
            cell.display_width = 0;
            cell.wide_continuation = true;
            cell.occupied = true;
            cell.style_id = inherited_style;
            cell.hyperlink_id = inherited_hyperlink;
            --remaining_wide_continuations;
        }
        else
        if (byte_is_printable_ascii(opcode)) {
            if (remaining_wide_continuations > 0) {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }

            cell.text = QString(QChar(static_cast<ushort>(opcode)));
            cell.display_width = 1;
            cell.occupied = true;
        }
        else
        if (opcode == k_opcode_extended_cell) {
            if (remaining_wide_continuations > 0) {
                return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
            }

            const Terminal_history_row_record_codec_status status =
                read_extended_cell(reader, header, cell, encoded_text);
            if (status != Terminal_history_row_record_codec_status::OK) {
                return status;
            }
        }
        else {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        if (cell.style_id > header.style_count ||
            cell.hyperlink_id > header.hyperlink_count)
        {
            return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
        }

        if (!cell.wide_continuation) {
            if (cell.occupied && cell.display_width > 1) {
                if (cell.display_width >
                    static_cast<int>(header.cell_count - column))
                {
                    return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
                }
                remaining_wide_continuations = cell.display_width - 1;
                inherited_style = cell.style_id;
                inherited_hyperlink = cell.hyperlink_id;
            }

            if (cell.style_id != k_default_terminal_style_id) {
                referenced_style_refs[static_cast<std::size_t>(cell.style_id)] = true;
            }
            if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                referenced_hyperlink_refs[static_cast<std::size_t>(cell.hyperlink_id)] =
                    true;
            }
        }

        record.cells.push_back(std::move(cell));
    }

    if (remaining_wide_continuations != 0) {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
    }

    if (!table_references_are_dense_from_one(referenced_style_refs) ||
        !table_references_are_dense_from_one(referenced_hyperlink_refs))
    {
        return Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
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

    if (payload_view.payload.size() < k_row_record_header_bytes) {
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
        static_cast<qint64>(header.content_stamp_ms),
    };
    record.metadata.source_width = static_cast<int>(header.source_width);
    record.metadata.style_lifetime = *style;
    record.metadata.wrap_state = *wrap;

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::decode_payload::materialize_styles");
        result.status = read_style_table(reader, header, record);
        if (result.status != Terminal_history_row_record_codec_status::OK) {
            return result;
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::decode_payload::materialize_hyperlinks");
        result.status = read_hyperlink_table(reader, header, record);
        if (result.status != Terminal_history_row_record_codec_status::OK) {
            return result;
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_history_row_record_codec::decode_payload::materialize_cells");
        result.status =
            payload_kind_from_flags(header.flags) == k_payload_kind_prefix_plain_ascii
                ? read_prefix_plain_ascii_stream(reader, header, record)
                : read_cell_stream(reader, header, record);
        if (result.status != Terminal_history_row_record_codec_status::OK) {
            return result;
        }
    }

    if (reader.offset() != payload_view.payload.size()) {
        result.status = Terminal_history_row_record_codec_status::INVALID_PAYLOAD;
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
