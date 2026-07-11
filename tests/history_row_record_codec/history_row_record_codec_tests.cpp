#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr std::size_t k_header_bytes = 116U;
constexpr std::size_t k_header_payload_bytes_offset = 12U;
constexpr std::size_t k_header_record_bytes_offset = 16U;
constexpr std::size_t k_header_flags_offset = 20U;
constexpr std::size_t k_header_source_width_offset = 96U;
constexpr std::size_t k_header_cell_count_offset = 100U;
constexpr std::size_t k_header_hyperlink_count_offset = 104U;
constexpr std::size_t k_header_style_count_offset = 114U;
constexpr std::size_t k_encoded_style_bytes = 16U;
constexpr std::uint32_t k_payload_kind_mask = 0x0fU;
constexpr std::uint32_t k_payload_kind_generic_compact = 0U;
constexpr std::uint32_t k_payload_kind_prefix_plain_ascii = 1U;
constexpr std::uint32_t k_149_column_prefix_ascii_record_bytes_with_ring_overhead = 305U;
constexpr term::Terminal_hyperlink_id k_first_hyperlink_ref = 1U;
constexpr term::Terminal_hyperlink_id k_second_hyperlink_ref = 2U;

void write_le_u16(
    std::vector<std::byte>& bytes,
    std::size_t             offset,
    std::uint16_t           value)
{
    for (std::size_t i = 0U; i < 2U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
}

void write_le_u32(
    std::vector<std::byte>& bytes,
    std::size_t             offset,
    std::uint32_t           value)
{
    for (std::size_t i = 0U; i < 4U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
}

std::uint32_t read_le_u32(
    const std::vector<std::byte>& bytes,
    std::size_t                   offset)
{
    std::uint32_t value = 0U;
    for (std::size_t i = 0U; i < 4U; ++i) {
        value |=
            static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

std::uint32_t payload_kind(const std::vector<std::byte>& payload)
{
    return read_le_u32(payload, k_header_flags_offset) & k_payload_kind_mask;
}

term::Terminal_text_style red_style()
{
    term::Terminal_text_style style = term::make_default_terminal_text_style();
    style.foreground = term::make_rgb_terminal_color_ref(0xffcc0000U);
    return style;
}

term::Terminal_text_style palette_style()
{
    term::Terminal_text_style style = term::make_default_terminal_text_style();
    style.foreground = term::make_palette_terminal_color_ref(1U);
    return style;
}

term::Terminal_history_row_cell make_cell(
    QString                 text,
    int                     display_width,
    bool                    occupied,
    term::Terminal_style_id style_id = term::k_default_terminal_style_id,
    term::Terminal_hyperlink_id hyperlink_id = term::k_no_terminal_hyperlink_id)
{
    term::Terminal_history_row_cell cell;
    cell.text          = std::move(text);
    cell.display_width = display_width;
    cell.occupied      = occupied;
    cell.style_id      = style_id;
    cell.hyperlink_id  = hyperlink_id;
    return cell;
}

term::Terminal_history_row_cell make_wide_continuation(
    term::Terminal_style_id style_id,
    term::Terminal_hyperlink_id hyperlink_id)
{
    term::Terminal_history_row_cell cell;
    cell.text.clear();
    cell.display_width     = 0;
    cell.wide_continuation = true;
    cell.occupied          = true;
    cell.style_id          = style_id;
    cell.hyperlink_id      = hyperlink_id;
    return cell;
}

term::Terminal_history_row_record make_base_record(
    std::uint64_t                                  retained_line_id,
    std::uint64_t                                  content_generation,
    term::Terminal_retained_line_provenance_source source,
    int                                            source_width)
{
    term::Terminal_history_row_record record;
    record.provenance.retained_line_id   = retained_line_id;
    record.provenance.content_generation = content_generation;
    record.provenance.source             = source;
    record.metadata.source_width         = source_width;
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;
    return record;
}

term::terminal_history_row_record_identity_t make_identity(
    std::uint64_t epoch,
    std::uint64_t row_sequence)
{
    return {
        epoch,
        row_sequence,
        0U,
        0U,
    };
}

bool cells_equal(
    const term::Terminal_history_row_cell& left,
    const term::Terminal_history_row_cell& right)
{
    return
        left.text              == right.text              &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.occupied          == right.occupied          &&
        left.style_id          == right.style_id          &&
        left.hyperlink_id      == right.hyperlink_id;
}

bool records_equal(
    const term::Terminal_history_row_record& left,
    const term::Terminal_history_row_record& right)
{
    return
        left.cells.size() == right.cells.size() &&
        std::equal(
            left.cells.begin(),
            left.cells.end(),
            right.cells.begin(),
            cells_equal) &&
        left.style_table == right.style_table &&
        left.provenance.retained_line_id   == right.provenance.retained_line_id &&
        left.provenance.content_generation == right.provenance.content_generation &&
        left.provenance.content_stamp_ms   == right.provenance.content_stamp_ms &&
        left.provenance.source             == right.provenance.source &&
        left.hyperlink_identity_keys       == right.hyperlink_identity_keys &&
        left.metadata.source_width         == right.metadata.source_width &&
        left.metadata.style_reference      == right.metadata.style_reference &&
        left.metadata.wrap_state           == right.metadata.wrap_state;
}

term::Terminal_history_row_record_decode_result append_and_decode(
    term::Terminal_history_ring&                     ring,
    const term::Terminal_history_row_record&         record,
    term::terminal_history_row_record_identity_t     identity,
    term::Terminal_history_row_record_append_result& append)
{
    append = term::encode_terminal_history_row_record_to_ring(
        ring,
        record,
        identity);
    if (append.status != term::Terminal_history_row_record_codec_status::OK) {
        return {};
    }

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    return term::decode_terminal_history_row_record(read, append.history_handle);
}

std::vector<std::byte> payload_bytes(
    term::Terminal_history_ring&                         ring,
    const term::Terminal_history_row_record_append_result& append)
{
    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    return {read.payload().begin(), read.payload().end()};
}

std::uint32_t record_bytes_for_payload(const std::vector<std::byte>& payload)
{
    return static_cast<std::uint32_t>(
        payload.size() + term::terminal_history_ring_record_overhead_bytes());
}

void refresh_payload_size_fields(std::vector<std::byte>& payload)
{
    write_le_u32(
        payload,
        k_header_payload_bytes_offset,
        static_cast<std::uint32_t>(payload.size()));
    write_le_u32(
        payload,
        k_header_record_bytes_offset,
        record_bytes_for_payload(payload));
}

std::size_t width_coded_byte_count(std::size_t value)
{
    if (value <= std::numeric_limits<std::uint8_t>::max()) {
        return 1U;
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return 2U;
    }
    return 4U;
}

std::size_t table_length_encoded_byte_count(std::size_t value)
{
    return 1U + width_coded_byte_count(value);
}

std::size_t cell_stream_offset(const term::Terminal_history_row_record& record)
{
    std::size_t offset =
        k_header_bytes + (record.style_table.size() * k_encoded_style_bytes);
    const term::Terminal_hyperlink_id last_hyperlink_ref =
        static_cast<term::Terminal_hyperlink_id>(record.hyperlink_identity_keys.size());
    for (term::Terminal_hyperlink_id ref = k_first_hyperlink_ref;
         ref <= last_hyperlink_ref;
         ++ref)
    {
        const std::size_t key_bytes =
            static_cast<std::size_t>(record.hyperlink_identity_keys.at(ref).size());
        offset += table_length_encoded_byte_count(key_bytes) + key_bytes;
    }
    return offset;
}

term::Terminal_history_row_record_decode_result decode_mutated_payload(
    const term::Terminal_history_ring_read_scope& read,
    const std::vector<std::byte>&                 payload)
{
    return term::decode_terminal_history_row_record_payload(
        {
            read.byte_sequence(),
            record_bytes_for_payload(payload),
            payload,
        },
        std::nullopt);
}

bool test_prefix_plain_ascii_rows_use_prefix_payload()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "prefix plain ASCII fixture ring initializes");

    term::Terminal_history_row_record blank = make_base_record(
        11U,
        17U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        4);
    blank.cells.resize(4U);

    term::Terminal_history_row_record_append_result blank_append;
    const term::Terminal_history_row_record_decode_result decoded_blank =
        append_and_decode(ring, blank, make_identity(3U, 11U), blank_append);
    ok &= check(blank_append.status == term::Terminal_history_row_record_codec_status::OK,
        "default blank row encodes");
    ok &= check(decoded_blank.status == term::Terminal_history_row_record_codec_status::OK,
        "default blank row decodes");
    ok &= check(records_equal(decoded_blank.record, blank),
        "default blank row round-trips");
    ok &= check(blank_append.commit.record_bytes ==
            term::terminal_history_ring_record_overhead_bytes() + k_header_bytes,
        "default blank row stores a zero-byte prefix payload plus row and ring headers");
    ok &= check(payload_kind(payload_bytes(ring, blank_append)) ==
            k_payload_kind_prefix_plain_ascii,
        "default blank row uses prefix plain ASCII payload kind 1");

    term::Terminal_history_row_record sparse_ascii = make_base_record(
        12U,
        18U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        4);
    sparse_ascii.cells.push_back(make_cell(QStringLiteral("A"), 1, true));
    sparse_ascii.cells.push_back(make_cell(QStringLiteral("B"), 1, true));
    sparse_ascii.cells.resize(4U);

    term::Terminal_history_row_record_append_result sparse_append;
    const term::Terminal_history_row_record_decode_result decoded_sparse =
        append_and_decode(ring, sparse_ascii, make_identity(3U, 12U), sparse_append);
    ok &= check(sparse_append.status == term::Terminal_history_row_record_codec_status::OK,
        "sparse prefix printable ASCII row encodes");
    ok &= check(decoded_sparse.status == term::Terminal_history_row_record_codec_status::OK,
        "sparse prefix printable ASCII row decodes");
    ok &= check(records_equal(decoded_sparse.record, sparse_ascii),
        "sparse prefix printable ASCII row round-trips");
    ok &= check(sparse_append.commit.record_bytes ==
            term::terminal_history_ring_record_overhead_bytes() + k_header_bytes + 2U,
        "sparse prefix printable ASCII row stores only occupied prefix bytes");
    ok &= check(payload_kind(payload_bytes(ring, sparse_append)) ==
            k_payload_kind_prefix_plain_ascii,
        "sparse prefix printable ASCII row uses prefix plain ASCII payload kind 1");

    term::Terminal_history_row_record ascii = make_base_record(
        13U,
        19U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        4);
    ascii.cells.push_back(make_cell(QStringLiteral("A"), 1, true));
    ascii.cells.push_back(make_cell(QStringLiteral("B"), 1, true));
    ascii.cells.push_back(make_cell(QStringLiteral("C"), 1, true));
    ascii.cells.push_back(make_cell(QStringLiteral("D"), 1, true));

    term::Terminal_history_row_record_append_result ascii_append;
    const term::Terminal_history_row_record_decode_result decoded_ascii =
        append_and_decode(ring, ascii, make_identity(3U, 13U), ascii_append);
    ok &= check(ascii_append.status == term::Terminal_history_row_record_codec_status::OK,
        "full-width printable ASCII row encodes");
    ok &= check(decoded_ascii.status == term::Terminal_history_row_record_codec_status::OK,
        "full-width printable ASCII row decodes");
    ok &= check(records_equal(decoded_ascii.record, ascii),
        "full-width printable ASCII row round-trips");
    ok &= check(ascii_append.commit.record_bytes ==
            term::terminal_history_ring_record_overhead_bytes() + k_header_bytes + 4U,
        "full-width printable ASCII row stores one prefix byte per occupied column");
    ok &= check(payload_kind(payload_bytes(ring, ascii_append)) ==
            k_payload_kind_prefix_plain_ascii,
        "full-width printable ASCII row uses prefix plain ASCII payload kind 1");

    term::Terminal_history_ring budget_ring({4096U, 4096U});
    term::Terminal_history_row_record budget = make_base_record(
        14U,
        20U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        149);
    for (int column = 0; column < 149; ++column) {
        const char text = static_cast<char>('a' + (column % 26));
        budget.cells.push_back(make_cell(QString(QChar(QLatin1Char(text))), 1, true));
    }

    const term::Terminal_history_row_record_append_result budget_append =
        term::encode_terminal_history_row_record_to_ring(
            budget_ring,
            budget,
            make_identity(3U, 14U));
    ok &= check(budget_append.status == term::Terminal_history_row_record_codec_status::OK,
        "149-column prefix printable ASCII budget row encodes");
    ok &= check(budget_append.commit.record_bytes <=
            k_149_column_prefix_ascii_record_bytes_with_ring_overhead,
        "149-column prefix printable ASCII record is measured including the 40-byte "
        "ring overhead and stays <= 305 bytes");
    const term::terminal_history_prefix_plain_ascii_retention_estimate_t estimate =
        term::make_terminal_history_prefix_plain_ascii_retention_estimate(
            budget_ring.capacity_bytes(),
            149);
    ok &= check(
        estimate.contract_version ==
            term::k_terminal_history_retention_estimate_contract_version &&
        estimate.source_width_columns == 149U &&
        estimate.record_bytes == budget_append.commit.record_bytes &&
        estimate.target_rows == term::k_terminal_history_retention_target_rows &&
        estimate.max_columns_at_target_rows == 0U,
        "prefix plain-ASCII retention estimate owns its version, target, and byte arithmetic");
    ok &= check(payload_kind(payload_bytes(budget_ring, budget_append)) ==
            k_payload_kind_prefix_plain_ascii,
        "149-column budget row uses prefix plain ASCII payload kind 1");

    return ok;
}

bool test_extended_styles_hyperlinks_and_wide_cells_round_trip()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record record = make_base_record(
        91U,
        23U,
        term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        4);
    record.style_table.push_back(red_style());
    record.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/wide"));
    record.cells.push_back(make_cell(
        QString::fromUtf8("\xc3\xa9"),
        1,
        true,
        1U,
        term::k_no_terminal_hyperlink_id));
    record.cells.push_back(make_cell(
        QStringLiteral(" "),
        1,
        false,
        1U,
        term::k_no_terminal_hyperlink_id));
    record.cells.push_back(make_cell(
        QString::fromUtf8("\xe7\x95\x8c"),
        2,
        true,
        1U,
        k_first_hyperlink_ref));
    record.cells.push_back(make_wide_continuation(1U, k_first_hyperlink_ref));

    term::terminal_history_row_record_identity_t identity = make_identity(4U, 91U);
    identity.previous_row_byte_sequence = 640U;
    identity.previous_row_sequence      = 90U;

    term::Terminal_history_row_record_append_result append;
    const term::Terminal_history_row_record_decode_result decoded =
        append_and_decode(ring, record, identity, append);
    ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
        "extended styled hyperlink row encodes");
    ok &= check(decoded.status == term::Terminal_history_row_record_codec_status::OK,
        "extended styled hyperlink row decodes");
    ok &= check(records_equal(decoded.record, record),
        "extended row preserves style values, hyperlink identity keys, wide spans, and provenance");
    ok &= check(decoded.previous_row_byte_sequence == identity.previous_row_byte_sequence &&
            decoded.previous_row_sequence == identity.previous_row_sequence,
        "extended row preserves previous-row traversal metadata");
    ok &= check(decoded.history_handle == append.history_handle,
        "extended row decoded handle matches the committed ring identity");

    const std::vector<std::byte> payload = payload_bytes(ring, append);
    ok &= check(payload.back() == static_cast<std::byte>(0x01U),
        "wide continuation is encoded only with the inherited continuation opcode");

    return ok;
}

