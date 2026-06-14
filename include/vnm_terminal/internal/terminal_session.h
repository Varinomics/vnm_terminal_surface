#pragma once

#include "vnm_terminal/internal/session_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_public_projection.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <cstddef>
#include <cstdint>
#include <vector>

class QFile;
class QKeyEvent;

namespace vnm_terminal::internal {

class Terminal_session_callback_lifetime;

struct Terminal_input_event_result
{
    bool                       handled = false;
    Terminal_session_result    result;
};

using Terminal_key_event_result   = Terminal_input_event_result;
using Terminal_mouse_event_result = Terminal_input_event_result;
using Terminal_ime_commit_result  = Terminal_input_event_result;
using Terminal_paste_text_result  = Terminal_input_event_result;
using Terminal_focus_event_result = Terminal_input_event_result;

enum class Terminal_lazy_snapshot_fallback_reason
{
    NONE,
    MISSING_PREVIOUS_CONTENT_SNAPSHOT,
    GRID_MISMATCH,
    VIEWPORT_MISMATCH,
    ACTIVE_BUFFER_MISMATCH,
    PUBLIC_PROJECTION,
    ROW_ORIGIN_GENERATION_MISMATCH,
    STYLE_COLOR_MODE_INCOMPATIBILITY,
    HYPERLINK_NAMESPACE_INCOMPATIBILITY,
    UNSTABLE_DIRTY_ROW_MUTATION_IDENTITY,
    UNSUPPORTED_GEOMETRY_OR_DETACHED_SNAPSHOT_PATH,
};

enum class Terminal_lazy_snapshot_evidence_mode
{
    ROW_VIEW_PARITY_TEST,
    PUBLICATION_CANDIDATE_NO_MATERIALIZATION,
};

struct Terminal_lazy_snapshot_fallback_reason_counters
{
    std::uint64_t missing_previous_content_snapshot        = 0U;
    std::uint64_t grid_mismatch                            = 0U;
    std::uint64_t viewport_mismatch                        = 0U;
    std::uint64_t active_buffer_mismatch                   = 0U;
    std::uint64_t public_projection                        = 0U;
    std::uint64_t row_origin_generation_mismatch           = 0U;
    std::uint64_t style_color_mode_incompatibility         = 0U;
    std::uint64_t hyperlink_namespace_incompatibility      = 0U;
    std::uint64_t unstable_dirty_row_mutation_identity     = 0U;
    std::uint64_t unsupported_geometry_or_detached_snapshot_path = 0U;
};

struct terminal_lazy_snapshot_fallback_reason_descriptor_t
{
    Terminal_lazy_snapshot_fallback_reason reason;
    const char*                            key;
    const char*                            profile_key;
    std::uint64_t Terminal_lazy_snapshot_fallback_reason_counters::*counter;
};

inline constexpr terminal_lazy_snapshot_fallback_reason_descriptor_t
    k_terminal_lazy_snapshot_fallback_reason_descriptors[] = {
        {
            Terminal_lazy_snapshot_fallback_reason::
                MISSING_PREVIOUS_CONTENT_SNAPSHOT,
            "missing_previous_content_snapshot",
            "lazy_snapshot_fallback_missing_previous_content_snapshot",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                missing_previous_content_snapshot,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH,
            "grid_mismatch",
            "lazy_snapshot_fallback_grid_mismatch",
            &Terminal_lazy_snapshot_fallback_reason_counters::grid_mismatch,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH,
            "viewport_mismatch",
            "lazy_snapshot_fallback_viewport_mismatch",
            &Terminal_lazy_snapshot_fallback_reason_counters::viewport_mismatch,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::ACTIVE_BUFFER_MISMATCH,
            "active_buffer_mismatch",
            "lazy_snapshot_fallback_active_buffer_mismatch",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                active_buffer_mismatch,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::PUBLIC_PROJECTION,
            "public_projection",
            "lazy_snapshot_fallback_public_projection",
            &Terminal_lazy_snapshot_fallback_reason_counters::public_projection,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::
                ROW_ORIGIN_GENERATION_MISMATCH,
            "row_origin_generation_mismatch",
            "lazy_snapshot_fallback_row_origin_generation_mismatch",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                row_origin_generation_mismatch,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::
                STYLE_COLOR_MODE_INCOMPATIBILITY,
            "style_color_mode_incompatibility",
            "lazy_snapshot_fallback_style_color_mode_incompatibility",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                style_color_mode_incompatibility,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::
                HYPERLINK_NAMESPACE_INCOMPATIBILITY,
            "hyperlink_namespace_incompatibility",
            "lazy_snapshot_fallback_hyperlink_namespace_incompatibility",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                hyperlink_namespace_incompatibility,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::
                UNSTABLE_DIRTY_ROW_MUTATION_IDENTITY,
            "unstable_dirty_row_mutation_identity",
            "lazy_snapshot_fallback_unstable_dirty_row_mutation_identity",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                unstable_dirty_row_mutation_identity,
        },
        {
            Terminal_lazy_snapshot_fallback_reason::
                UNSUPPORTED_GEOMETRY_OR_DETACHED_SNAPSHOT_PATH,
            "unsupported_geometry_or_detached_snapshot_path",
            "lazy_snapshot_fallback_unsupported_geometry_or_detached_snapshot_path",
            &Terminal_lazy_snapshot_fallback_reason_counters::
                unsupported_geometry_or_detached_snapshot_path,
        },
    };

inline constexpr std::span<const terminal_lazy_snapshot_fallback_reason_descriptor_t>
terminal_lazy_snapshot_fallback_reason_descriptors()
{
    return std::span<const terminal_lazy_snapshot_fallback_reason_descriptor_t>(
        k_terminal_lazy_snapshot_fallback_reason_descriptors);
}

inline std::uint64_t terminal_lazy_snapshot_fallback_reason_counter(
    const Terminal_lazy_snapshot_fallback_reason_counters&       counters,
    const terminal_lazy_snapshot_fallback_reason_descriptor_t&   descriptor)
{
    return counters.*descriptor.counter;
}

struct Terminal_session_lazy_snapshot_composer_result
{
    bool                                    eligible = false;
    Terminal_lazy_snapshot_fallback_reason  fallback_reason =
        Terminal_lazy_snapshot_fallback_reason::NONE;
    std::optional<Terminal_render_snapshot> lazy_snapshot;
    bool                                    materialization_matches_full_snapshot = false;
    bool                                    materialization_mismatch_for_testing  = false;
    std::uint64_t                           dirty_rows_visible = 0U;
    std::uint64_t                           previous_snapshot_borrow_candidate_rows = 0U;
    std::uint64_t                           previous_snapshot_borrowed_rows = 0U;
    std::uint64_t                           producer_owned_rows = 0U;
    std::uint64_t                           producer_materialized_rows = 0U;
    std::uint64_t                           producer_cells_scanned = 0U;
    std::uint64_t                           producer_cells_emitted = 0U;
    std::uint64_t                           consumer_materialization_calls = 0U;
    std::uint64_t                           consumer_materialization_rows = 0U;
    std::uint64_t                           consumer_materialization_cells = 0U;
};

struct Terminal_session_profile_stats
{
    bool                       enabled                               = false;
    std::uint64_t              render_snapshot_requests              = 0U;
    std::uint64_t              render_snapshots_constructed          = 0U;
    std::uint64_t              render_snapshot_publications          = 0U;
    std::uint64_t              full_snapshot_publications            = 0U;
    std::uint64_t              content_snapshot_publications         = 0U;
    std::uint64_t              selection_snapshot_publications       = 0U;
    std::uint64_t              geometry_snapshot_publications        = 0U;
    std::uint64_t              public_projection_scroll_requests     = 0U;
    std::uint64_t              public_projection_scroll_publications = 0U;
    std::uint64_t              dirty_coalescing_attempts             = 0U;
    std::uint64_t              dirty_coalescing_applied              = 0U;
    std::uint64_t              zero_dirty_snapshot_publications      = 0U;
    std::uint64_t              snapshots_superseded_before_render    = 0U;
    std::uint64_t              snapshots_marked_rendered            = 0U;
    std::uint64_t              snapshots_consumed_by_bridge          = 0U;
    std::uint64_t              max_unrendered_snapshot_generations   = 0U;
    std::uint64_t              geometry_derived_materialization_calls = 0U;
    std::uint64_t              geometry_derived_materialization_rows  = 0U;
    std::uint64_t              geometry_derived_materialization_cells = 0U;
    std::uint64_t              row_view_parity_materialization_calls  = 0U;
    std::uint64_t              row_view_parity_materialization_rows   = 0U;
    std::uint64_t              row_view_parity_materialization_cells  = 0U;
    std::uint64_t              retained_snapshot_payload_bytes       = 0U;
    std::uint64_t              retained_snapshot_generation_count    = 0U;
    std::uint64_t              max_retained_snapshot_payload_bytes   = 0U;
    std::uint64_t              max_retained_snapshot_generation_count = 0U;
    std::uint64_t              lazy_snapshot_eligibility_checks      = 0U;
    std::uint64_t              lazy_snapshot_eligible_checks         = 0U;
    std::uint64_t              lazy_snapshot_full_fallbacks          = 0U;
    std::uint64_t              lazy_snapshot_dirty_rows_visible      = 0U;
    std::uint64_t              lazy_snapshot_previous_snapshot_borrow_candidate_rows = 0U;
    std::uint64_t              lazy_snapshot_previous_snapshot_borrowed_rows = 0U;
    std::uint64_t              lazy_snapshot_producer_owned_rows     = 0U;
    std::uint64_t              lazy_snapshot_producer_materialized_rows = 0U;
    std::uint64_t              lazy_snapshot_producer_cells_scanned  = 0U;
    std::uint64_t              lazy_snapshot_producer_cells_emitted  = 0U;
    std::uint64_t              lazy_snapshot_materialization_mismatches_for_testing = 0U;
    Terminal_lazy_snapshot_fallback_reason_counters lazy_snapshot_fallback_reasons;
};

class Terminal_session
{
public:
    Terminal_session(
        std::unique_ptr<Terminal_backend>      backend,
        Terminal_session_config                config = {});

