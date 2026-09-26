#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/sixel_decoder.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_csi_byte_kind
{
    EMBEDDED_CONTROL,
    PARAMETER,
    INTERMEDIATE,
    FINAL,
    INVALID,
};

// The parser owns the byte grammar shared by incremental observers that must
// agree with its CSI recognition before they defer a sequence.
Terminal_csi_byte_kind terminal_csi_byte_kind(unsigned char byte);

class Terminal_byte_stream_parser
{
public:
    // Parses bytes from offset on and stops right after a sixel image
    // completes, so its caller applies each image before the next is decoded
    // and at most one decoded raster is alive at a time, however many images
    // one chunk describes. offset advances past what was parsed; the caller
    // calls again until it reaches bytes.size().
    std::vector<Parser_action> ingest(QByteArrayView bytes, qsizetype& offset);

    // Where a caller that yields after each sixel image should end the next
    // chunk it hands over: just past the first byte of bytes that could
    // complete an image, or bytes.size() when none can. The bound is
    // conservative, so a chunk may end early but never holds two image ends.
    // It reads the parser's state without changing it and scans each byte of
    // bytes at most once, up to the bound.
    qsizetype sixel_image_boundary(QByteArrayView bytes) const;

    // The decoded size a sixel image may reach; its owner keeps it equal to
    // the retained history's largest record.
    void set_sixel_raster_limit_bytes(std::size_t limit_bytes)
    {
        m_sixel_decoder.set_raster_limit_bytes(limit_bytes);
    }

private:
    enum class String_state_result
    {
        NOT_STRING,
        CONSUMED,
    };

    qsizetype ingest_buffer(
        QByteArrayView                 bytes,
        std::vector<Parser_action>&    actions);

    String_state_result try_start_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    String_state_result try_consume_escape_or_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void continue_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void classify_dcs_header(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void start_string(
        Parser_sequence_family         family,
        QByteArrayView                 bytes,
        qsizetype                      payload_begin,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    qsizetype find_string_terminator(
        QByteArrayView                 bytes,
        Parser_sequence_family         family,
        qsizetype                      payload_begin,
        Parser_string_terminator&      terminator);

    bool append_string_payload(
        Parser_sequence_family         family,
        QByteArrayView                 payload,
        std::vector<Parser_action>&    actions);

    void finish_string(
        Parser_sequence_family         family,
        Parser_string_terminator       terminator,
        std::vector<Parser_action>&    actions);

    void finish_sixel(
        Parser_string_terminator       terminator,
        std::vector<Parser_action>&    actions);

    void finish_csi_sequence(
        QByteArrayView                 bytes,
        qsizetype                      csi_begin,
        qsizetype                      final_offset,
        std::vector<Parser_action>&    actions);

    void handle_osc_payload(
        QByteArray                     payload,
        std::vector<Parser_action>&    actions);

    bool should_buffer_incomplete_utf8(
        QByteArrayView                 bytes,
        qsizetype                      offset) const;

    void emit_unsupported_control(
        QString                        source_sequence,
        Parser_sequence_family         family,
        std::vector<Parser_action>&    actions);

    void continue_discarded_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset);

    void continue_discarded_escape(
        QByteArrayView                 bytes,
        qsizetype&                     offset);

    QByteArray                 m_pending_prefix;
    QByteArray                 m_string_payload;
    Parser_sequence_family     m_string_family                 = Parser_sequence_family::NONE;
    bool                       m_string_over_limit             = false;
    Terminal_utf8_scan_state   m_string_utf8_scan_state;
    bool                       m_dcs_header_pending            = false;
    Sixel_decoder              m_sixel_decoder;
    bool                       m_discarding_csi                = false;
    bool                       m_discarding_escape             = false;
    std::uint64_t              m_next_host_request_id          = 1U;
};

}
