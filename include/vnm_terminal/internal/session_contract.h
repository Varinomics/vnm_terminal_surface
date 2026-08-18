#pragma once

#include "vnm_terminal/backend_output_capture.h"
#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <cstddef>
#include <cstdint>

namespace vnm_terminal::internal {

class Terminal_transcript_recorder;

enum class Terminal_session_command_kind
{
    START,
    INTERRUPT,
    TERMINATE,
    FORCE_RELEASE_SYNCHRONIZED_OUTPUT,
    BACKEND_EXIT,
    BACKEND_ERROR,
    RESIZE,
    USER_WRITE,
    USER_PASTE,
    USER_MESSAGE,
    TERMINAL_REPLY,
    BACKEND_OUTPUT,
    TEXT_AREA_RESIZE_ARBITRATION,
};

enum class Terminal_queue_result_code
{
    ACCEPTED,
    HARD_LIMIT_REACHED,
};

enum class Terminal_session_notification_kind
{
    PROCESS_STARTED,
    PROCESS_EXITED,
    SNAPSHOT_READY,
    OUTPUT_ACTIVITY,
    OUTPUT_BACKPRESSURE_CHANGED,
    BACKEND_ERROR,
    BELL_REQUESTED,
    TITLE_CHANGED,
    ICON_NAME_CHANGED,
    RESIZE_TRANSACTION,
    TEXT_AREA_RESIZE_REQUESTED,
    HOST_REQUEST,
    TEXT_AREA_RESIZE_ARBITRATION_REQUESTED,
    TEXT_AREA_RESIZE_ARBITRATION_SETTLED,
};

enum class Terminal_session_result_code
{
    ACCEPTED,
    INVALID_STATE,
    INVALID_ARGUMENT,
    QUEUE_HARD_LIMIT_REACHED,
    BACKEND_REJECTED,
};

constexpr std::size_t k_terminal_default_output_queue_high_water_bytes =  64U * 1024U;
constexpr std::size_t k_terminal_default_output_queue_hard_limit_bytes = 256U * 1024U;

struct Terminal_queue_limits
{
    std::size_t high_water_bytes       = k_terminal_default_output_queue_high_water_bytes;
    std::size_t hard_limit_bytes       = k_terminal_default_output_queue_hard_limit_bytes;
    std::size_t high_water_commands    = 256U;
    std::size_t hard_limit_commands    = 1024U;
};

// Why an in-flight text-area resize arbitration ended. ACCEPTED and REJECTED
// are the host's own answer; the rest are settlements the session or the
// surface had to make because the host's answer became impossible or never
// arrived.
enum class Terminal_text_area_resize_arbitration_outcome
{
    ACCEPTED,
    REJECTED,
    HOLD_LIMIT_REACHED,
    HOST_GEOMETRY_CHANGED,
    TEXT_AREA_RESIZE_DISABLED,
    ARBITRATION_DISABLED,
    PROCESS_EXITED,
    TIMED_OUT,
};

// Sized to absorb one surface output high-water burst so ordinary throughput
// does not defeat a host that answers inside its deadline, while still bounding
// what a single unanswered request can retain.
constexpr std::size_t k_terminal_default_text_area_resize_hold_limit_bytes = 1024U * 1024U;

// Presence-tagged optional capability record. A session config without it keeps
// the sequence-point grid commit and never asks a host anything.
struct terminal_text_area_resize_arbitration_config_t
{
    std::size_t hold_limit_bytes = k_terminal_default_text_area_resize_hold_limit_bytes;
};

struct terminal_text_area_resize_arbitration_request_t
{
    std::uint64_t        request_id = 0U;
    terminal_grid_size_t requested_grid_size;
};

struct terminal_text_area_resize_arbitration_settlement_t
{
    std::uint64_t                                 request_id = 0U;
    Terminal_text_area_resize_arbitration_outcome outcome    =
        Terminal_text_area_resize_arbitration_outcome::REJECTED;
    terminal_grid_size_t                          effective_grid_size;
};

enum class Terminal_model_resize_result
{
    APPLIED,
    INVALID_GRID_SIZE,
    NOT_APPLIED,
};

enum class Terminal_backend_resize_result
{
    APPLIED,
    FAILED,
};

struct Terminal_resize_transaction
{
    std::uint64_t                  id                       = 0U;
    QSizeF                         source_geometry;
    terminal_grid_size_t           target_grid_size;
    Terminal_buffer_id             active_buffer            = Terminal_buffer_id::PRIMARY;
    Terminal_model_resize_result   model_result             = Terminal_model_resize_result::APPLIED;
    Terminal_backend_resize_result backend_result           = Terminal_backend_resize_result::FAILED;
    QSizeF                         snapshot_geometry;
    terminal_grid_size_t           snapshot_grid_size;
    bool                           backend_geometry_in_sync = true;
};

struct Terminal_session_command
{
    std::uint64_t                              sequence = 0U;
    std::uint64_t                              interaction_trace_id = 0U;
    std::uint64_t                              backend_callback_epoch = 0U;
    Terminal_session_command_kind              kind     = Terminal_session_command_kind::BACKEND_OUTPUT;
    QByteArray                                 bytes;
    std::optional<Terminal_launch_config>      launch_config;
    std::optional<Terminal_resize_transaction> resize;
    std::optional<Terminal_backend_exit>       exit;
    std::optional<Terminal_backend_error>      error;
    std::optional<terminal_text_area_resize_arbitration_settlement_t>
                                               text_area_resize_arbitration;
};

struct Terminal_session_result
{
    Terminal_session_result_code             code = Terminal_session_result_code::ACCEPTED;
    std::uint64_t                            sequence = 0U;
    bool                                     high_water_reached = false;
    std::optional<Terminal_backend_error>    error;
};

struct Terminal_queue_result
{
    Terminal_queue_result_code     code               = Terminal_queue_result_code::ACCEPTED;
    bool                           high_water_reached = false;
};

struct Terminal_session_notification
{
    Terminal_session_notification_kind          kind     =
        Terminal_session_notification_kind::SNAPSHOT_READY;
    std::uint64_t                                  sequence            = 0U;
    QString                                        message;
    std::optional<Terminal_backend_error>          backend_error;
    std::optional<Terminal_backend_exit>           exit;
    std::optional<Terminal_resize_transaction>     resize;
    bool                                           backpressure_active = false;
    std::optional<Terminal_osc52_write_request>    clipboard_write_request;
    std::optional<terminal_grid_size_t>            text_area_resize_request;
    bool                                           bell_audible = false;
    bool                                           bell_visual  = false;
    std::optional<terminal_text_area_resize_arbitration_request_t>
                                                   text_area_resize_arbitration_request;
    std::optional<terminal_text_area_resize_arbitration_settlement_t>
                                                   text_area_resize_arbitration_settlement;
};

struct Terminal_bell_policy
{
    bool                           audible_enabled       = true;
    bool                           visual_enabled        = true;
    std::uint64_t                  coalescing_window_ms  = 100U;
    std::size_t                    max_events_per_window = 1U;
};

struct Terminal_bell_state
{
    Terminal_bell_policy           policy;
    std::uint64_t                  window_start_ms  = 0U;
    std::size_t                    events_in_window = 0U;
};

struct Terminal_bell_request
{
    bool                           audible = false;
    bool                           visual  = false;
};

struct Terminal_session_config
{
    // With either backend-event notifier set, output bytes are first bounded in
    // callback ingress and then transferred into the session output queue, so
    // a stalled owner can transiently hold up to roughly two output hard limits.
    Terminal_queue_limits           output_queue_limits;
    Terminal_queue_limits           write_queue_limits;
    Terminal_bell_policy            bell_policy;
    std::function<std::uint64_t()>  bell_clock_ms;
    std::function<void()>           backend_event_notifier;
    // Runs after durable callback ingress and may run on a backend thread.
    // The pressure bit is true when this accepted output callback has reached
    // callback-ingress high water. Owners can select a pressure-safe wake path
    // without reading or mutating GUI-thread state.
    std::function<void(std::uint64_t, bool)>
                                    backend_event_epoch_notifier;
    std::size_t                     trace_command_limit                      = 0U;
    std::size_t                     trace_notification_limit                 = 0U;
    std::size_t                     trace_result_limit                       = 0U;
    std::size_t                     trace_resize_limit                       = 0U;
    std::size_t                     trace_output_chunk_limit                 = 0U;
    std::optional<Backend_output_capture_config>
                                    backend_output_capture_config;
    int                             scrollback_limit                         = 1000;
    std::size_t                     retained_history_capacity_bytes =
        k_terminal_default_retained_history_capacity_bytes;
    bool                            capture_last_model_ingest_result         = false;
    bool                            capture_dirty_row_stats                  = false;
    bool                            recover_scrollback_from_primary_repaints = false;
    bool                            selection_trace_enabled                  = false;
    bool                            selection_viewport_projection_enabled    = false;
    Terminal_synchronized_output_scroll_policy synchronized_output_scroll_policy =
        Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    Terminal_text_area_resize_policy text_area_resize_policy =
        Terminal_text_area_resize_policy::APPLICATION_CONTROLLED;
    // Absent means the two-phase transaction is not installed: CSI 8 t keeps the
    // sequence-point commit and the host is never asked. Present means output is
    // held at the sequence point until the host answers through
    // Terminal_session::settle_text_area_resize_arbitration. A host embedding
    // Terminal_session directly owns that answer: the session has no clock and
    // bounds the wait only by hold_limit_bytes.
    std::optional<terminal_text_area_resize_arbitration_config_t>
                                    text_area_resize_arbitration;
    std::shared_ptr<Terminal_transcript_recorder> transcript_recorder;
};

class Bounded_terminal_command_queue
{
public:
    explicit Bounded_terminal_command_queue(Terminal_queue_limits limits)
    :
        m_limits(limits)
    {}

