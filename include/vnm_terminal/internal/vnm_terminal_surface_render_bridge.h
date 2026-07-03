#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class VNM_TerminalSurface;

namespace vnm_terminal::internal {

struct Terminal_surface_render_invalidation_stats_t
{
    std::uint64_t                           update_requests                       = 0U;
    std::uint64_t                           scheduled_updates                     = 0U;
    std::uint64_t                           coalesced_requests                    = 0U;
    std::uint64_t                           consumed_updates                      = 0U;
    std::uint64_t                           render_snapshot_callback_epoch        = 0U;
    std::uint64_t                           last_rendered_snapshot_sequence       = 0U;
    std::uint64_t                           last_rendered_publication_generation  = 0U;
    bool                                    pending_update                        = false;
};

struct Terminal_surface_backend_drain_stats_t
{
    std::uint64_t                           total_drain_calls                    = 0U;
    std::uint64_t                           budgeted_drain_calls                 = 0U;
    std::uint64_t                           unbudgeted_drain_calls               = 0U;
    std::uint64_t                           posted_drain_calls                   = 0U;
    std::uint64_t                           posted_full_budget_calls             = 0U;
    std::uint64_t                           posted_frame_pending_small_budget_calls = 0U;
    std::uint64_t                           budget_exhausted_incomplete          = 0U;
    std::uint64_t                           cursor_stable_incomplete             = 0U;
    std::uint64_t                           total_elapsed_ns                     = 0U;
    std::uint64_t                           max_elapsed_ns                       = 0U;
    std::uint64_t                           session_processing_calls             = 0U;
    std::uint64_t                           session_processing_elapsed_ns        = 0U;
    std::uint64_t                           session_processing_max_elapsed_ns    = 0U;
    std::uint64_t                           sync_from_session_calls              = 0U;
    std::uint64_t                           sync_from_session_elapsed_ns         = 0U;
    std::uint64_t                           sync_from_session_max_elapsed_ns     = 0U;
    std::uint64_t                           frame_work_pending_drain_calls       = 0U;
    std::uint64_t                           frame_work_pending_elapsed_ns        = 0U;
    std::uint64_t                           render_update_pending_drain_calls    = 0U;
    std::uint64_t                           atlas_completion_pending_drain_calls = 0U;
    std::uint64_t                           requeue_count                        = 0U;
    std::uint64_t                           pending_callback_after_drain         = 0U;
    std::uint64_t                           output_backpressure_after_drain      = 0U;
};

struct Render_profile_snapshot_t
{
    std::uint64_t                           sequence                        = 0U;
    Profile_node_snapshot                   root;
    Profile_timeline_snapshot               timeline;
#if VNM_TERMINAL_PROFILING_ENABLED
    terminal_text_layout_slow_diagnostics_t slow_text_layouts;
#endif
};

struct Cursor_withhold_content_id
{
    std::uint64_t                           session_generation = 0U;
    std::uint64_t                           content_generation = 0U;
    std::uint64_t                           backend_progress_generation = 0U;
};

inline bool operator==(
    const Cursor_withhold_content_id& left,
    const Cursor_withhold_content_id& right)
{
    return
        left.session_generation == right.session_generation &&
        left.content_generation == right.content_generation &&
        left.backend_progress_generation == right.backend_progress_generation;
}

inline bool operator!=(
    const Cursor_withhold_content_id& left,
    const Cursor_withhold_content_id& right)
{
    return !(left == right);
}

enum class Cursor_withhold_test_event_kind
{
    UNSETTLED_CONTENT_PUBLISHED,
    ACCEPTED_TERMINAL_INPUT,
    SETTLED_CONTENT_PUBLISHED,
    INSTALLED_CONTENT_SNAPSHOT_SETTLED,
    NO_PUBLICATION_NO_SETTLEMENT,
    HELD_NO_PUBLICATION,
    METADATA_ONLY_PUBLICATION,
    SUCCESSFUL_RECOVERY_RELEASE,
    SESSION_RESET,
    STALE_SESSION_EVENT,
};

struct Cursor_withhold_test_event
{
    Cursor_withhold_test_event_kind         kind =
        Cursor_withhold_test_event_kind::NO_PUBLICATION_NO_SETTLEMENT;
    std::optional<Cursor_withhold_content_id>
                                             content_id;
    std::uint64_t                           session_generation = 0U;
};

struct Cursor_withhold_state_snapshot
{
    std::uint64_t                           session_generation = 0U;
    std::optional<Cursor_withhold_content_id>
                                             protected_content_id;
    bool                                    input_exemption = false;
    bool                                    cursor_withheld = false;
};

class VNM_TerminalSurface_render_bridge
{
public:
    // Internal test/render handoff only. Call on the surface GUI thread.
    static void set_render_snapshot(
        VNM_TerminalSurface&       surface,
        std::shared_ptr<const Terminal_render_snapshot>
                                   snapshot);

