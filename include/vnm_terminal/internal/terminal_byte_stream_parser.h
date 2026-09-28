#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/sixel_decoder.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
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
    static constexpr std::size_t k_macro_storage_limit_bytes = 6U * 1024U;
    static constexpr std::size_t k_macro_expansion_limit_bytes = 1024U * 1024U;
    // Parses from offset and stops after each sixel string or stored-macro
    // invocation. This lets the caller apply an image before another is
    // decoded; the next call expands a macro before resuming later host bytes.
    // It also stops where the budget cannot pay for the next sixel step.
    // Call again while input remains or macro_work_pending() is true.
    std::vector<Parser_action> ingest(
        QByteArrayView       bytes,
        qsizetype&           offset,
        Sixel_work_budget*   budget = nullptr,
        std::size_t*         macro_replay_bytes_remaining = nullptr);

    // Whether the last ingest stopped because its budget ran out.
    bool sixel_work_deferred() const { return m_sixel_work_deferred; }
    bool macro_work_pending() const { return !m_macro_frames.empty(); }
    bool macro_work_advanced() const { return m_macro_work_advanced; }
    bool macro_work_deferred() const { return m_macro_work_deferred; }
    bool parse_boundary_reached() const { return m_parser_boundary_reached; }
    // A UTF-8 scalar started in replayed macro text or a string payload may
    // finish in the next host span. Session scanners must share this carry.
    std::optional<Terminal_utf8_scan_state> pending_utf8_scan_state() const;
    void begin_macro_expansion_scope()
    {
        m_macro_expansion_bytes = 0U;
        m_macro_expansion_scope_active = true;
    }
    void end_macro_expansion_scope() { m_macro_expansion_scope_active = false; }

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

    void continue_sixel_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

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

    bool define_macro(QByteArrayView payload);
    void invoke_macro(
        QByteArrayView                 parameter_bytes,
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
    Sixel_work_budget*         m_sixel_work_budget             = nullptr;
    bool                       m_sixel_work_deferred           = false;
    // A sixel string ended since the parse loop last looked.
    bool                       m_sixel_string_ended            = false;
    bool                       m_discarding_csi                = false;
    bool                       m_discarding_escape             = false;
    struct Macro_frame
    {
        int        id = 0;
        QByteArray bytes;
        qsizetype  offset = 0;
    };
    std::array<std::optional<QByteArray>, 64> m_macros;
    std::deque<Macro_frame>    m_macro_frames;
    std::size_t                m_macro_storage_bytes           = 0U;
    std::size_t                m_macro_expansion_bytes         = 0U;
    bool                       m_macro_expansion_scope_active   = false;
    bool                       m_macro_boundary_reached        = false;
    bool                       m_macro_work_advanced           = false;
    bool                       m_macro_work_deferred           = false;
    bool                       m_parser_boundary_reached       = false;
    std::uint64_t              m_next_host_request_id          = 1U;
};

}