    ~Terminal_session();

    Terminal_session(const Terminal_session&)            = delete;
    Terminal_session& operator=(const Terminal_session&) = delete;

    Terminal_session_result start(
        Terminal_launch_config     launch_config);

    Terminal_session_result write_user_bytes(
        QByteArray                 bytes);
    // Writes already-encoded user input without draining backend callbacks.
    // Callers must choose bytes from already-published state and must avoid
    // this route when pending backend callbacks make protocol state unsafe.
    Terminal_session_result write_user_bytes_without_backend_drain(
        QByteArray                 bytes);
    // Writes already-encoded user input without draining backend callbacks
    // only when no backend callback events are queued or active under the
    // session lock. Returns std::nullopt without writing when callback ingress
    // could make the published protocol state stale.
    std::optional<Terminal_session_result>
        try_write_user_bytes_without_backend_drain_if_callbacks_empty(
            QByteArray             bytes);

    Terminal_key_event_result write_key_event(
        const QKeyEvent&           event);

    Terminal_mouse_event_result write_mouse_event(
        Terminal_mouse_event       event);
    // Writes a terminal mouse report without draining backend callbacks only
    // when callback ingress is empty under the session lock. Returns std::nullopt
    // without writing when pending callbacks could make mouse protocol state stale.
    std::optional<Terminal_mouse_event_result>
        try_write_mouse_event_without_backend_drain_if_callbacks_empty(
            Terminal_mouse_event   event);

