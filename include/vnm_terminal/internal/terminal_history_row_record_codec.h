#pragma once

#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/terminal_hyperlink.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_style.h"
#include <QByteArray>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_history_row_record_codec_status
{
    OK,
    INVALID_ARGUMENT,
    SIZE_OVERFLOW,
    RING_RESERVE_FAILED,
    RING_COMMIT_FAILED,
    RING_READ_FAILED,
    TRUNCATED_RECORD,
    INVALID_HEADER,
    INVALID_PAYLOAD,
    INVALID_ENUM,
    EPOCH_MISMATCH,
    BYTE_SEQUENCE_MISMATCH,
    ROW_SEQUENCE_MISMATCH,
    RECORD_SIZE_MISMATCH,
    CONTENT_GENERATION_MISMATCH,
};

enum class Terminal_history_row_record_payload_kind : std::uint32_t
{
    GENERIC_COMPACT    = 0U,
    PREFIX_PLAIN_ASCII = 1U,
};

struct Terminal_history_row_cell
{
    QString                        text = QStringLiteral(" ");
    int                            display_width = 1;
    bool                           wide_continuation = false;
    bool                           occupied = false;
    Terminal_style_id              style_id = k_default_terminal_style_id;
    Terminal_hyperlink_id          hyperlink_id = k_no_terminal_hyperlink_id;
};

struct Terminal_history_row_record
{
    std::vector<Terminal_history_row_cell>
                                   cells;
    std::vector<Terminal_text_style>
                                   style_table;
    Terminal_retained_line_provenance
                                   provenance;
    std::map<Terminal_hyperlink_id, QByteArray>
                                   hyperlink_identity_keys;
    terminal_retained_row_record_metadata_t
                                   metadata;
};

struct terminal_history_row_record_identity_t
{
    std::uint64_t                  epoch = 0U;
    std::uint64_t                  row_sequence = 0U;
};

struct terminal_history_row_record_payload_view_t
{
    std::uint64_t                  byte_sequence = 0U;
    std::uint32_t                  record_bytes = 0U;
    std::span<const std::byte>     payload;
};

struct Terminal_history_row_record_append_result
{
    Terminal_history_row_record_codec_status
                                   status = Terminal_history_row_record_codec_status::OK;
    Terminal_history_ring_status   ring_status = Terminal_history_ring_status::OK;
    terminal_history_ring_commit_result_t
                                   commit;
    terminal_history_handle_t      history_handle;
    Terminal_history_row_record_payload_kind
                                   payload_kind =
                                       Terminal_history_row_record_payload_kind::GENERIC_COMPACT;
};

struct Terminal_history_row_record_decode_result
{
    Terminal_history_row_record_codec_status
                                   status = Terminal_history_row_record_codec_status::OK;
    Terminal_history_ring_status   ring_status = Terminal_history_ring_status::OK;
    terminal_history_handle_t      history_handle;
    Terminal_history_row_record_payload_kind
                                   payload_kind =
                                       Terminal_history_row_record_payload_kind::GENERIC_COMPACT;
    Terminal_history_row_record    record;
};

Terminal_history_row_record_append_result encode_terminal_history_row_record_to_ring(
    Terminal_history_ring&                       ring,
    const Terminal_history_row_record&           record,
    terminal_history_row_record_identity_t       identity);

Terminal_history_row_record_decode_result decode_terminal_history_row_record(
    const Terminal_history_ring_read_scope&      read_scope,
    std::optional<terminal_history_handle_t>     expected_handle = std::nullopt);

Terminal_history_row_record_decode_result decode_terminal_history_row_record_payload(
    terminal_history_row_record_payload_view_t   payload_view,
    std::optional<terminal_history_handle_t>     expected_handle = std::nullopt);

terminal_history_prefix_plain_ascii_retention_estimate_t
make_terminal_history_prefix_plain_ascii_retention_estimate(
    std::uint64_t                                byte_budget,
    int                                          source_width_columns);

}
