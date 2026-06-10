#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "helpers/test_check.h"

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

constexpr std::size_t k_test_row_record_cell_count_offset = 100U;
constexpr std::size_t k_test_row_record_hyperlink_count_offset = 104U;
constexpr std::size_t k_test_row_record_cell_hyperlink_offset = 116U + 16U;

void write_le_u32(
    std::vector<std::byte>& bytes,
    std::size_t             offset,
    std::uint32_t           value)
{
    for (std::size_t i = 0U; i < 4U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
}

void write_le_u64(
    std::vector<std::byte>& bytes,
    std::size_t             offset,
    std::uint64_t           value)
{
    for (std::size_t i = 0U; i < 8U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
}

term::Terminal_history_row_cell make_cell(
    QString                 text,
    int                     display_width,
    bool                    occupied,
    term::Terminal_style_id style_id = term::k_default_terminal_style_id,
    std::uint64_t           hyperlink_id = 0U)
{
    term::Terminal_history_row_cell cell;
    cell.text = std::move(text);
    cell.display_width = display_width;
    cell.occupied = occupied;
    cell.style_id = style_id;
    cell.hyperlink_id = hyperlink_id;
    return cell;
}

term::Terminal_history_row_cell make_wide_continuation(
    term::Terminal_style_id style_id,
    std::uint64_t           hyperlink_id)
{
    term::Terminal_history_row_cell cell;
    cell.text.clear();
    cell.display_width = 0;
    cell.wide_continuation = true;
    cell.style_id = style_id;
    cell.hyperlink_id = hyperlink_id;
    return cell;
}

term::Terminal_history_row_record make_base_record(
    std::uint64_t                                  retained_line_id,
    std::uint64_t                                  content_generation,
    term::Terminal_retained_line_provenance_source source,
    int                                            source_width)
{
    term::Terminal_history_row_record record;
    record.provenance.retained_line_id = retained_line_id;
    record.provenance.content_generation = content_generation;
    record.provenance.source = source;
    record.metadata.source_width = source_width;
    record.metadata.style_lifetime =
        term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
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
        left.text == right.text &&
        left.display_width == right.display_width &&
        left.wide_continuation == right.wide_continuation &&
        left.occupied == right.occupied &&
        left.style_id == right.style_id &&
        left.hyperlink_id == right.hyperlink_id;
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
        left.provenance.retained_line_id == right.provenance.retained_line_id &&
        left.provenance.content_generation == right.provenance.content_generation &&
        left.provenance.source == right.provenance.source &&
        left.hyperlink_identity_keys == right.hyperlink_identity_keys &&
        left.metadata.source_width == right.metadata.source_width &&
        left.metadata.style_lifetime == right.metadata.style_lifetime &&
        left.metadata.wrap_state == right.metadata.wrap_state;
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

bool test_dense_and_blank_rows_round_trip()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 4B dense/blank fixture ring initializes");

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
        "Phase 4B blank row encodes into the ring");
    ok &= check(decoded_blank.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B blank row decodes from a bounded read scope");
    ok &= check(records_equal(decoded_blank.record, blank),
        "Phase 4B blank dense row preserves owned cell and metadata values");

    term::Terminal_history_row_record dense = make_base_record(
        12U,
        18U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        4);
    dense.cells.push_back(make_cell(QStringLiteral("A"), 1, true));
    dense.cells.push_back(make_cell(QStringLiteral("B"), 1, true));
    dense.cells.push_back(make_cell(QStringLiteral("C"), 1, true));
    dense.cells.push_back(make_cell(QStringLiteral(" "), 1, false));

    term::Terminal_history_row_record_append_result dense_append;
    const term::Terminal_history_row_record_decode_result decoded_dense =
        append_and_decode(ring, dense, make_identity(3U, 12U), dense_append);
    ok &= check(dense_append.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B dense content row encodes into the ring");
    ok &= check(decoded_dense.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B dense content row decodes from a bounded read scope");
    ok &= check(records_equal(decoded_dense.record, dense),
        "Phase 4B dense content row preserves all dense cell values");

    return ok;
}

bool test_wide_clusters_styles_hyperlinks_and_recovery_round_trip()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 4B cluster fixture ring initializes");

    term::Terminal_history_row_record record = make_base_record(
        91U,
        23U,
        term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        5);
    record.cells.push_back(make_cell(QString::fromUtf8("A\xcc\x81"), 1, true, 7U, 100U));
    record.cells.push_back(make_cell(QString::fromUtf8("\xe7\x95\x8c"), 2, true, 8U, 101U));
    record.cells.push_back(make_wide_continuation(8U, 101U));
    record.cells.push_back(make_cell(
        QString::fromUtf8(
            "\xf0\x9f\x91\xa9"
            "\xe2\x80\x8d"
            "\xf0\x9f\x92\xbb"),
        2,
        true,
        9U,
        0U));
    record.cells.push_back(make_cell(QStringLiteral(" "), 1, false, 10U, 0U));
    record.hyperlink_identity_keys.emplace(
        100U,
        QByteArrayLiteral("uri:https://example.test/combining"));
    record.hyperlink_identity_keys.emplace(
        101U,
        QByteArrayLiteral("uri:https://example.test/wide"));

    term::terminal_history_row_record_identity_t identity = make_identity(4U, 81U);
    identity.previous_row_byte_sequence = 640U;
    identity.previous_row_sequence = 80U;

    term::Terminal_history_row_record_append_result append;
    const term::Terminal_history_row_record_decode_result decoded =
        append_and_decode(ring, record, identity, append);
    ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B styled hyperlink recovered row encodes into the ring");
    ok &= check(decoded.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B styled hyperlink recovered row decodes from the ring");
    ok &= check(records_equal(decoded.record, record),
        "Phase 4B preserves clusters, wide spans, styles, hyperlinks, provenance, source width, and wrap metadata");
    ok &= check(decoded.previous_row_byte_sequence == identity.previous_row_byte_sequence &&
            decoded.previous_row_sequence == identity.previous_row_sequence,
        "Phase 4B preserves previous-row traversal metadata without implementing traversal");
    ok &= check(decoded.history_handle == append.history_handle,
        "Phase 4B decoded handle matches the ring-framed committed identity");

    return ok;
}

bool test_self_contained_hyperlinks_are_required()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 4B hyperlink self-containment fixture ring initializes");

    term::Terminal_history_row_record missing_key = make_base_record(
        101U,
        103U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    missing_key.cells.push_back(make_cell(QStringLiteral("link"), 1, true, 0U, 501U));

    const term::Terminal_history_row_record_append_result missing_key_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            missing_key,
            make_identity(7U, 101U));
    ok &= check(missing_key_append.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "Phase 4B rejects encode of a hyperlink cell without a row-local identity key");
    ok &= check(ring.head_byte_sequence() == 0U,
        "Phase 4B missing hyperlink key failure publishes no ring record");

    term::Terminal_history_row_record valid = make_base_record(
        102U,
        104U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    valid.cells.push_back(make_cell(QStringLiteral("link"), 1, true, 0U, 601U));
    valid.hyperlink_identity_keys.emplace(
        601U,
        QByteArrayLiteral("uri:https://example.test/self-contained"));

    const term::Terminal_history_row_record_append_result valid_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            valid,
            make_identity(7U, 102U));
    ok &= check(valid_append.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B hyperlink self-containment decode fixture encodes");

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(valid_append.commit.byte_sequence);
    std::vector<std::byte> payload(read.payload().begin(), read.payload().end());
    write_le_u64(payload, k_test_row_record_cell_hyperlink_offset, 602U);

    const term::Terminal_history_row_record_decode_result decode_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes(),
                payload,
            },
            std::nullopt);
    ok &= check(decode_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "Phase 4B rejects decode of a hyperlink cell without a row-local identity key");

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
        "Phase 4B bounded-count fixture encodes");

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    std::vector<std::byte> payload(read.payload().begin(), read.payload().end());

    std::vector<std::byte> bad_cell_count = payload;
    write_le_u32(
        bad_cell_count,
        k_test_row_record_cell_count_offset,
        std::numeric_limits<std::uint32_t>::max());
    const term::Terminal_history_row_record_decode_result cell_count_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes(),
                bad_cell_count,
            },
            std::nullopt);
    ok &= check(cell_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "Phase 4B bounds malformed cell_count before reserving decoded cells");

    std::vector<std::byte> bad_hyperlink_count = payload;
    write_le_u32(
        bad_hyperlink_count,
        k_test_row_record_hyperlink_count_offset,
        std::numeric_limits<std::uint32_t>::max());
    const term::Terminal_history_row_record_decode_result hyperlink_count_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes(),
                bad_hyperlink_count,
            },
            std::nullopt);
    ok &= check(hyperlink_count_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_PAYLOAD,
        "Phase 4B bounds malformed hyperlink_count before reserving decoded data");

    return ok;
}