    Terminal_ime_commit_result write_ime_commit(
        QString                    text);

    Terminal_paste_text_result write_paste_text(
        QString                                text,
        Terminal_paste_framing_policy          policy);

    Terminal_focus_event_result write_focus_event(
        bool                       focused);

    void set_ime_preedit(
        QString                    text,
        int                        cursor_position);

    void cancel_ime_preedit();

    Terminal_session_result write_terminal_reply(
        const Terminal_reply&      reply);

    Terminal_session_result resize(
        QSizeF                     source_geometry,
        terminal_grid_size_t       grid_size);

    Terminal_viewport_scroll_result scroll_viewport_lines(
        int                        line_delta);

    Terminal_viewport_scroll_result scroll_viewport_lines_from_published_state(
        int                        line_delta,
        Terminal_viewport_state    published_viewport);

    Terminal_viewport_scroll_result scroll_published_viewport_lines(
        int                        line_delta);

    Terminal_viewport_scroll_result scroll_published_viewport_to_offset_from_tail(
        int                        offset_from_tail);

    void set_selection_range(
        Terminal_selection_range   range);
    void set_selection_range_from_published_source(
        Terminal_selection_range   range,
        terminal_selection_source_identity_t source);
    // Caller must have just drained backend callbacks and pending commands, then
    // pass the current published source from that same drain. This variant skips
    // a second drain so a GUI-event selection proof stays coherent.
    void set_selection_range_from_drained_published_source(
        Terminal_selection_range   range,
        terminal_selection_source_identity_t source);
    // Detaches public visual selection state while preserving any cached payload.
    // During synchronized output, this publishes a selection-only snapshot from
    // the last public content so held output remains unpublished.
    void detach_selection_visual_attachment();