    static void set_render_profiler(
        VNM_TerminalSurface&                   surface,
        std::shared_ptr<Hierarchical_profiler> profiler);

    static Render_profile_snapshot_t render_profiler_snapshot(
        const VNM_TerminalSurface& surface);

    static void set_dirty_row_stats_enabled(
        VNM_TerminalSurface&       surface,
        bool                       enabled);

    static void set_selection_trace_enabled(
        VNM_TerminalSurface&       surface,
        bool                       enabled);

    static Qsg_atlas_frame_report qsg_atlas_frame(
        const VNM_TerminalSurface& surface);

    static Qsg_atlas_frame_report capture_qsg_atlas_frame_for_testing(
        const VNM_TerminalSurface& surface);

    static Terminal_screen_model_dirty_row_stats dirty_row_stats(
        const VNM_TerminalSurface& surface);

    static Terminal_screen_model_dirty_row_timeline dirty_row_timeline(
        const VNM_TerminalSurface& surface);

    static Terminal_screen_model_profile_stats model_profile_stats(
        const VNM_TerminalSurface& surface);

    static Terminal_session_profile_stats session_profile_stats(
        const VNM_TerminalSurface& surface);

    static void set_session_profile_stats_enabled_for_benchmark(
        VNM_TerminalSurface&       surface,
        bool                       enabled);

    static void set_cursor_blink_visible(
        VNM_TerminalSurface&       surface,
        bool                       visible);

    static void set_ime_preedit_state(
        VNM_TerminalSurface&       surface,
        Ime_preedit_state          state);

    static bool start_process_with_backend(
        VNM_TerminalSurface&                   surface,
        std::unique_ptr<Terminal_backend>      backend,
        QStringList                            argv,
        QString                                working_directory = {});

    static bool backend_callback_drain_queued(
        const VNM_TerminalSurface& surface);

    static std::size_t pending_backend_callback_count(
        const VNM_TerminalSurface& surface);

    static std::uint64_t backend_callback_enqueue_epoch(
        const VNM_TerminalSurface& surface);

    static std::uint64_t backend_callback_processed_epoch(
        const VNM_TerminalSurface& surface);

    static void drain_backend_callback_events(
        VNM_TerminalSurface&       surface);

    // Test-only path for stale-session drains that must deliver notifications
    // even when the budgeted stop is UNSETTLED.
    static Backend_callback_drain_stop drain_backend_callback_events_with_budget_for_testing(
        VNM_TerminalSurface&                    surface,
        std::chrono::steady_clock::duration     budget);

    static void drain_backend_callback_events_for_posted_work(
        VNM_TerminalSurface&       surface);

    static void set_backend_callback_frame_catchup_budget_for_benchmark(
        VNM_TerminalSurface&                    surface,
        std::chrono::steady_clock::duration     budget);

    static std::chrono::steady_clock::duration backend_callback_frame_catchup_budget_for_testing(
        const VNM_TerminalSurface& surface);