bool test_header_footer_and_handle_validation_failures()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_record record = make_base_record(
        31U,
        37U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        2);
    record.cells.push_back(make_cell(QStringLiteral("O"), 1, true));
    record.cells.push_back(make_cell(QStringLiteral("K"), 1, true));

    term::Terminal_history_row_record_append_result append;
    const term::Terminal_history_row_record_decode_result decoded =
        append_and_decode(ring, record, make_identity(5U, 31U), append);
    ok &= check(decoded.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B validation fixture decodes before mutation");

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
        "Phase 4B rejects a row-record header magic failure");

    std::vector<std::byte> bad_footer = payload;
    bad_footer.back() = std::byte{0U};
    const term::Terminal_history_row_record_decode_result footer_failure =
        term::decode_terminal_history_row_record_payload(
            {
                read.byte_sequence(),
                read.record_bytes(),
                bad_footer,
            },
            append.history_handle);
    ok &= check(footer_failure.status ==
            term::Terminal_history_row_record_codec_status::INVALID_FOOTER,
        "Phase 4B rejects a row-record footer magic failure");

    term::terminal_history_handle_t wrong_generation = append.history_handle;
    ++wrong_generation.content_generation;
    const term::Terminal_history_row_record_decode_result generation_failure =
        term::decode_terminal_history_row_record(read, wrong_generation);
    ok &= check(generation_failure.status ==
            term::Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH,
        "Phase 4B validates expected content generation during decode");

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
        "Phase 4B validates ring-framed record size against the row payload");

    return ok;
}