    void clear_selection();
    void set_scrollback_limit(int limit);
    void set_color_state(Terminal_color_state state);
    void set_primary_repaint_recovery_enabled(bool enabled);
    Terminal_session_result interrupt();
    Terminal_session_result terminate();
    Terminal_session_result force_release_synchronized_output();
    // Releases held synchronized output without draining queued backend callbacks.
    // Timeout recovery uses this after a bounded catch-up so sustained output
    // cannot postpone stale-frame publication indefinitely.
    Terminal_session_result force_release_synchronized_output_without_backend_drain();

    Terminal_process_state process_state() const;
    bool backend_ready() const;
    bool backend_geometry_in_sync() const;
    bool output_backpressure_active() const;
    bool render_publication_blocked() const;
    Terminal_synchronized_output_scroll_policy effective_synchronized_output_scroll_policy() const;
    bool has_pending_backend_callback_events() const;
    std::size_t pending_backend_callback_event_count() const;
    std::uint64_t backend_callback_enqueue_epoch() const;
    std::uint64_t backend_callback_processed_epoch() const;
    bool mouse_reporting_active() const;
    bool alternate_scroll_active() const;
    std::uint64_t alternate_scroll_mode_generation() const;
    Terminal_viewport_state viewport_state() const;
    terminal_grid_size_t grid_size() const;
    std::uint64_t last_processed_sequence() const;
    bool has_selection() const;
    Terminal_selection_result selected_text() const;
    Terminal_selection_anchor_domain selection_anchor_domain() const;
    std::optional<terminal_selection_visual_lease_t> selection_visual_lease() const;
    std::optional<terminal_selection_source_identity_t> published_selection_source_identity() const;

    std::vector<Terminal_session_command> processed_commands() const;
    std::vector<Terminal_session_notification> notifications() const;
    /**
     * @brief Drain public session notifications since the previous call.
     *
     * notifications() remains a bounded diagnostic trace; callers that deliver
     * public events must use this drainable channel. The channel coalesces
     * high-frequency state notifications and keeps a bounded critical-event
     * queue so trace truncation does not drive public signal delivery.
     */
    std::vector<Terminal_session_notification> take_pending_notifications();
    std::vector<Terminal_resize_transaction> resize_transactions() const;
    std::vector<QByteArray> output_chunks() const;
    std::optional<Terminal_render_snapshot> latest_render_snapshot() const;
    std::shared_ptr<const Terminal_render_snapshot> latest_render_snapshot_handle() const;
    std::optional<Terminal_render_snapshot> latest_content_render_snapshot_for_testing() const;
    std::optional<Terminal_public_projection> capture_public_projection_for_testing();
    std::optional<Terminal_public_projection> public_projection_for_testing() const;
    void install_public_projection_for_testing(Terminal_public_projection projection);
    std::optional<Terminal_viewport_state> public_viewport_for_testing() const;
    std::optional<Terminal_public_release_intent> public_release_intent_for_testing() const;
    Terminal_public_viewport_scroll_result scroll_public_projection_viewport_lines_for_testing(
        int                        line_delta);
    Terminal_public_viewport_scroll_result
        scroll_public_projection_viewport_to_offset_from_tail_for_testing(
            int                    offset_from_tail);
    Terminal_public_viewport_scroll_result
        scroll_public_projection_viewport_to_tail_for_testing();
    void invalidate_public_projection_for_testing(
        Terminal_public_projection_disable_reason reason);
    void set_synchronized_output_scroll_policy(
        Terminal_synchronized_output_scroll_policy policy);
    void set_synchronized_output_scroll_policy_for_testing(
        Terminal_synchronized_output_scroll_policy policy);
    std::uint64_t render_snapshot_generation() const;
    void mark_render_snapshot_synced(std::uint64_t generation);
    Ime_preedit_state ime_preedit_state() const;
    std::uint64_t ime_preedit_generation() const;
    std::optional<Terminal_screen_model_result> last_model_ingest_result() const;
    void set_dirty_row_stats_enabled(bool enabled);
    Terminal_screen_model_dirty_row_stats dirty_row_stats() const;
    Terminal_screen_model_dirty_row_timeline dirty_row_timeline() const;
    void set_profile_stats_enabled(bool enabled);
    Terminal_screen_model_profile_stats model_profile_stats() const;
    Terminal_session_profile_stats profile_stats() const;
    Terminal_session_lazy_snapshot_composer_result
        compose_lazy_render_snapshot_for_benchmark_evidence(
            std::shared_ptr<const Terminal_render_snapshot> previous_content_snapshot,
            const Terminal_render_snapshot& full_snapshot,
            Terminal_lazy_snapshot_evidence_mode evidence_mode,
            bool unsupported_geometry_or_detached_snapshot_path = false);
    Terminal_session_lazy_snapshot_composer_result compose_lazy_render_snapshot_for_testing(
        std::shared_ptr<const Terminal_render_snapshot> previous_content_snapshot,
        const Terminal_render_snapshot& full_snapshot,
        bool unsupported_geometry_or_detached_snapshot_path = false);
    Terminal_session_lazy_snapshot_composer_result compose_lazy_render_snapshot_for_testing(
        const Terminal_render_snapshot* previous_content_snapshot,
        const Terminal_render_snapshot& full_snapshot,
        bool unsupported_geometry_or_detached_snapshot_path = false);
    std::optional<Terminal_backend_exit> exit_status() const;