    Terminal_queue_result enqueue(Terminal_session_command command);
    std::optional<Terminal_session_command> dequeue();

    std::size_t byte_count()    const { return m_bytes;         }
    std::size_t command_count() const { return m_command_count; }

    Terminal_queue_result would_accept(
        std::size_t    byte_count,
        std::size_t    command_count = 1U) const;

    Terminal_queue_result reserve(
        std::size_t    byte_count,
        std::size_t    command_count = 1U);

    void release(
        std::size_t    byte_count,
        std::size_t    command_count = 1U);

    bool high_water_reached() const;

private:
    Terminal_queue_limits                m_limits;
    std::deque<Terminal_session_command> m_commands;
    std::size_t                          m_bytes = 0U;
    std::size_t                          m_command_count = 0U;
};

Terminal_bell_request record_bell_event(
    Terminal_bell_state&   state,
    std::uint64_t          timestamp_ms);

Terminal_session_command make_terminal_reply_command(
    std::uint64_t          sequence,
    const Terminal_reply&  reply);

Terminal_session_command make_start_command(
    std::uint64_t          sequence,
    Terminal_launch_config launch_config);

Terminal_session_command make_interrupt_command(
    std::uint64_t          sequence);

Terminal_session_command make_terminate_command(
    std::uint64_t          sequence);

Terminal_session_command make_force_release_synchronized_output_command(
    std::uint64_t          sequence);

Terminal_session_command make_backend_exit_command(
    std::uint64_t          sequence,
    Terminal_backend_exit  exit);

Terminal_session_command make_backend_error_command(
    std::uint64_t          sequence,
    Terminal_backend_error error);

Terminal_session_command make_backend_output_command(
    std::uint64_t          sequence,
    QByteArray             bytes);

Terminal_session_command make_user_write_command(
    std::uint64_t          sequence,
    QByteArray             bytes,
    std::uint64_t          interaction_trace_id = 0U);

Terminal_session_command make_user_paste_command(
    std::uint64_t          sequence,
    QByteArray             bytes,
    std::uint64_t          interaction_trace_id = 0U);

Terminal_session_command make_user_message_command(
    std::uint64_t          sequence,
    QByteArray             bytes,
    std::uint64_t          interaction_trace_id = 0U);

Terminal_session_command make_resize_command(
    std::uint64_t                  sequence,
    Terminal_resize_transaction    resize);

}