    static void set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
        VNM_TerminalSurface&                    surface,
        std::chrono::steady_clock::duration     extension);

    static std::chrono::steady_clock::duration
        backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
            const VNM_TerminalSurface& surface);

    static void set_pending_published_mouse_report_block_count_for_testing(
        VNM_TerminalSurface&       surface,
        int                        count);

    static void simulate_update_polish(
        VNM_TerminalSurface&       surface);

    static void handle_synchronized_output_recovery_timeout(
        VNM_TerminalSurface&                    surface,
        std::chrono::steady_clock::duration     budget);

    static bool force_release_synchronized_output_without_backend_drain_for_testing(
        VNM_TerminalSurface&       surface);

    static bool detach_selection_visual_attachment_for_testing(
        VNM_TerminalSurface&       surface);

    static std::shared_ptr<const Terminal_render_snapshot> render_snapshot(
        const VNM_TerminalSurface& surface);

    // Device-pixel-snapped cell metrics the surface currently uses for grid
    // sizing, pixel-wheel accumulation, and selection/pointer hit-testing.
    // Tests must read these rather than recomputing raw QFontMetricsF values,
    // which omit the device-pixel snapping production applies.
    static terminal_cell_metrics_t cell_metrics(
        const VNM_TerminalSurface& surface);

    static Ime_preedit_state ime_preedit_state(
        const VNM_TerminalSurface& surface);

    static Terminal_surface_render_invalidation_stats_t invalidation_stats(
        const VNM_TerminalSurface& surface);

    static std::uint64_t session_rendered_render_snapshot_generation(
        const VNM_TerminalSurface& surface);

    static Terminal_surface_backend_drain_stats_t backend_drain_stats(
        const VNM_TerminalSurface& surface);

    static bool atlas_completion_pending_for_testing(
        const VNM_TerminalSurface& surface);

    static bool mark_completed_atlas_completion_pending_for_testing(
        VNM_TerminalSurface&       surface);

    static bool mark_reported_atlas_completion_pending_for_testing(
        VNM_TerminalSurface&       surface,
        std::uint64_t              reported_publication_generation,
        bool                       drew);

    static terminal_renderer_stats_t last_renderer_stats(
        const VNM_TerminalSurface& surface);

    static terminal_renderer_cumulative_stats_t cumulative_renderer_stats(
        const VNM_TerminalSurface& surface);

    static terminal_renderer_lifecycle_stats_t lifecycle_stats(
        const VNM_TerminalSurface& surface);

    static Cursor_withhold_state_snapshot cursor_withhold_state_for_testing(
        const VNM_TerminalSurface& surface);

    static void apply_cursor_withhold_event_for_testing(
        VNM_TerminalSurface&           surface,
        Cursor_withhold_test_event     event);

    // Test-only semantic bridge for recovery observations that production
    // cannot reach without either returning before recovery or installing a
    // force-release publication. This still enters through the classifier and
    // does not expose retired cursor-withhold counter authority.
    static void apply_no_publication_recovery_observation_for_testing(
        VNM_TerminalSurface&       surface,
        bool                       active_session);

    static bool cursor_withheld_for_testing(
        const VNM_TerminalSurface& surface);

    static std::optional<Cursor_withhold_content_id>
        installed_cursor_withhold_content_id_for_testing(
            const VNM_TerminalSurface& surface);

    static std::shared_ptr<Terminal_renderer_lifecycle_recorder> lifecycle_recorder(
        VNM_TerminalSurface&       surface);

    static void release_resources(
        VNM_TerminalSurface&       surface);

    static void simulate_scene_graph_invalidated(
        VNM_TerminalSurface&       surface);

    static void simulate_stale_scene_graph_invalidated(
        VNM_TerminalSurface&       surface);

    static void invalidate_public_projection_for_testing(
        VNM_TerminalSurface&                       surface,
        Terminal_public_projection_disable_reason  reason);
};

}