bool test_generic_compact_default_ascii_after_blank_round_trip()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record record = make_base_record(
        96U,
        32U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        3);
    record.provenance.content_stamp_ms = 123456789LL;
    record.cells.resize(1U);
    record.cells.push_back(make_cell(QStringLiteral("A"), 1, true));
    record.cells.resize(3U);

    term::Terminal_history_row_record_append_result append;
    const term::Terminal_history_row_record_decode_result decoded =
        append_and_decode(ring, record, make_identity(5U, 96U), append);
    ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
        "generic compact blank-prefix ASCII fixture encodes");
    ok &= check(decoded.status == term::Terminal_history_row_record_codec_status::OK,
        "generic compact blank-prefix ASCII fixture decodes");
    ok &= check(records_equal(decoded.record, record),
        "generic compact ASCII opcode after a blank round-trips with provenance stamp");
    ok &= check(payload_kind(payload_bytes(ring, append)) == k_payload_kind_generic_compact,
        "generic compact blank-prefix ASCII fixture uses payload kind 0");

    return ok;
}

bool test_self_contained_tables_are_required()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});

    term::Terminal_history_row_record missing_style = make_base_record(
        101U,
        103U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    missing_style.cells.push_back(make_cell(QStringLiteral("S"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result missing_style_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            missing_style,
            make_identity(7U, 101U));
    ok &= check(missing_style_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects a style ref without a row-local style table entry");

    term::Terminal_history_row_record unreferenced_style = make_base_record(
        102U,
        104U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    unreferenced_style.style_table.push_back(red_style());
    unreferenced_style.cells.resize(1U);
    const term::Terminal_history_row_record_append_result unreferenced_style_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            unreferenced_style,
            make_identity(7U, 102U));
    ok &= check(unreferenced_style_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects an unreferenced row-local style table entry");

    term::Terminal_history_row_record default_style = make_base_record(
        105U,
        107U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    default_style.style_table.push_back(term::make_default_terminal_text_style());
    default_style.cells.push_back(make_cell(QStringLiteral("D"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result default_style_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            default_style,
            make_identity(7U, 105U));
    ok &= check(default_style_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects default-style row-local style table entries");

    term::Terminal_history_row_record duplicate_style = make_base_record(
        106U,
        108U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    duplicate_style.style_table.push_back(red_style());
    duplicate_style.style_table.push_back(red_style());
    duplicate_style.cells.push_back(make_cell(QStringLiteral("A"), 1, true, 1U));
    duplicate_style.cells.push_back(make_cell(QStringLiteral("B"), 1, true, 2U));
    const term::Terminal_history_row_record_append_result duplicate_style_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            duplicate_style,
            make_identity(7U, 106U));
    ok &= check(duplicate_style_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects duplicate row-local style table values");

    term::Terminal_history_row_record missing_key = make_base_record(
        103U,
        105U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    missing_key.cells.push_back(
        make_cell(QStringLiteral("L"), 1, true, 0U, k_first_hyperlink_ref));
    const term::Terminal_history_row_record_append_result missing_key_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            missing_key,
            make_identity(7U, 103U));
    ok &= check(missing_key_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects a hyperlink ref without a row-local identity key");

    term::Terminal_history_row_record duplicate_key = make_base_record(
        104U,
        106U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    duplicate_key.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/same"));
    duplicate_key.hyperlink_identity_keys.emplace(
        k_second_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/same"));
    duplicate_key.cells.push_back(
        make_cell(QStringLiteral("A"), 1, true, 0U, k_first_hyperlink_ref));
    duplicate_key.cells.push_back(
        make_cell(QStringLiteral("B"), 1, true, 0U, k_second_hyperlink_ref));
    const term::Terminal_history_row_record_append_result duplicate_key_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            duplicate_key,
            make_identity(7U, 104U));
    ok &= check(duplicate_key_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects duplicate row-local hyperlink identity keys");
    ok &= check(ring.head_byte_sequence() == 0U,
        "failed table validation publishes no ring record");

    return ok;
}

bool test_style_payload_canonicality_is_required()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});

    auto append_with_style = [&](term::Terminal_text_style style, std::uint64_t row) {
        term::Terminal_history_row_record record = make_base_record(
            row,
            row + 100U,
            term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
            1);
        record.style_table.push_back(style);
        record.cells.push_back(make_cell(QStringLiteral("S"), 1, true, 1U));
        return term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(14U, row));
    };

    term::Terminal_text_style reserved_attribute = red_style();
    reserved_attribute.attributes = 0x0100U;
    ok &= check(append_with_style(reserved_attribute, 181U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects style-table entries with reserved attribute bits");

    term::Terminal_text_style invalid_palette = palette_style();
    invalid_palette.foreground.palette_index = 256U;
    ok &= check(append_with_style(invalid_palette, 182U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects style-table entries with invalid palette indices");

    term::Terminal_text_style noncanonical_default = red_style();
    noncanonical_default.background.rgba = 1U;
    ok &= check(append_with_style(noncanonical_default, 183U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects noncanonical ignored DEFAULT color fields");

    term::Terminal_text_style noncanonical_palette = palette_style();
    noncanonical_palette.foreground.rgba = 1U;
    ok &= check(append_with_style(noncanonical_palette, 184U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects noncanonical ignored PALETTE_INDEX color fields");

    term::Terminal_text_style noncanonical_rgb = red_style();
    noncanonical_rgb.foreground.palette_index = 1U;
    ok &= check(append_with_style(noncanonical_rgb, 185U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects noncanonical ignored RGB color fields");

    term::Terminal_history_row_record red_record = make_base_record(
        186U,
        286U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    red_record.style_table.push_back(red_style());
    red_record.cells.push_back(make_cell(QStringLiteral("R"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result red_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            red_record,
            make_identity(14U, 186U));
    ok &= check(red_append.status == term::Terminal_history_row_record_codec_status::OK,
        "style payload mutation RGB fixture encodes");

    const term::Terminal_history_ring_read_scope red_read =
        ring.read_record(red_append.commit.byte_sequence);
    const std::vector<std::byte> red_payload(
        red_read.payload().begin(),
        red_read.payload().end());

    std::vector<std::byte> decoded_reserved_attribute = red_payload;
    write_le_u16(decoded_reserved_attribute, k_header_bytes + 14U, 0x0100U);
    const term::Terminal_history_row_record_decode_result reserved_attribute_failure =
        decode_mutated_payload(red_read, decoded_reserved_attribute);
    ok &= check(reserved_attribute_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects style-table entries with reserved attribute bits");

    std::vector<std::byte> decoded_noncanonical_rgb = red_payload;
    write_le_u16(decoded_noncanonical_rgb, k_header_bytes + 1U, 1U);
    const term::Terminal_history_row_record_decode_result noncanonical_rgb_failure =
        decode_mutated_payload(red_read, decoded_noncanonical_rgb);
    ok &= check(noncanonical_rgb_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects noncanonical ignored RGB color fields");

    std::vector<std::byte> decoded_noncanonical_default = red_payload;
    write_le_u32(decoded_noncanonical_default, k_header_bytes + 10U, 1U);
    const term::Terminal_history_row_record_decode_result noncanonical_default_failure =
        decode_mutated_payload(red_read, decoded_noncanonical_default);
    ok &= check(noncanonical_default_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects noncanonical ignored DEFAULT color fields");

    std::vector<std::byte> decoded_default_style = red_payload;
    std::fill(
        decoded_default_style.begin() + static_cast<std::ptrdiff_t>(k_header_bytes),
        decoded_default_style.begin() + static_cast<std::ptrdiff_t>(
            k_header_bytes + k_encoded_style_bytes),
        std::byte{0});
    const term::Terminal_history_row_record_decode_result default_style_failure =
        decode_mutated_payload(red_read, decoded_default_style);
    ok &= check(default_style_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects default-style row-local style table entries");

    term::Terminal_history_row_record palette_record = make_base_record(
        187U,
        287U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    palette_record.style_table.push_back(palette_style());
    palette_record.cells.push_back(make_cell(QStringLiteral("P"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result palette_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            palette_record,
            make_identity(14U, 187U));
    ok &= check(palette_append.status == term::Terminal_history_row_record_codec_status::OK,
        "style payload mutation palette fixture encodes");

    const term::Terminal_history_ring_read_scope palette_read =
        ring.read_record(palette_append.commit.byte_sequence);
    const std::vector<std::byte> palette_payload(
        palette_read.payload().begin(),
        palette_read.payload().end());

    std::vector<std::byte> decoded_invalid_palette = palette_payload;
    write_le_u16(decoded_invalid_palette, k_header_bytes + 1U, 256U);
    const term::Terminal_history_row_record_decode_result invalid_palette_failure =
        decode_mutated_payload(palette_read, decoded_invalid_palette);
    ok &= check(invalid_palette_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects style-table entries with invalid palette indices");

    std::vector<std::byte> decoded_noncanonical_palette = palette_payload;
    write_le_u32(decoded_noncanonical_palette, k_header_bytes + 3U, 1U);
    const term::Terminal_history_row_record_decode_result noncanonical_palette_failure =
        decode_mutated_payload(palette_read, decoded_noncanonical_palette);
    ok &= check(noncanonical_palette_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects noncanonical ignored PALETTE_INDEX color fields");

    term::Terminal_history_row_record two_style_record = make_base_record(
        188U,
        288U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    two_style_record.style_table.push_back(red_style());
    two_style_record.style_table.push_back(palette_style());
    two_style_record.cells.push_back(make_cell(QStringLiteral("R"), 1, true, 1U));
    two_style_record.cells.push_back(make_cell(QStringLiteral("P"), 1, true, 2U));
    const term::Terminal_history_row_record_append_result two_style_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            two_style_record,
            make_identity(14U, 188U));
    ok &= check(two_style_append.status == term::Terminal_history_row_record_codec_status::OK,
        "duplicate style mutation fixture encodes");

    const term::Terminal_history_ring_read_scope two_style_read =
        ring.read_record(two_style_append.commit.byte_sequence);
    std::vector<std::byte> duplicate_style_payload(
        two_style_read.payload().begin(),
        two_style_read.payload().end());
    std::copy(
        duplicate_style_payload.begin() + static_cast<std::ptrdiff_t>(k_header_bytes),
        duplicate_style_payload.begin() + static_cast<std::ptrdiff_t>(
            k_header_bytes + k_encoded_style_bytes),
        duplicate_style_payload.begin() + static_cast<std::ptrdiff_t>(
            k_header_bytes + k_encoded_style_bytes));
    const term::Terminal_history_row_record_decode_result duplicate_style_failure =
        decode_mutated_payload(two_style_read, duplicate_style_payload);
    ok &= check(duplicate_style_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects duplicate row-local style table values");

    return ok;
}

bool test_encode_rejects_malformed_cell_states()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});

    auto append_record = [&](term::Terminal_history_row_record record, std::uint64_t row) {
        return term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(15U, row));
    };

    term::Terminal_history_row_record default_blank_empty_text = make_base_record(
        191U,
        291U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    default_blank_empty_text.cells.resize(1U);
    default_blank_empty_text.cells[0].text.clear();
    ok &= check(append_record(default_blank_empty_text, 191U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects default blank cells with noncanonical blank text");

    term::Terminal_history_row_record styled_blank_empty_text = make_base_record(
        192U,
        292U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    styled_blank_empty_text.style_table.push_back(red_style());
    styled_blank_empty_text.cells.push_back(make_cell(QString(), 1, false, 1U));
    ok &= check(append_record(styled_blank_empty_text, 192U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects styled blank cells with noncanonical blank text");

    term::Terminal_history_row_record occupied_empty_text = make_base_record(
        193U,
        293U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    occupied_empty_text.cells.push_back(make_cell(QString(), 1, true));
    ok &= check(append_record(occupied_empty_text, 193U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects occupied cells with empty text");

    term::Terminal_history_row_record continuation_not_occupied = make_base_record(
        194U,
        294U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    continuation_not_occupied.cells.push_back(
        make_cell(QString::fromUtf8("\xe7\x95\x8c"), 2, true));
    continuation_not_occupied.cells.push_back(
        make_wide_continuation(0U, term::k_no_terminal_hyperlink_id));
    continuation_not_occupied.cells[1].occupied = false;
    ok &= check(append_record(continuation_not_occupied, 194U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects wide continuations that are not occupied");

    term::Terminal_history_row_record continuation_wrong_style = make_base_record(
        195U,
        295U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    continuation_wrong_style.style_table.push_back(red_style());
    continuation_wrong_style.cells.push_back(
        make_cell(QString::fromUtf8("\xe7\x95\x8c"), 2, true, 1U));
    continuation_wrong_style.cells.push_back(
        make_wide_continuation(0U, term::k_no_terminal_hyperlink_id));
    ok &= check(append_record(continuation_wrong_style, 195U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects wide continuations with non-inherited style refs");

    term::Terminal_history_row_record continuation_wrong_link = make_base_record(
        196U,
        296U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    continuation_wrong_link.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/wide-link"));
    continuation_wrong_link.cells.push_back(
        make_cell(
            QString::fromUtf8("\xe7\x95\x8c"),
            2,
            true,
            0U,
            k_first_hyperlink_ref));
    continuation_wrong_link.cells.push_back(
        make_wide_continuation(0U, term::k_no_terminal_hyperlink_id));
    ok &= check(append_record(continuation_wrong_link, 196U).status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "encode rejects wide continuations with non-inherited hyperlink refs");

    return ok;
}

bool test_malformed_counts_are_bounded_before_allocation()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record record = make_base_record(
        111U,
        113U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    record.cells.push_back(make_cell(QStringLiteral("x"), 1, true));

    const term::Terminal_history_row_record_append_result append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(8U, 111U));
    ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
        "bounded-count fixture encodes");

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    const std::vector<std::byte> payload(read.payload().begin(), read.payload().end());

    std::vector<std::byte> zero_source_width = payload;
    write_le_u32(zero_source_width, k_header_source_width_offset, 0U);
    const term::Terminal_history_row_record_decode_result zero_source_width_failure =
        decode_mutated_payload(read, zero_source_width);
    ok &= check(zero_source_width_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects zero source_width before reserving decoded cells");

    std::vector<std::byte> unsupported_source_width = payload;
    write_le_u32(
        unsupported_source_width,
        k_header_source_width_offset,
        static_cast<std::uint32_t>(term::k_terminal_screen_model_max_columns + 1));
    const term::Terminal_history_row_record_decode_result unsupported_source_width_failure =
        decode_mutated_payload(read, unsupported_source_width);
    ok &= check(unsupported_source_width_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects unsupported source_width before reserving decoded cells");

    std::vector<std::byte> bad_cell_count = payload;
    write_le_u32(
        bad_cell_count,
        k_header_cell_count_offset,
        std::numeric_limits<std::uint32_t>::max());
    const term::Terminal_history_row_record_decode_result cell_count_failure =
        decode_mutated_payload(read, bad_cell_count);
    ok &= check(cell_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode bounds malformed cell_count before reserving decoded cells");

    std::vector<std::byte> bad_hyperlink_count = payload;
    write_le_u32(
        bad_hyperlink_count,
        k_header_hyperlink_count_offset,
        std::numeric_limits<std::uint32_t>::max());
    const term::Terminal_history_row_record_decode_result hyperlink_count_failure =
        decode_mutated_payload(read, bad_hyperlink_count);
    ok &= check(hyperlink_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode bounds malformed hyperlink_count before reserving decoded data");

    std::vector<std::byte> bad_style_count = payload;
    write_le_u16(
        bad_style_count,
        k_header_style_count_offset,
        std::numeric_limits<std::uint16_t>::max());
    const term::Terminal_history_row_record_decode_result style_count_failure =
        decode_mutated_payload(read, bad_style_count);
    ok &= check(style_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode bounds malformed style_count before reserving decoded styles");

    return ok;
}

bool test_decode_rejects_reserved_and_noncanonical_streams()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record ascii = make_base_record(
        121U,
        127U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    ascii.cells.resize(1U);
    ascii.cells.push_back(make_cell(QStringLiteral("A"), 1, true));

    const term::Terminal_history_row_record_append_result ascii_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            ascii,
            make_identity(9U, 121U));
    ok &= check(ascii_append.status == term::Terminal_history_row_record_codec_status::OK,
        "reserved opcode fixture encodes");

    const term::Terminal_history_ring_read_scope ascii_read =
        ring.read_record(ascii_append.commit.byte_sequence);
    const std::vector<std::byte> ascii_payload(
        ascii_read.payload().begin(),
        ascii_read.payload().end());
    ok &= check(payload_kind(ascii_payload) == k_payload_kind_generic_compact,
        "reserved opcode fixture uses generic compact payload kind 0");

    std::vector<std::byte> reserved_opcode(
        ascii_payload.begin(),
        ascii_payload.end());
    reserved_opcode[k_header_bytes] = static_cast<std::byte>(0x03U);
    const term::Terminal_history_row_record_decode_result reserved_opcode_failure =
        decode_mutated_payload(ascii_read, reserved_opcode);
    ok &= check(reserved_opcode_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects reserved primary opcodes");

    std::vector<std::byte> noncanonical_ascii(
        ascii_payload.begin(),
        ascii_payload.end());
    noncanonical_ascii[k_header_bytes + 1U] = static_cast<std::byte>(0x02U);
    noncanonical_ascii.insert(
        noncanonical_ascii.begin() + static_cast<std::ptrdiff_t>(k_header_bytes + 2U),
        {
            static_cast<std::byte>(0x00U),
            static_cast<std::byte>('A'),
        });
    refresh_payload_size_fields(noncanonical_ascii);
    const term::Terminal_history_row_record_decode_result noncanonical_ascii_failure =
        decode_mutated_payload(ascii_read, noncanonical_ascii);
    ok &= check(noncanonical_ascii_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects extended encoding for default printable ASCII");

    term::Terminal_history_row_record styled_ascii = make_base_record(
        122U,
        128U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    styled_ascii.style_table.push_back(red_style());
    styled_ascii.cells.push_back(make_cell(QStringLiteral("A"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result styled_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            styled_ascii,
            make_identity(9U, 122U));
    ok &= check(styled_append.status == term::Terminal_history_row_record_codec_status::OK,
        "noncanonical styled ASCII fixture encodes");

    const term::Terminal_history_ring_read_scope styled_read =
        ring.read_record(styled_append.commit.byte_sequence);
    const std::size_t styled_stream = cell_stream_offset(styled_ascii);
    std::vector<std::byte> reserved_extended_kind(
        styled_read.payload().begin(),
        styled_read.payload().end());
    reserved_extended_kind[styled_stream + 1U] = static_cast<std::byte>(0x24U);
    const term::Terminal_history_row_record_decode_result reserved_kind_failure =
        decode_mutated_payload(styled_read, reserved_extended_kind);
    ok &= check(reserved_kind_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects reserved extended cell kinds");

    term::Terminal_history_row_record wide = make_base_record(
        123U,
        129U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    wide.cells.push_back(make_cell(QString::fromUtf8("\xe7\x95\x8c"), 2, true));
    wide.cells.push_back(make_wide_continuation(0U, term::k_no_terminal_hyperlink_id));
    const term::Terminal_history_row_record_append_result wide_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            wide,
            make_identity(9U, 123U));
    ok &= check(wide_append.status == term::Terminal_history_row_record_codec_status::OK,
        "wide continuation mutation fixture encodes");

    const term::Terminal_history_ring_read_scope wide_read =
        ring.read_record(wide_append.commit.byte_sequence);
    const std::size_t wide_stream = cell_stream_offset(wide);
    std::vector<std::byte> bad_continuation(
        wide_read.payload().begin(),
        wide_read.payload().end());
    bad_continuation.back() = static_cast<std::byte>(0x00U);
    const term::Terminal_history_row_record_decode_result bad_continuation_failure =
        decode_mutated_payload(wide_read, bad_continuation);
    ok &= check(bad_continuation_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects a non-continuation opcode inside a wide span");

    std::vector<std::byte> tag1_reserved_bits(
        wide_read.payload().begin(),
        wide_read.payload().end());
    tag1_reserved_bits[wide_stream + 2U] = static_cast<std::byte>(0x11U);
    const term::Terminal_history_row_record_decode_result tag1_reserved_failure =
        decode_mutated_payload(wide_read, tag1_reserved_bits);
    ok &= check(tag1_reserved_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects reserved tag1 bits");

    std::vector<std::byte> display_present_width_missing(
        wide_read.payload().begin(),
        wide_read.payload().end());
    display_present_width_missing[wide_stream + 2U] = static_cast<std::byte>(0x00U);
    const term::Terminal_history_row_record_decode_result display_missing_failure =
        decode_mutated_payload(wide_read, display_present_width_missing);
    ok &= check(display_missing_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects display-presence and display-width-code mismatches");

    std::vector<std::byte> nonshortest_display_width(
        wide_read.payload().begin(),
        wide_read.payload().end());
    nonshortest_display_width[wide_stream + 2U] = static_cast<std::byte>(0x02U);
    nonshortest_display_width.insert(
        nonshortest_display_width.begin() +
            static_cast<std::ptrdiff_t>(wide_stream + 5U),
        static_cast<std::byte>(0x00U));
    refresh_payload_size_fields(nonshortest_display_width);
    const term::Terminal_history_row_record_decode_result nonshortest_width_failure =
        decode_mutated_payload(wide_read, nonshortest_display_width);
    ok &= check(nonshortest_width_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects non-shortest width-coded values");

    term::Terminal_history_row_record linked = make_base_record(
        124U,
        130U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    linked.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/presence"));
    linked.cells.push_back(
        make_cell(QStringLiteral("A"), 1, true, 0U, k_first_hyperlink_ref));
    const term::Terminal_history_row_record_append_result linked_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            linked,
            make_identity(9U, 124U));
    ok &= check(linked_append.status == term::Terminal_history_row_record_codec_status::OK,
        "hyperlink presence mutation fixture encodes");

    const term::Terminal_history_ring_read_scope linked_read =
        ring.read_record(linked_append.commit.byte_sequence);
    const std::size_t linked_stream = cell_stream_offset(linked);
    std::vector<std::byte> hyperlink_present_width_missing(
        linked_read.payload().begin(),
        linked_read.payload().end());
    hyperlink_present_width_missing[linked_stream + 2U] = static_cast<std::byte>(0x00U);
    const term::Terminal_history_row_record_decode_result hyperlink_missing_failure =
        decode_mutated_payload(linked_read, hyperlink_present_width_missing);
    ok &= check(hyperlink_missing_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects hyperlink-presence and hyperlink-width-code mismatches");

    std::vector<std::byte> display_absent_width_present(
        linked_read.payload().begin(),
        linked_read.payload().end());
    display_absent_width_present[linked_stream + 2U] = static_cast<std::byte>(0x05U);
    const term::Terminal_history_row_record_decode_result display_present_failure =
        decode_mutated_payload(linked_read, display_absent_width_present);
    ok &= check(display_present_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects absent display fields with a nonzero display-width code");

    term::Terminal_history_row_record utf8 = make_base_record(
        125U,
        131U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    utf8.cells.push_back(make_cell(QString::fromUtf8("\xc3\xa9"), 1, true));
    const term::Terminal_history_row_record_append_result utf8_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            utf8,
            make_identity(9U, 125U));
    ok &= check(utf8_append.status == term::Terminal_history_row_record_codec_status::OK,
        "occupied text-length mutation fixture encodes");

    const term::Terminal_history_ring_read_scope utf8_read =
        ring.read_record(utf8_append.commit.byte_sequence);
    const std::size_t utf8_stream = cell_stream_offset(utf8);
    std::vector<std::byte> empty_occupied_text(
        utf8_read.payload().begin(),
        utf8_read.payload().end());
    empty_occupied_text[utf8_stream + 2U] = static_cast<std::byte>(0x00U);
    const term::Terminal_history_row_record_decode_result empty_text_failure =
        decode_mutated_payload(utf8_read, empty_occupied_text);
    ok &= check(empty_text_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects occupied cells with encoded empty text");

    term::Terminal_history_row_record width_mismatch = make_base_record(
        126U,
        132U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        3);
    width_mismatch.cells.push_back(make_cell(QString::fromUtf8("\xe7\x95\x8c"), 2, true));
    width_mismatch.cells.push_back(
        make_wide_continuation(0U, term::k_no_terminal_hyperlink_id));
    width_mismatch.cells.resize(3U);
    const term::Terminal_history_row_record_append_result width_mismatch_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            width_mismatch,
            make_identity(9U, 126U));
    ok &= check(width_mismatch_append.status ==
            term::Terminal_history_row_record_codec_status::OK,
        "display/text width mismatch mutation fixture encodes");

    const term::Terminal_history_ring_read_scope width_mismatch_read =
        ring.read_record(width_mismatch_append.commit.byte_sequence);
    const std::size_t width_mismatch_stream = cell_stream_offset(width_mismatch);
    std::vector<std::byte> bad_text_width(
        width_mismatch_read.payload().begin(),
        width_mismatch_read.payload().end());
    bad_text_width[width_mismatch_stream + 4U] = static_cast<std::byte>(0x03U);
    const term::Terminal_history_row_record_decode_result bad_text_width_failure =
        decode_mutated_payload(width_mismatch_read, bad_text_width);
    ok &= check(bad_text_width_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects display-width and measured text-width mismatches");

    return ok;
}

bool test_decode_rejects_malformed_prefix_plain_ascii_streams()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record prefix = make_base_record(
        127U,
        133U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    prefix.cells.push_back(make_cell(QStringLiteral("A"), 1, true));

    const term::Terminal_history_row_record_append_result prefix_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            prefix,
            make_identity(9U, 127U));
    ok &= check(prefix_append.status == term::Terminal_history_row_record_codec_status::OK,
        "malformed prefix plain ASCII fixture encodes");

    const term::Terminal_history_ring_read_scope prefix_read =
        ring.read_record(prefix_append.commit.byte_sequence);
    const std::vector<std::byte> prefix_payload(
        prefix_read.payload().begin(),
        prefix_read.payload().end());
    ok &= check(payload_kind(prefix_payload) == k_payload_kind_prefix_plain_ascii,
        "malformed prefix fixture uses prefix plain ASCII payload kind 1");

    std::vector<std::byte> non_printable = prefix_payload;
    non_printable[k_header_bytes] = static_cast<std::byte>(0x1fU);
    const term::Terminal_history_row_record_decode_result non_printable_failure =
        decode_mutated_payload(prefix_read, non_printable);
    ok &= check(non_printable_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects non-printable bytes in a prefix plain ASCII stream");

    std::vector<std::byte> too_many_prefix_bytes = prefix_payload;
    too_many_prefix_bytes.push_back(static_cast<std::byte>('B'));
    refresh_payload_size_fields(too_many_prefix_bytes);
    const term::Terminal_history_row_record_decode_result too_many_prefix_failure =
        decode_mutated_payload(prefix_read, too_many_prefix_bytes);
    ok &= check(too_many_prefix_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects prefix plain ASCII payload byte counts above source_width");

    std::vector<std::byte> prefix_with_style_count = prefix_payload;
    write_le_u16(prefix_with_style_count, k_header_style_count_offset, 1U);
    const term::Terminal_history_row_record_decode_result style_count_failure =
        decode_mutated_payload(prefix_read, prefix_with_style_count);
    ok &= check(style_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects nonzero style_count under prefix plain ASCII kind 1");

    std::vector<std::byte> prefix_with_hyperlink_count = prefix_payload;
    write_le_u32(prefix_with_hyperlink_count, k_header_hyperlink_count_offset, 1U);
    const term::Terminal_history_row_record_decode_result hyperlink_count_failure =
        decode_mutated_payload(prefix_read, prefix_with_hyperlink_count);
    ok &= check(hyperlink_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects nonzero hyperlink_count under prefix plain ASCII kind 1");

    std::vector<std::byte> reserved_payload_kind = prefix_payload;
    write_le_u32(reserved_payload_kind, k_header_flags_offset, 2U);
    const term::Terminal_history_row_record_decode_result reserved_kind_failure =
        decode_mutated_payload(prefix_read, reserved_payload_kind);
    ok &= check(reserved_kind_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_HEADER,
        "decode rejects reserved row-record payload kinds");

    return ok;
}

bool test_decode_rejects_bad_refs_empty_keys_and_invalid_utf8()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});

    term::Terminal_history_row_record styled = make_base_record(
        131U,
        137U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    styled.style_table.push_back(red_style());
    styled.cells.push_back(make_cell(QStringLiteral("A"), 1, true, 1U));
    const term::Terminal_history_row_record_append_result styled_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            styled,
            make_identity(10U, 131U));
    ok &= check(styled_append.status == term::Terminal_history_row_record_codec_status::OK,
        "bad style-ref fixture encodes");

    const term::Terminal_history_ring_read_scope styled_read =
        ring.read_record(styled_append.commit.byte_sequence);
    std::vector<std::byte> bad_style_ref(
        styled_read.payload().begin(),
        styled_read.payload().end());
    bad_style_ref[k_header_bytes + 16U + 2U] = static_cast<std::byte>(2U);
    const term::Terminal_history_row_record_decode_result bad_style_ref_failure =
        decode_mutated_payload(styled_read, bad_style_ref);
    ok &= check(bad_style_ref_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects a style ref beyond style_count");

    term::Terminal_history_row_record linked = make_base_record(
        132U,
        138U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    linked.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/link"));
    linked.cells.push_back(
        make_cell(QStringLiteral("A"), 1, true, 0U, k_first_hyperlink_ref));
    const term::Terminal_history_row_record_append_result linked_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            linked,
            make_identity(10U, 132U));
    ok &= check(linked_append.status == term::Terminal_history_row_record_codec_status::OK,
        "bad hyperlink-ref fixture encodes");

    const term::Terminal_history_ring_read_scope linked_read =
        ring.read_record(linked_append.commit.byte_sequence);
    const std::vector<std::byte> linked_payload(
        linked_read.payload().begin(),
        linked_read.payload().end());
    const std::size_t linked_stream_offset = k_header_bytes + 1U + 1U +
        static_cast<std::size_t>(
            linked.hyperlink_identity_keys.at(k_first_hyperlink_ref).size());

    std::vector<std::byte> bad_hyperlink_ref = linked_payload;
    bad_hyperlink_ref[linked_stream_offset + 3U] = static_cast<std::byte>(2U);
    const term::Terminal_history_row_record_decode_result bad_hyperlink_ref_failure =
        decode_mutated_payload(linked_read, bad_hyperlink_ref);
    ok &= check(bad_hyperlink_ref_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects a hyperlink ref beyond hyperlink_count");

    std::vector<std::byte> invalid_identity_length_width = linked_payload;
    invalid_identity_length_width[k_header_bytes] = static_cast<std::byte>(4U);
    const term::Terminal_history_row_record_decode_result invalid_length_width_failure =
        decode_mutated_payload(linked_read, invalid_identity_length_width);
    ok &= check(invalid_length_width_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects invalid table-length width codes");

    std::vector<std::byte> empty_identity = linked_payload;
    empty_identity[k_header_bytes + 1U] = static_cast<std::byte>(0U);
    const term::Terminal_history_row_record_decode_result empty_identity_failure =
        decode_mutated_payload(linked_read, empty_identity);
    ok &= check(empty_identity_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects encoded nonzero-width identity length zero");

    term::Terminal_history_row_record utf8 = make_base_record(
        133U,
        139U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    utf8.cells.push_back(make_cell(QString::fromUtf8("\xc3\xa9"), 1, true));
    const term::Terminal_history_row_record_append_result utf8_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            utf8,
            make_identity(10U, 133U));
    ok &= check(utf8_append.status == term::Terminal_history_row_record_codec_status::OK,
        "invalid UTF-8 mutation fixture encodes");

    const term::Terminal_history_ring_read_scope utf8_read =
        ring.read_record(utf8_append.commit.byte_sequence);
    std::vector<std::byte> invalid_utf8(
        utf8_read.payload().begin(),
        utf8_read.payload().end());
    invalid_utf8.back() = static_cast<std::byte>(0xffU);
    const term::Terminal_history_row_record_decode_result invalid_utf8_failure =
        decode_mutated_payload(utf8_read, invalid_utf8);
    ok &= check(invalid_utf8_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "decode rejects malformed UTF-8 text");

    return ok;
}

bool test_header_and_handle_validation_failures()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record record = make_base_record(
        141U,
        147U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    record.cells.push_back(make_cell(QStringLiteral("O"), 1, true));
    record.cells.push_back(make_cell(QStringLiteral("K"), 1, true));

    term::Terminal_history_row_record_append_result append;
    const term::Terminal_history_row_record_decode_result decoded =
        append_and_decode(ring, record, make_identity(11U, 141U), append);
    ok &= check(decoded.status == term::Terminal_history_row_record_codec_status::OK,
        "validation fixture decodes before mutation");

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    std::vector<std::byte> payload(read.payload().begin(), read.payload().end());

    std::vector<std::byte> bad_header = payload;
    bad_header.front() = std::byte{0U};
    const term::Terminal_history_row_record_decode_result header_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes(),
                bad_header,
            },
            append.history_handle);
    ok &= check(header_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_HEADER,
        "decode rejects a row-record header magic failure");

    std::vector<std::byte> bad_flags = payload;
    write_le_u32(bad_flags, k_header_flags_offset, 0x10U);
    const term::Terminal_history_row_record_decode_result flags_failure =
        decode_mutated_payload(read, bad_flags);
    ok &= check(flags_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_HEADER,
        "decode rejects reserved row-record flag bits");

    term::terminal_history_handle_t wrong_generation = append.history_handle;
    ++wrong_generation.content_generation;
    const term::Terminal_history_row_record_decode_result generation_failure =
        term::decode_terminal_history_row_record(read, wrong_generation);
    ok &= check(generation_failure.status ==
            term::Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH,
        "decode validates expected content generation");

    const term::Terminal_history_row_record_decode_result size_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes() + 1U,
                payload,
            },
            std::nullopt);
    ok &= check(size_failure.status ==
            term::Terminal_history_row_record_codec_status::RECORD_SIZE_MISMATCH,
        "decode validates ring-framed record size against the row payload");

    return ok;
}

bool test_materialized_decode_owns_data_after_read_scope_and_eviction()
{
    bool ok = true;

    term::Terminal_history_ring ring({1600U, 1600U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "bounded ownership fixture ring initializes");

    term::Terminal_history_row_record record = make_base_record(
        151U,
        153U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    record.hyperlink_identity_keys.emplace(
        k_first_hyperlink_ref,
        QByteArrayLiteral("uri:https://example.test/owned"));
    record.cells.push_back(
        make_cell(QStringLiteral("O"), 1, true, 0U, k_first_hyperlink_ref));

    const term::Terminal_history_row_record_append_result first_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(12U, 151U));
    ok &= check(first_append.status == term::Terminal_history_row_record_codec_status::OK,
        "ownership fixture first row encodes");

    term::Terminal_history_row_record_decode_result materialized;
    {
        const term::Terminal_history_ring_read_scope read =
            ring.read_record(first_append.commit.byte_sequence);
        materialized = term::decode_terminal_history_row_record(
            read,
            first_append.history_handle);
    }
    ok &= check(materialized.status == term::Terminal_history_row_record_codec_status::OK,
        "decode materializes row data inside a bounded read scope");

    for (std::uint64_t row_sequence = 152U; row_sequence < 164U; ++row_sequence) {
        term::Terminal_history_row_record filler = make_base_record(
            row_sequence,
            row_sequence + 100U,
            term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
            1);
        filler.cells.push_back(make_cell(QStringLiteral("f"), 1, true));
        const term::Terminal_history_row_record_append_result filler_append =
            term::encode_terminal_history_row_record_to_ring(
                ring,
                filler,
                make_identity(12U, row_sequence));
        ok &= check(filler_append.status ==
                term::Terminal_history_row_record_codec_status::OK,
            "ownership fixture filler row encodes");
    }

    ok &= check(ring.read_record(first_append.commit.byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "ownership fixture evicts the original ring record");
    ok &= check(materialized.record.cells.front().text == QStringLiteral("O") &&
            materialized.record.hyperlink_identity_keys.at(k_first_hyperlink_ref) ==
                QByteArrayLiteral("uri:https://example.test/owned"),
        "decoded materialization remains owned after read-scope end and ring eviction");

    return ok;
}

bool test_row_record_oversize_hard_fails_before_publication()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "oversize fixture ring initializes");

    term::Terminal_history_row_record oversized = make_base_record(
        171U,
        177U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        4000);
    for (int column = 0; column < 4000; ++column) {
        oversized.cells.push_back(make_cell(QStringLiteral("x"), 1, true));
    }

    const term::Terminal_history_row_record_append_result append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            oversized,
            make_identity(13U, 171U));
    ok &= check(append.status ==
            term::Terminal_history_row_record_codec_status::RING_RESERVE_FAILED,
        "oversize row-record hard-fails at ring reservation");
    ok &= check(append.ring_status == term::Terminal_history_ring_status::OVERSIZE_RECORD,
        "oversize row-record exposes the terminal-local oversize status");
    ok &= check(ring.head_byte_sequence() == 0U,
        "oversize row-record publishes no partial or fallback record");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_prefix_plain_ascii_rows_use_prefix_payload();
    ok &= test_extended_styles_hyperlinks_and_wide_cells_round_trip();
    ok &= test_generic_compact_default_ascii_after_blank_round_trip();
    ok &= test_self_contained_tables_are_required();
    ok &= test_style_payload_canonicality_is_required();
    ok &= test_encode_rejects_malformed_cell_states();
    ok &= test_malformed_counts_are_bounded_before_allocation();
    ok &= test_decode_rejects_reserved_and_noncanonical_streams();
    ok &= test_decode_rejects_malformed_prefix_plain_ascii_streams();
    ok &= test_decode_rejects_bad_refs_empty_keys_and_invalid_utf8();
    ok &= test_header_and_handle_validation_failures();
    ok &= test_materialized_decode_owns_data_after_read_scope_and_eviction();
    ok &= test_row_record_oversize_hard_fails_before_publication();
    return ok ? 0 : 1;
}