    /**
     * @brief Drain backend callbacks that were posted through Terminal_backend_callbacks.
     *
     * When Terminal_session_config::backend_event_notifier is set, backend callback
     * threads only enqueue into the bounded callback ingress and invoke the notifier.
     * The owner must then call this method on the session owner thread before reading
     * session state or taking pending notifications. Without a notifier the callback
     * path calls this method inline, preserving the synchronous test/backend contract.
     */
    void process_backend_callback_events();
    bool process_backend_callback_events_for(
        std::chrono::steady_clock::duration     budget);
    bool process_backend_callback_events_until_epoch(
        std::uint64_t                           target_epoch);

private:
    enum class Queue_category
    {
        NONE,
        OUTPUT,
        WRITE,
    };

    enum class User_write_viewport_policy
    {
        RETURN_TO_TAIL,
        PRESERVE_VIEWPORT,
    };

    enum class Backend_callback_drain_policy
    {
        DRAIN_CALLBACKS,
        KEEP_CALLBACKS_QUEUED,
    };

    struct Live_primary_viewport_anchor
    {
        terminal_history_handle_t       history_handle;
        int                             viewport_row = 0;
    };

    struct Deferred_backend_content_snapshot
    {
        Terminal_screen_model_result        model_result;
        Terminal_public_scroll_diagnostics public_scroll_diagnostics;
        QString                            message;
        std::uint64_t                      sequence = 0U;
    };

    Terminal_session_result enqueue_command(
        Terminal_session_command   command);

    Terminal_session_result enqueue_and_process_synchronous_command(
        Terminal_session_command           command,
        Backend_callback_drain_policy      drain_policy =
            Backend_callback_drain_policy::DRAIN_CALLBACKS);

    bool process_pending_commands(
        Backend_callback_drain_policy      drain_policy =
            Backend_callback_drain_policy::DRAIN_CALLBACKS,
        std::optional<std::chrono::steady_clock::time_point>
                                            deadline = std::nullopt,
        std::optional<std::uint64_t>       target_backend_callback_epoch =
            std::nullopt);

    Terminal_session_result process_command(
        Terminal_session_command   command);

    Terminal_session_result process_start_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_write_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_resize_command(
        Terminal_session_command   command);

    void finalize_resize_transaction(
        const Terminal_resize_transaction&     resize,
        std::uint64_t                          sequence,
        QString                                message);

    void publish_resize_outcome(
        std::uint64_t                          sequence,
        bool                                   render_snapshot_available,
        bool                                   grid_size_changed,
        bool                                   geometry_transition_warrants_publication,
        const Terminal_screen_model_result&    model_result,
        const Terminal_viewport_state&         previous_viewport,
        terminal_grid_size_t                   previous_grid_size);

    Terminal_session_result process_interrupt_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_terminate_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_force_release_synchronized_output_command(
        const Terminal_session_command&        command);
    Terminal_session_result force_release_synchronized_output_locked(
        std::uint64_t                          sequence);