bool test_materialized_decode_owns_data_after_read_scope_and_eviction()
{
    bool ok = true;

    // The capacity bounds the largest admissible record at capacity / 8; the
    // 116-byte row-record header plus this fixture's cell and hyperlink
    // payload needs a 288-byte record budget, while twelve filler rows must
    // still overrun the ring so the original record is evicted.
    term::Terminal_history_ring ring({2304U, 2304U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 4B bounded ownership fixture ring initializes");

    term::Terminal_history_row_record record = make_base_record(
        71U,
        73U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        1);
    record.cells.push_back(make_cell(QStringLiteral("owned"), 1, true, 6U, 701U));
    record.hyperlink_identity_keys.emplace(
        701U,
        QByteArrayLiteral("uri:https://example.test/owned"));

    const term::Terminal_history_row_record_append_result first_append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(6U, 71U));
    ok &= check(first_append.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B ownership fixture first row encodes");

    term::Terminal_history_row_record_decode_result materialized;
    {
        const term::Terminal_history_ring_read_scope read =
            ring.read_record(first_append.commit.byte_sequence);
        materialized = term::decode_terminal_history_row_record(
            read,
            first_append.history_handle);
    }
    ok &= check(materialized.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4B materializes row data inside a bounded read scope");

    for (std::uint64_t row_sequence = 72U; row_sequence < 84U; ++row_sequence) {
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
                make_identity(6U, row_sequence));
        ok &= check(filler_append.status ==
                term::Terminal_history_row_record_codec_status::OK,
            "Phase 4B ownership fixture filler row encodes");
    }

    ok &= check(ring.read_record(first_append.commit.byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "Phase 4B ownership fixture evicts the original ring record");
    ok &= check(materialized.record.cells.front().text == QStringLiteral("owned") &&
            materialized.record.hyperlink_identity_keys.at(701U) ==
                QByteArrayLiteral("uri:https://example.test/owned"),
        "Phase 4B decoded materialization remains owned after read-scope end and ring eviction");

    return ok;
}

bool test_row_record_oversize_hard_fails_before_publication()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 8 oversize fixture ring initializes");

    term::Terminal_history_row_record oversized = make_base_record(
        121U,
        127U,
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        40);
    for (int column = 0; column < 40; ++column) {
        oversized.cells.push_back(make_cell(
            QStringLiteral("oversize-cell-text"),
            1,
            true));
    }

    const term::Terminal_history_row_record_append_result append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            oversized,
            make_identity(10U, 121U));
    ok &= check(append.status ==
            term::Terminal_history_row_record_codec_status::RING_RESERVE_FAILED,
        "Phase 8 row-record oversize hard-fails at ring reservation");
    ok &= check(append.ring_status == term::Terminal_history_ring_status::OVERSIZE_RECORD,
        "Phase 8 row-record oversize exposes the terminal-local oversize status");
    ok &= check(ring.head_byte_sequence() == 0U,
        "Phase 8 row-record oversize publishes no partial or fallback record");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_dense_and_blank_rows_round_trip();
    ok &= test_wide_clusters_styles_hyperlinks_and_recovery_round_trip();
    ok &= test_self_contained_hyperlinks_are_required();
    ok &= test_malformed_counts_are_bounded_before_allocation();
    ok &= test_header_footer_and_handle_validation_failures();
    ok &= test_materialized_decode_owns_data_after_read_scope_and_eviction();
    ok &= test_row_record_oversize_hard_fails_before_publication();
    return ok ? 0 : 1;
}
