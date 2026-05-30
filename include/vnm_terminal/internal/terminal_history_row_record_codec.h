#pragma once

#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
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
    INVALID_FOOTER,
    INVALID_ENUM,
    EPOCH_MISMATCH,
    BYTE_SEQUENCE_MISMATCH,
    ROW_SEQUENCE_MISMATCH,
    RECORD_SIZE_MISMATCH,
    CONTENT_GENERATION_MISMATCH,
};

struct Terminal_history_row_cell
{
    QString                        text = QStringLiteral(" ");
    int                            display_width = 1;
    bool                           wide_continuation = false;
    bool                           occupied = false;
    Terminal_style_id              style_id = k_default_terminal_style_id;
    std::uint64_t                  hyperlink_id = 0U;
};

struct Terminal_history_row_record
{
    std::vector<Terminal_history_row_cell>
                                   cells;
    Terminal_retained_line_provenance
                                   provenance;
    std::map<std::uint64_t, QByteArray>
                                   hyperlink_identity_keys;
    terminal_retained_row_record_metadata_t
                                   metadata;
};

struct terminal_history_row_record_identity_t
{
    std::uint64_t                  epoch = 0U;
    std::uint64_t                  row_sequence = 0U;
    std::uint64_t                  previous_row_byte_sequence = 0U;
    std::uint64_t                  previous_row_sequence = 0U;
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
};

struct Terminal_history_row_record_decode_result
{
    Terminal_history_row_record_codec_status
                                   status = Terminal_history_row_record_codec_status::OK;
    Terminal_history_ring_status   ring_status = Terminal_history_ring_status::OK;
    terminal_history_handle_t      history_handle;
    std::uint64_t                  previous_row_byte_sequence = 0U;
    std::uint64_t                  previous_row_sequence = 0U;
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

}