    Terminal_session_result process_backend_output_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_backend_exit_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_backend_error_command(
        const Terminal_session_command&        command);

    Terminal_session_result write_user_bytes_locked(
        QByteArray                         bytes,
        User_write_viewport_policy         viewport_policy,
        Backend_callback_drain_policy      drain_policy =
            Backend_callback_drain_policy::DRAIN_CALLBACKS);

    Terminal_key_event_result write_key_event_locked(
        const QKeyEvent&                   event,
        Backend_callback_drain_policy      drain_policy);

    Terminal_mouse_event_result write_mouse_event_locked(
        Terminal_mouse_event               event,
        Backend_callback_drain_policy      drain_policy);

    Terminal_viewport_scroll_result scroll_viewport_lines_locked(
        int                                line_delta);

    Terminal_backend_callbacks make_backend_callbacks();
    void drain_backend_callback_commands();
    void pause_backend_output_from_callback_ingress();

    template<class T>
    static void push_bounded(std::vector<T>& values, T value, std::size_t limit)
    {
        values.push_back(std::move(value));
        if (values.size() > limit) {
            values.erase(values.begin());
        }
    }

    void record_processed_command(
        Terminal_session_command   command);

    void record_notification(
        Terminal_session_notification          notification);

    void record_pending_notification(
        Terminal_session_notification          notification);

    void record_resize_transaction(
        Terminal_resize_transaction            resize);

    void record_backend_output_capture_chunk(
        QByteArrayView             bytes);

    void record_output_chunk(
        QByteArray                 bytes);

    void record_output_activity(
        std::uint64_t              sequence);

    void record_backend_error(
        std::uint64_t              sequence,
        Terminal_backend_error     error);

    void initialize_screen_model(
        terminal_grid_size_t       grid_size);

    void ingest_backend_output_segment(
        std::uint64_t              sequence,
        QByteArrayView             bytes);

    void defer_backend_content_snapshot(
        std::uint64_t                          sequence,
        QString                                message,
        const Terminal_screen_model_result&    model_result,
        Terminal_public_scroll_diagnostics     public_scroll_diagnostics);

    void flush_deferred_backend_content_snapshot();

    bool handle_parser_actions(
        std::uint64_t                          sequence,
        const Terminal_screen_model_result&    result);

    bool apply_text_area_resize_request(
        std::uint64_t              sequence,
        terminal_grid_size_t       grid_size);

    void handle_bell_request(
        std::uint64_t              sequence);

    void sync_viewport_from_model_result(
        const Terminal_screen_model_result&    result,
        std::optional<Live_primary_viewport_anchor>
                                             detached_viewport_anchor);
    std::optional<Live_primary_viewport_anchor>
        capture_live_primary_detached_viewport_anchor() const;
    void resolve_live_primary_detached_viewport_anchor(
        const Live_primary_viewport_anchor&    detached_viewport_anchor);
    void scroll_live_primary_viewport_to_offset_from_tail(
        int                                    offset_from_tail);

    bool publish_viewport_snapshot_if_allowed(
        std::uint64_t              sequence,
        QString                    message);

    bool return_viewport_to_tail_after_user_input(
        std::uint64_t              sequence);

    void publish_selection_snapshot(
        std::uint64_t              sequence,
        QString                    message,
        bool                       allow_blocked_selection_only_snapshot = false);

    void advance_selection_content_basis_for_model_result(
        const Terminal_screen_model_result&    result,
        const Terminal_viewport_state&         previous_viewport,
        terminal_grid_size_t                   previous_grid_size);

    void record_blocked_synchronized_row_origin_change(
        const Terminal_screen_model_result&    result);

    Terminal_screen_model_result model_result_with_deferred_synchronized_row_origins(
        Terminal_screen_model_result           result);

    std::optional<terminal_selection_source_identity_t>
        published_selection_source_identity_unlocked() const;

    void set_selection_range_from_published_source_locked(
        Terminal_selection_range               range,
        std::optional<terminal_selection_source_identity_t>
                                            expected_source);

    terminal_selection_visual_lease_t make_selection_visual_lease(
        Terminal_selection_range               range) const;

    terminal_selection_visual_lease_t make_selection_visual_lease(
        Terminal_selection_range               range,
        const terminal_selection_source_identity_t& source) const;

    Terminal_render_snapshot_request make_render_snapshot_request(
        std::uint64_t                      sequence,
        Terminal_render_snapshot_purpose   purpose,
        Terminal_public_scroll_diagnostics public_scroll_diagnostics = {}) const;

    void record_snapshot_publication_queued_for_bridge();

    void publish_render_snapshot(
        std::uint64_t              sequence,
        QString                    message,
        Terminal_render_snapshot_purpose
                                   purpose = Terminal_render_snapshot_purpose::CONTENT,
        Terminal_public_scroll_diagnostics public_scroll_diagnostics = {});

    bool publish_public_projection_scroll_snapshot(
        std::uint64_t                  sequence,
        QString                        message,
        const Terminal_viewport_state& public_viewport_before,
        const Terminal_viewport_state& public_viewport_after);
    Terminal_viewport_scroll_result finish_public_projection_scroll(
        Terminal_public_viewport_scroll_result scroll_result,
        Terminal_public_viewport_controller    public_viewport_controller_before,
        Terminal_viewport_state                public_viewport_before,
        QString                                message);

    void publish_synchronized_resize_snapshot(
        std::uint64_t              sequence,
        QString                    message);

    bool selection_range_is_valid_for_active_model(
        const Terminal_selection_range&        range) const;

    bool public_projection_hold_active() const;
    Terminal_synchronized_output_policy_change_event
        synchronized_output_policy_change_event() const;
    bool immediate_public_projection_policy_enabled() const;
    void latch_synchronized_output_scroll_policy_for_new_hold();
    void reset_synchronized_output_policy_lifecycle();
    bool capture_public_projection_from_latest_content_basis();
    std::optional<Terminal_public_scroll_diagnostics> reconcile_public_projection_release(
        const Terminal_public_release_intent& release_intent,
        Terminal_viewport_state               live_viewport_before_on_release);
    std::optional<Terminal_public_scroll_diagnostics>
        synchronized_output_policy_change_diagnostics() const;
    void reset_public_projection_lifecycle();
    void invalidate_public_projection(
        Terminal_public_projection_disable_reason reason,
        Terminal_public_scroll_diagnostic_reason  diagnostic_reason =
            Terminal_public_scroll_diagnostic_reason::NONE);

    void advance_ime_preedit_generation();

    void record_result(
        Terminal_session_result    result);

    Terminal_session_result result_for_sequence(
        std::uint64_t              sequence,
        Terminal_session_result    fallback) const;

    Terminal_session_result result_after_processing(
        std::uint64_t              sequence,
        Terminal_session_result    enqueue_result) const;

    std::uint64_t bell_clock_milliseconds() const;
    void begin_result_capture(std::uint64_t sequence);
    void end_result_capture();

    Terminal_session_result make_rejected_result(
        std::uint64_t                          sequence,
        Terminal_session_result_code           code,
        Terminal_backend_error                 error) const;

    Terminal_session_result make_accepted_result(
        std::uint64_t                          sequence) const;

    Terminal_session_result make_backend_rejected_result(
        std::uint64_t                          sequence,
        std::optional<Terminal_backend_error>  error) const;

    std::uint64_t next_sequence();
    std::uint64_t next_resize_id();
    Queue_category queue_category_for(Terminal_session_command_kind kind) const;
    Bounded_terminal_command_queue& queue_for(Queue_category category);
    const Bounded_terminal_command_queue& queue_for(Queue_category category) const;
    bool is_session_writable() const;

    Terminal_queue_result would_accept_command(
        Queue_category             category,
        std::size_t                byte_count,
        std::size_t                command_count) const;

    void add_to_queue_state(
        Queue_category             category,
        std::size_t                byte_count);

    void remove_from_queue_state(
        Queue_category             category,
        std::size_t                byte_count,
        std::size_t                command_count = 1U);

    bool queue_high_water_reached(
        Queue_category             category) const;

    void set_output_backpressure_active(
        bool                       active,
        std::uint64_t              sequence);

    Terminal_session_result handle_output_overflow(
        std::uint64_t              sequence,
        QString                    message);

    void terminate_after_output_overflow(
        std::uint64_t              sequence);

    bool should_ignore_backend_output_after_stop(
        std::uint64_t              sequence) const;

    mutable std::recursive_mutex                           m_mutex;
    std::shared_ptr<Terminal_session_callback_lifetime>    m_callback_lifetime;

    std::unique_ptr<Terminal_backend>                      m_backend;
    Terminal_session_config                                m_config;
    std::deque<Terminal_session_command>                   m_pending_commands;
    Bounded_terminal_command_queue                         m_output_queue;
    Bounded_terminal_command_queue                         m_write_queue;
    std::vector<Terminal_session_command>                  m_processed_commands;
    std::vector<Terminal_session_notification>             m_notifications;
    std::vector<Terminal_session_notification>             m_pending_notifications;
    std::vector<Terminal_session_result>                   m_results;
    std::vector<Terminal_resize_transaction>               m_resize_transactions;
    std::vector<QByteArray>                                m_output_chunks;
    QByteArray                                             m_backend_output_prescan_pending;
    Terminal_utf8_scan_state                               m_backend_output_prescan_utf8_state;
    std::mutex                                             m_backend_output_capture_mutex;
    std::unique_ptr<QFile>                                 m_backend_output_capture_file;
    std::optional<Terminal_screen_model>                   m_screen_model;
    // Last color state requested via set_color_state, remembered so it can be
    // reapplied when the screen model is (re)created on the first resize.
    std::optional<Terminal_color_state>                    m_color_state;
    std::shared_ptr<const Terminal_render_snapshot>        m_latest_render_snapshot;
    std::shared_ptr<const Terminal_render_snapshot>        m_latest_content_render_snapshot;
    terminal_selection_content_basis_t                     m_latest_content_render_snapshot_content_basis;
    std::optional<Terminal_public_projection>              m_public_projection;
    Terminal_public_viewport_controller                    m_public_viewport_controller;
    std::optional<Terminal_screen_model_result>            m_last_model_ingest_result;
    std::optional<Terminal_screen_model_result>            m_render_snapshot_model_result;
    std::optional<Deferred_backend_content_snapshot>       m_deferred_backend_content_snapshot;
    std::optional<Terminal_backend_exit>                   m_exit_status;
    Ime_preedit_state                                      m_ime_preedit;
    Terminal_process_state                     m_process_state =
        Terminal_process_state::NOT_STARTED;
    terminal_grid_size_t                                   m_grid_size;
    std::uint64_t                                          m_next_sequence = 1U;
    std::uint64_t                                          m_next_resize_id = 1U;
    std::uint64_t                                          m_last_processed_sequence = 0U;
    std::uint64_t                                          m_backend_callback_publication_epoch = 0U;
    std::uint64_t                                          m_last_processed_backend_callback_epoch = 0U;
    std::uint64_t                                          m_budgeted_backend_output_sequence = 0U;
    std::uint64_t                                          m_render_snapshot_generation = 0U;
    std::uint64_t                                          m_render_snapshot_synced_generation = 0U;
    std::uint64_t                                          m_next_public_projection_generation = 1U;
    std::optional<Terminal_synchronized_output_scroll_policy>
                                                           m_synchronized_output_hold_policy;
    Terminal_synchronized_output_policy_change_event       m_synchronized_output_policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
    std::uint64_t                                          m_ime_preedit_generation = 0U;
    std::uint64_t                                          m_alternate_scroll_mode_generation = 0U;
    std::uint64_t                                          m_active_buffer_epoch = 0U;
    std::uint64_t                                          m_row_origin_generation = 0U;
    int                                                    m_deferred_synchronized_evicted_scrollback_rows = 0;
    std::uint64_t                                          m_selection_session_epoch = 0U;
    bool                                                   m_backend_ready = false;
    bool                                                   m_backend_geometry_in_sync = false;
    bool                                                   m_output_backpressure_active = false;
    bool                                                   m_processing_commands = false;
    bool                                                   m_backend_content_snapshot_deferral_active = false;
    bool                                                   m_backend_error_queued_during_command = false;
    bool                                                   m_stop_requested = false;
    std::uint64_t                                          m_stop_requested_sequence = 0U;
    std::uint64_t                                          m_result_capture_sequence = 0U;
    std::optional<Terminal_session_result>                 m_captured_result;
    Terminal_viewport_controller                           m_viewport_controller;
    Terminal_bell_state                                    m_bell_state;
    Selection_contract_controller                          m_selection;
    terminal_selection_content_basis_t                     m_selection_content_basis;
    Terminal_session_profile_stats                         m_profile_stats;
    Terminal_buffer_id                         m_selection_buffer_id =
        Terminal_buffer_id::PRIMARY;
    bool                                                   m_deferred_viewport_changed = false;
    bool                                                   m_visual_bell_active        = false;
};

}
