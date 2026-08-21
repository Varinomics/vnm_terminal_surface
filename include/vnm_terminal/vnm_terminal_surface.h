#pragma once

#include "vnm_terminal/backend_output_capture.h"
#include "vnm_terminal/terminal_message_submission.h"

#include <QQuickItem>
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace vnm_terminal {

struct Terminal_environment_entry
{
    QString name;
    QString value;
};

struct Terminal_process_start_request
{
    QStringList argv;
    QString working_directory;
    std::vector<Terminal_environment_entry> base_environment;
    std::optional<std::vector<Terminal_environment_entry>>
        capability_environment;
};

enum class Terminal_process_start_determinacy
{
    DETERMINATE,
    INDETERMINATE,
};

struct Terminal_process_start_result
{
    bool accepted = false;
    bool native_dispatch_occurred = false;
    Terminal_process_start_determinacy determinacy =
        Terminal_process_start_determinacy::DETERMINATE;
};

} // namespace vnm_terminal

class QQuickWindow;
class QScreen;
class QHoverEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QInputMethodEvent;
class QEvent;

namespace vnm_terminal::internal {
class Terminal_backend;
class Terminal_session;
enum class Backend_callback_drain_stop : std::uint8_t;
struct Terminal_backend_error;
struct Terminal_launch_config;
struct Terminal_viewport_state;
struct Terminal_session_notification;
struct Terminal_text_area_resize_arbitration_event;
struct Terminal_session_result;
class VNM_TerminalSurface_render_bridge;
}

class VNM_TerminalSurface : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString fontFamily
        READ font_family WRITE set_font_family NOTIFY font_family_changed)
    Q_PROPERTY(qreal fontSize
        READ font_size WRITE set_font_size NOTIFY font_size_changed)
    Q_PROPERTY(QString colorScheme
        READ color_scheme WRITE set_color_scheme NOTIFY color_scheme_changed)
    Q_PROPERTY(Cursor_style cursorStyle
        READ cursor_style WRITE set_cursor_style NOTIFY cursor_style_changed)
    Q_PROPERTY(bool cursorBlinkEnabled
        READ cursor_blink_enabled WRITE set_cursor_blink_enabled
        NOTIFY cursor_blink_enabled_changed)
    Q_PROPERTY(int scrollbackLimit
        READ scrollback_limit WRITE set_scrollback_limit NOTIFY scrollback_limit_changed)
    Q_PROPERTY(bool interactionDiagnosticsEnabled
        READ interaction_diagnostics_enabled WRITE set_interaction_diagnostics_enabled
        NOTIFY interaction_diagnostics_enabled_changed)
    Q_PROPERTY(QString interactionDiagnosticsPath
        READ interaction_diagnostics_path CONSTANT)
    Q_PROPERTY(QString interactionDiagnosticsError
        READ interaction_diagnostics_error NOTIFY interaction_diagnostics_error_changed)
    Q_PROPERTY(bool primaryRepaintRecoveryEnabled
        READ primary_repaint_recovery_enabled WRITE set_primary_repaint_recovery_enabled
        NOTIFY primary_repaint_recovery_enabled_changed)
    Q_PROPERTY(int synchronizedOutputStaleTimeoutMs
        READ synchronized_output_stale_timeout_ms
        WRITE set_synchronized_output_stale_timeout_ms
        NOTIFY synchronized_output_stale_timeout_ms_changed)
    Q_PROPERTY(Synchronized_output_scroll_policy synchronizedOutputScrollPolicy
        READ synchronized_output_scroll_policy
        WRITE set_synchronized_output_scroll_policy
        NOTIFY synchronized_output_scroll_policy_changed)
    Q_PROPERTY(Text_area_resize_policy textAreaResizePolicy
        READ text_area_resize_policy WRITE set_text_area_resize_policy
        NOTIFY text_area_resize_policy_changed)
    Q_PROPERTY(bool textAreaResizeArbitrationEnabled
        READ text_area_resize_arbitration_enabled
        WRITE set_text_area_resize_arbitration_enabled
        NOTIFY text_area_resize_arbitration_enabled_changed)
    Q_PROPERTY(int textAreaResizeArbitrationTimeoutMs
        READ text_area_resize_arbitration_timeout_ms
        WRITE set_text_area_resize_arbitration_timeout_ms
        NOTIFY text_area_resize_arbitration_timeout_ms_changed)
    Q_PROPERTY(Mouse_reporting_policy mouseReportingPolicy
        READ mouse_reporting_policy WRITE set_mouse_reporting_policy
        NOTIFY mouse_reporting_policy_changed)
    Q_PROPERTY(Copy_shortcut_policy copyShortcutPolicy
        READ copy_shortcut_policy WRITE set_copy_shortcut_policy
        NOTIFY copy_shortcut_policy_changed)
    Q_PROPERTY(bool copyOnSelect
        READ copy_on_select WRITE set_copy_on_select
        NOTIFY copy_on_select_changed)
    Q_PROPERTY(Wheel_event_policy wheelEventPolicy
        READ wheel_event_policy WRITE set_wheel_event_policy
        NOTIFY wheel_event_policy_changed)
    Q_PROPERTY(Alternate_screen_wheel_policy alternateScreenWheelPolicy
        READ alternate_screen_wheel_policy WRITE set_alternate_screen_wheel_policy
        NOTIFY alternate_screen_wheel_policy_changed)
    Q_PROPERTY(Bracketed_paste_policy bracketedPastePolicy
        READ bracketed_paste_policy WRITE set_bracketed_paste_policy
        NOTIFY bracketed_paste_policy_changed)
    Q_PROPERTY(Bell_policy audibleBellPolicy
        READ audible_bell_policy WRITE set_audible_bell_policy
        NOTIFY audible_bell_policy_changed)
    Q_PROPERTY(Bell_policy visualBellPolicy
        READ visual_bell_policy WRITE set_visual_bell_policy
        NOTIFY visual_bell_policy_changed)
    Q_PROPERTY(bool rowTimestampTooltipEnabled
        READ row_timestamp_tooltip_enabled WRITE set_row_timestamp_tooltip_enabled
        NOTIFY row_timestamp_tooltip_enabled_changed)
    Q_PROPERTY(Text_renderer_mode textRendererMode
        READ text_renderer_mode WRITE set_text_renderer_mode
        NOTIFY text_renderer_mode_changed)
    Q_PROPERTY(Lcd_subpixel_order lcdSubpixelOrder
        READ lcd_subpixel_order WRITE set_lcd_subpixel_order
        NOTIFY lcd_subpixel_order_changed)
    Q_PROPERTY(bool msdfTextAvailable
        READ msdf_text_available NOTIFY msdf_text_available_changed)
    Q_PROPERTY(bool msdfTextChecking
        READ msdf_text_checking NOTIFY msdf_text_checking_changed)
    Q_PROPERTY(QString terminalTitle READ terminal_title NOTIFY terminal_title_changed)
    Q_PROPERTY(QString terminalIconName
        READ terminal_icon_name NOTIFY terminal_icon_name_changed)
    Q_PROPERTY(Process_state processState READ process_state NOTIFY process_state_changed)
    Q_PROPERTY(bool backendReady READ backend_ready NOTIFY backend_ready_changed)
    Q_PROPERTY(bool backendGeometryInSync READ backend_geometry_in_sync NOTIFY geometry_sync_changed)
    Q_PROPERTY(int rows READ rows NOTIFY grid_geometry_changed)
    Q_PROPERTY(int columns READ columns NOTIFY grid_geometry_changed)
    Q_PROPERTY(int scrollbackRows READ scrollback_rows NOTIFY viewport_changed)
    Q_PROPERTY(int viewportVisibleRows READ viewport_visible_rows NOTIFY viewport_changed)
    Q_PROPERTY(int viewportOffsetFromTail
        READ viewport_offset_from_tail NOTIFY viewport_changed)
    Q_PROPERTY(bool viewportAtTail READ viewport_at_tail NOTIFY viewport_changed)
    Q_PROPERTY(Selection_state selectionState READ selection_state NOTIFY selection_changed)
    Q_PROPERTY(QString searchQuery
        READ search_query WRITE set_search_query NOTIFY search_changed)
    Q_PROPERTY(Search_result_state searchResultState
        READ search_result_state NOTIFY search_changed)
    Q_PROPERTY(int searchMatchCount READ search_match_count NOTIFY search_changed)
    Q_PROPERTY(int currentSearchMatch READ current_search_match NOTIFY search_changed)

public:
    enum class Cursor_style
    {
        BLOCK,
        BAR,
        UNDERLINE,
    };
    Q_ENUM(Cursor_style)

    enum class Mouse_reporting_policy
    {
        DISABLED,
        APPLICATION_CONTROLLED,
    };
    Q_ENUM(Mouse_reporting_policy)

    enum class Copy_shortcut_policy
    {
        TERMINAL_INPUT,
        COPY_SELECTION_OR_TERMINAL_INPUT,
        COPY_SELECTION_OR_IGNORE,
    };
    Q_ENUM(Copy_shortcut_policy)

    enum class Wheel_event_policy
    {
        APPLICATION_CONTROLLED,
        LOCAL_SCROLLBACK_FIRST,
        LOCAL_SCROLLBACK_ONLY,
    };
    Q_ENUM(Wheel_event_policy)

    enum class Synchronized_output_scroll_policy
    {
        DEFER_UNTIL_CONTENT_PUBLICATION,
        IMMEDIATE_PUBLIC_PROJECTION,
    };
    Q_ENUM(Synchronized_output_scroll_policy)

    enum class Text_area_resize_policy
    {
        APPLICATION_CONTROLLED,
        DISABLED,
    };
    Q_ENUM(Text_area_resize_policy)

    enum class Text_area_resize_arbitration_decision
    {
        REJECT,
        ACCEPT,
    };
    Q_ENUM(Text_area_resize_arbitration_decision)

    enum class Text_area_resize_arbitration_outcome
    {
        ACCEPTED,
        REJECTED,
        HOLD_LIMIT_REACHED,
        TEXT_AREA_RESIZE_DISABLED,
        ARBITRATION_DISABLED,
        PROCESS_EXITED,
        TIMED_OUT,
    };
    Q_ENUM(Text_area_resize_arbitration_outcome)

    enum class Alternate_screen_wheel_policy
    {
        MOUSE_REPORTING_FIRST,
        CURSOR_KEYS,
        PAGE_KEYS,
    };
    Q_ENUM(Alternate_screen_wheel_policy)

    enum class Bracketed_paste_policy
    {
        DISABLED,
        APPLICATION_CONTROLLED,
        ENABLED,
    };
    Q_ENUM(Bracketed_paste_policy)

    enum class Bell_policy
    {
        DISABLED,
        ENABLED,
    };
    Q_ENUM(Bell_policy)

    enum class Text_renderer_mode
    {
        AUTO,
        MSDF,
        GLYPH,
    };
    Q_ENUM(Text_renderer_mode)

    enum class Lcd_subpixel_order
    {
        AUTO,
        NONE,
        RGB,
        BGR,
        VRGB,
        VBGR,
    };
    Q_ENUM(Lcd_subpixel_order)

    enum class Process_state
    {
        NOT_STARTED,
        STARTING,
        RUNNING,
        EXITED,
        FAILED,
    };
    Q_ENUM(Process_state)

    enum class Exit_reason
    {
        EXITED,
        INTERRUPTED,
        TERMINATED,
        FAILED_TO_START,
    };
    Q_ENUM(Exit_reason)

    enum class Backend_error_code
    {
        INVALID_LAUNCH_CONFIG,
        INVALID_INITIAL_GRID_SIZE,
        WORKING_DIRECTORY_UNAVAILABLE,
        START_FAILED,
        WRITE_FAILED,
        RESIZE_FAILED,
        INTERRUPT_FAILED,
        TERMINATE_FAILED,
        OUTPUT_OVERFLOW,
        CALLBACK_MISSING,
        READ_FAILED,
    };
    Q_ENUM(Backend_error_code)

    enum class Clipboard_response_decision
    {
        DENY,
        ALLOW,
    };
    Q_ENUM(Clipboard_response_decision)

    enum class Scroll_noop_cause
    {
        NONE,
        ZERO_LINE_DELTA,
        NO_SESSION,
        SYNCHRONIZED_OUTPUT_DEFERRED,
        SYNCHRONIZED_OUTPUT_PUBLISHED,
        ALTERNATE_SCREEN,
        BOUNDARY_OR_CLAMP,
        NO_PUBLICATION,
    };
    Q_ENUM(Scroll_noop_cause)

    enum class Scroll_action
    {
        NONE,
        VIEWPORT_MOVED,
        AT_BOUNDARY,
        DEFERRED_INTENT_RECORDED,
        TERMINAL_INPUT,
    };
    Q_ENUM(Scroll_action)

    // Diagnostic schema strings for the scroll-diagnostic enums. NONE maps to an
    // empty string. Hosts should branch on the enum values, not on these strings.
    static QString scroll_noop_cause_name(Scroll_noop_cause cause);
    static QString scroll_action_name(Scroll_action action);

    struct wheel_scroll_diagnostic_result_t
    {
        Scroll_noop_cause no_op_cause                   = Scroll_noop_cause::NONE;
        Scroll_action     scroll_action                 = Scroll_action::NONE;
        qint64            backend_drain_elapsed_ns      = 0;
        int               backend_drain_calls           = 0;
        int               applied_line_delta            = 0;
        bool              session_present               = false;
        bool              render_publication_blocked    = false;
        bool              published_synchronized_output = false;
        bool              alternate_screen              = false;
        bool              local_scroll_intent_recorded  = false;
        bool              local_scroll_applied          = false;
        bool              visible_scroll_applied        = false;
        bool              deferred_intent_recorded      = false;
        bool              event_accepted                = false;
    };

    enum class Selection_state
    {
        NONE,
        ACTIVE,
    };
    Q_ENUM(Selection_state)

    enum class Search_result_state
    {
        INACTIVE,
        SOURCE_UNAVAILABLE,
        NO_MATCH,
        MATCH,
    };
    Q_ENUM(Search_result_state)

    explicit VNM_TerminalSurface(QQuickItem* parent = nullptr);
    ~VNM_TerminalSurface() override;

    QString font_family() const;
    void set_font_family(const QString& font_family);

    qreal font_size() const;
    void set_font_size(qreal font_size);

    QString color_scheme() const;
    void set_color_scheme(const QString& color_scheme);

    Q_INVOKABLE QStringList available_color_schemes() const;
    Q_INVOKABLE QVariantMap color_scheme_preview(const QString& color_scheme) const;

    Cursor_style cursor_style() const;
    void set_cursor_style(Cursor_style cursor_style);

    bool cursor_blink_enabled() const;
    void set_cursor_blink_enabled(bool enabled);

    int scrollback_limit() const;
    void set_scrollback_limit(int limit);

    // Interaction diagnostics use one bounded process-global trace writer.
    // Only one surface may own it at a time. Printable text is redacted, but
    // control-key identity and event timing remain diagnostic data.
    bool interaction_diagnostics_enabled() const;
    void set_interaction_diagnostics_enabled(bool enabled);
    QString interaction_diagnostics_path() const;
    QString interaction_diagnostics_error() const;
    void record_interaction_diagnostic(
        const char*    category,
        const char*    event,
        const QString& details = {},
        std::uint64_t  correlation_id = 0U) const;
    void record_key_interaction_diagnostic(
        const char*      category,
        const char*      event,
        const QKeyEvent& key_event,
        std::uint64_t    correlation_id = 0U) const;

    static std::size_t default_retained_history_capacity_bytes();
    static std::size_t minimum_retained_history_capacity_bytes();
    static std::size_t maximum_retained_history_capacity_bytes();
    std::size_t retained_history_capacity_bytes() const;
    void set_retained_history_capacity_bytes(std::size_t capacity_bytes);

    bool primary_repaint_recovery_enabled() const;
    void set_primary_repaint_recovery_enabled(bool enabled);

    std::optional<vnm_terminal::Backend_output_capture_config>
        backend_output_capture_config() const;
    void set_backend_output_capture_config(
        std::optional<vnm_terminal::Backend_output_capture_config> config);

    QString transcript_capture_path() const;
    void set_transcript_capture_path(const QString& path);

    bool transcript_snapshot_diagnostics() const;
    void set_transcript_snapshot_diagnostics(bool enabled);

    bool transcript_timing_diagnostics() const;
    void set_transcript_timing_diagnostics(bool enabled);

    bool wheel_trace_enabled() const;
    void set_wheel_trace_enabled(bool enabled);
    void record_wheel_trace_event(
        const QString& source,
        const QWheelEvent& event,
        const QString& route,
        const QString& outcome,
        bool           accepted,
        int            wheel_steps = 0,
        int            effective_line_delta = 0,
        qreal          angle_remainder = 0.0,
        qreal          pixel_remainder = 0.0,
        int            backend_drain_calls = 0,
        qint64         backend_drain_elapsed_ns = 0,
        bool           local_scroll_intent_recorded = false,
        const QString& local_scroll_block_reason = {},
        const QString& scroll_action = {},
        int            applied_line_delta = 0,
        bool           deferred_intent_recorded = false);

    int synchronized_output_stale_timeout_ms() const;
    void set_synchronized_output_stale_timeout_ms(int timeout_ms);

    Synchronized_output_scroll_policy synchronized_output_scroll_policy() const;
    void set_synchronized_output_scroll_policy(
        Synchronized_output_scroll_policy policy);

    /**
     * Whether XTWINOPS `CSI 8 ; rows ; columns t` may move the text area.
     *
     * The grid is applied at the parser sequence point, so following output in
     * the same chunk is interpreted against the new text area. The decision
     * cannot be deferred to a `text_area_resize_requested()` handler, which is
     * why this is declared up front rather than answered per request.
     *
     * A host sets DISABLED whenever its window geometry belongs to the window
     * manager rather than to it (maximized, fullscreen, minimized). Requests
     * are then ignored: no grid change, no pty resize, and no
     * `text_area_resize_requested()` signal. The default,
     * APPLICATION_CONTROLLED, honors the request and signals the host.
     */
    Text_area_resize_policy text_area_resize_policy() const;
    void set_text_area_resize_policy(Text_area_resize_policy policy);

    /**
     * Whether XTWINOPS `CSI 8 ; rows ; columns t` is a two-phase transaction.
     *
     * Enabled, a captured sequence no longer commits anything on its own.
     * Backend output stops at the sequence point, the host receives
     * `text_area_resize_arbitration_requested(request_id, rows, columns)`,
     * resizes its window, and answers with `respond_text_area_resize` carrying
     * the grid it actually got. The host's own window resize is not the answer,
     * but it is an ordinary geometry change: it reaches the grid and the pty
     * exactly as it would with no request in flight. The answer decides only
     * whether the request was granted, committing the answered grid and
     * interpreting the held output against it, so an acceptance adds a reflow
     * and a pty resize only where its grid differs from the one the host's own
     * resize already reached, and a refusal adds neither. Output after the
     * sequence is held until the host answers, bounded by the session's hold
     * limit and by `textAreaResizeArbitrationTimeoutMs`.
     *
     * If the request itself exceeds the bounded control-sequence allowance, or
     * its own callback already has more trailing bytes than the hold limit, the
     * request is declined without being presented and processing continues
     * normally.
     *
     * At most one request is actionable at a time. If the process exits before
     * a queued request reaches the host, neither that stale request nor a
     * matching historical settlement is emitted.
     *
     * A captured request emits no `text_area_resize_requested()`, because an
     * arbitrating host has already moved its window by the time the request
     * settles. One shape is not captured: a `CSI 8 t` carrying an embedded C0
     * control byte and split across a backend read boundary that falls after
     * the control. The parser applies such a control as it scans and carries
     * only the stripped prefix across the boundary, so the two halves are no
     * longer joinable in the byte stream the transaction watches. That request
     * commits at the sequence point and emits `text_area_resize_requested()`,
     * exactly as it does with arbitration disabled. An arbitrating host
     * therefore connects both signals; leaving `text_area_resize_requested()`
     * unconnected moves the grid and the pty with nothing telling the host to
     * follow.
     *
     * Disabled, the default, the sequence keeps its sequence-point behavior and
     * `textAreaResizePolicy` remains the only host control over it.
     */
    bool text_area_resize_arbitration_enabled() const;
    void set_text_area_resize_arbitration_enabled(bool enabled);

    int  text_area_resize_arbitration_timeout_ms() const;
    void set_text_area_resize_arbitration_timeout_ms(int timeout_ms);
    Mouse_reporting_policy mouse_reporting_policy() const;
    void set_mouse_reporting_policy(Mouse_reporting_policy policy);

    Copy_shortcut_policy copy_shortcut_policy() const;
    void set_copy_shortcut_policy(Copy_shortcut_policy policy);

    bool copy_on_select() const;
    void set_copy_on_select(bool enabled);

    Wheel_event_policy wheel_event_policy() const;
    void set_wheel_event_policy(Wheel_event_policy policy);

    Alternate_screen_wheel_policy alternate_screen_wheel_policy() const;
    void set_alternate_screen_wheel_policy(Alternate_screen_wheel_policy policy);

    Bracketed_paste_policy bracketed_paste_policy() const;
    void set_bracketed_paste_policy(Bracketed_paste_policy policy);

    Bell_policy audible_bell_policy() const;
    void set_audible_bell_policy(Bell_policy policy);

    Bell_policy visual_bell_policy() const;
    void set_visual_bell_policy(Bell_policy policy);

    bool row_timestamp_tooltip_enabled() const;
    void set_row_timestamp_tooltip_enabled(bool enabled);

    Text_renderer_mode text_renderer_mode() const;
    void set_text_renderer_mode(Text_renderer_mode mode);

    bool msdf_text_available() const;
    bool msdf_text_checking() const;

    Lcd_subpixel_order lcd_subpixel_order() const;
    void set_lcd_subpixel_order(Lcd_subpixel_order order);

    QString terminal_title() const;
    QString terminal_icon_name() const;
    Process_state process_state() const;
    bool backend_ready() const;
    bool backend_geometry_in_sync() const;
    int rows() const;
    int columns() const;
    int scrollback_rows() const;
    int viewport_visible_rows() const;
    int viewport_offset_from_tail() const;
    bool viewport_at_tail() const;
    Selection_state selection_state() const;
    QString search_query() const;
    Search_result_state search_result_state() const;
    int search_match_count() const;
    int current_search_match() const;

    // Cumulative frame counts for host-side timing and frame evidence. The
    // atlas counter is the canonical renderer frame count; the paint-completed
    // counter reflects GUI/render completion bookkeeping without requiring
    // internal headers.
    quint64 paint_completed_frame_count() const;
    quint64 qsg_atlas_render_frame_count() const;

    void set_selection_trace_enabled(bool enabled);
    void set_dirty_row_stats_enabled(bool enabled);
    void set_clipboard_text_reader(std::function<std::optional<QString>()> reader);

    Q_INVOKABLE bool respond_clipboard_write(
        quint64                        request_id,
        Clipboard_response_decision    decision);

    /**
     * Answers the in-flight text-area resize arbitration.
     *
     * ACCEPT commits the grid the host actually got, which may differ from the
     * requested one when the window system clamped it. That grid is all the
     * answer decides: a C0 control byte the child embedded in the captured
     * sequence, which the parser applies ahead of the resize, takes effect
     * whether or not the answer matched the request. REJECT leaves the grid
     * untouched and ignores the grid arguments. Either way the held output
     * resumes. Returns false, with a CALLBACK_MISSING `backend_error`, when the
     * request id does not match the one in flight. An ACCEPT carrying a grid
     * the terminal cannot support ends the request as a refusal instead and
     * returns false with a RESIZE_FAILED `backend_error`; the held output
     * resumes against the unchanged grid rather than waiting out the timeout.
     *
     * Resizing the window first is expected and does not settle the request:
     * the item geometry reaches the grid the way it always does, so `rows()`
     * and `columns()` report the grid the host actually got and the answer can
     * be truthful. The request stays in flight until this call, its timeout, or
     * the session ending it.
     */
    Q_INVOKABLE bool respond_text_area_resize(
        quint64                                 request_id,
        Text_area_resize_arbitration_decision   decision,
        int                                     effective_rows,
        int                                     effective_columns);

    /**
     * Returns the OSC 8 target published at the item-coordinate point.
     *
     * An empty result means the current render snapshot has no explicit
     * hyperlink at the point. The bytes are terminal-provided, untrusted data;
     * hosts must validate them before dispatching outside the terminal.
     */
    Q_INVOKABLE QByteArray explicit_hyperlink_at(qreal x, qreal y) const;
    Q_INVOKABLE QString selected_text();
    Q_INVOKABLE void    clear_selection();
    Q_INVOKABLE void    set_search_query(QString query);
    Q_INVOKABLE void    clear_search();
    Q_INVOKABLE bool    search_next();
    Q_INVOKABLE bool    search_previous();
    Q_INVOKABLE bool    paste_text(QString text);
    vnm_terminal::Terminal_message_submission_result submit_utf8_message(
        QByteArray message_utf8);
    Q_INVOKABLE bool    paste_clipboard_text();
    /**
     * Re-derives the terminal grid from the current item geometry, resizing the
     * model and the backend when the two have diverged. It issues no resize when
     * the grid already matches the item, though it still re-resolves render
     * state, so it is not free.
     *
     * Hosts that honor `text_area_resize_requested()` need this when resizing
     * the shell did not land on the requested geometry, for example when the
     * window manager clamps the request: the model is on the requested grid,
     * the item is not, and no geometry change arrives to reconcile them. Hosts
     * that never want the text area moved should set
     * `textAreaResizePolicy` to DISABLED instead, which stops the request being
     * applied at all.
     *
     * This resizes synchronously, so calling it from a
     * `text_area_resize_requested()` handler nests inside session notification
     * delivery and can reorder later host-visible signals from that same
     * delivery. Deferring it to the next event loop turn avoids that.
     */
    Q_INVOKABLE void    refresh_grid_from_item_geometry();
    // Scrolls only when the published public viewport is primary-screen
    // scrollback and can be updated immediately. Under the default synchronized
    // output policy, hidden synchronized output remains deferred and returns
    // false. Under IMMEDIATE_PUBLIC_PROJECTION, a valid hold publishes a public
    // projection scroll snapshot, while an invalidated hold accepts a deferred
    // release intent without consulting hidden live bounds.
    Q_INVOKABLE bool scroll_viewport_lines(int line_delta);
    wheel_scroll_diagnostic_result_t scroll_viewport_lines_with_diagnostics(
        int                    line_delta);
    wheel_scroll_diagnostic_result_t scroll_viewport_lines_with_diagnostics(
        int                    line_delta,
        QString                source);
    Q_INVOKABLE bool scroll_to_offset_from_tail(int offset_from_tail);
    wheel_scroll_diagnostic_result_t scroll_to_offset_from_tail_with_diagnostics(
        int                    offset_from_tail);
    wheel_scroll_diagnostic_result_t scroll_to_offset_from_tail_with_diagnostics(
        int                    offset_from_tail,
        QString                source);
    bool scroll_to_offset_from_tail_from_source(
        int                    offset_from_tail,
        QString                source);
    /**
     * Starts one terminal child from explicit semantic inputs.
     *
     * `base_environment` is a caller-captured, policy-sanitized snapshot. The
     * optional capability contribution remains separate until this call and
     * cannot alter terminal-owned or executable-lookup names. The surface
     * composes the final environment without ambient inheritance, revalidates
     * the absolute working directory at native admission, and performs at most
     * one native dispatch. An indeterminate dispatched result must not be
     * retried.
     */
    vnm_terminal::Terminal_process_start_result start_terminal(
        vnm_terminal::Terminal_process_start_request request);
    Q_INVOKABLE bool interrupt_process();
    Q_INVOKABLE bool terminate_process();

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void font_family_changed();

    void font_size_changed();
    void color_scheme_changed();
    void cursor_style_changed();
    void cursor_blink_enabled_changed();
    void scrollback_limit_changed();
    void interaction_diagnostics_enabled_changed();
    void interaction_diagnostics_error_changed();
    void primary_repaint_recovery_enabled_changed();
    void synchronized_output_stale_timeout_ms_changed();
    void synchronized_output_scroll_policy_changed();
    void text_area_resize_policy_changed();
    void text_area_resize_arbitration_requested(quint64 request_id, int rows, int columns);
    // Reports every ending of an arbitration that was presented to the host,
    // the host's own answer and the settlements the terminal makes alike. A
    // request withdrawn before presentation emits neither signal. rows and
    // columns carry the grid the held output replays against: the grid the
    // answer committed on ACCEPTED, and the grid current at settlement on every
    // other outcome.
    void text_area_resize_arbitration_settled(
        quint64                                 request_id,
        Text_area_resize_arbitration_outcome    outcome,
        int                                     rows,
        int                                     columns);
    void text_area_resize_arbitration_enabled_changed();
    void text_area_resize_arbitration_timeout_ms_changed();
    void mouse_reporting_policy_changed();
    void copy_shortcut_policy_changed();
    void copy_on_select_changed();
    void wheel_event_policy_changed();
    void alternate_screen_wheel_policy_changed();
    void bracketed_paste_policy_changed();
    void audible_bell_policy_changed();
    void visual_bell_policy_changed();
    void row_timestamp_tooltip_enabled_changed();
    void text_renderer_mode_changed();
    void msdf_text_available_changed();
    void msdf_text_checking_changed();
    void lcd_subpixel_order_changed();
    void terminal_title_changed();
    void terminal_icon_name_changed();
    void process_state_changed();
    void backend_ready_changed();
    void grid_geometry_changed();
    void geometry_sync_changed();
    void viewport_changed();
    void selection_changed();
    void search_changed();

    void process_started();
    void process_exited(Exit_reason reason, int exit_code);
    void backend_error(Backend_error_code code, QString message);
    void output_activity();
    void output_backpressure_changed(bool active);
    void bell_requested();

    void text_area_resize_requested(
        int                    rows,
        int                    columns);

    void clipboard_write_requested(
        quint64                request_id,
        QString                target_selection,
        QByteArray             payload);

    // Emitted only for a completed Ctrl+left-click on one explicit OSC 8
    // identity in the current published snapshot. The surface never opens the
    // target; the host owns validation and external dispatch.
    void explicit_hyperlink_activation_requested(
        QByteArray             target);

    // Hover-idle row timestamp tooltip contract: requested fires after the
    // pointer rests over a stamped row, dismissed fires once per shown tooltip
    // on the first subsequent pointer activity, viewport scroll, or disable.
    // The timestamp is the wall-clock time the row's content last changed,
    // not when the line first appeared; never-written rows request nothing.
    // x and y are the pointer position in item coordinates so the host can
    // place the tooltip.
    void row_timestamp_tooltip_requested(
        qreal                  x,
        qreal                  y,
        QDateTime              timestamp);
    void row_timestamp_tooltip_dismissed();

private:
    friend class vnm_terminal::internal::VNM_TerminalSurface_render_bridge;

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void updatePolish() override;
    void releaseResources() override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

    void refresh_grid_metrics();
    void refresh_grid_metrics_if_device_pixel_ratio_changed();

    void set_grid_size(
        int                    rows,
        int                    columns);

    void set_viewport_state(
        const vnm_terminal::internal::Terminal_viewport_state&
                               state);

    void set_process_state(
        Process_state          state);

    void set_backend_ready(
        bool                   ready);

    void set_backend_geometry_in_sync(
        bool                   in_sync);

    void bind_window_signals(
        QQuickWindow*          window);

    void bind_screen_signals(
        QScreen*               screen);

    void handle_scene_graph_invalidated(
        std::uint64_t          window_binding_generation);

    vnm_terminal::Terminal_process_start_result start_native_terminal(
        vnm_terminal::internal::Terminal_launch_config
                               launch_config);

    vnm_terminal::Terminal_process_start_result start_backend_terminal(
        std::unique_ptr<vnm_terminal::internal::Terminal_backend>
                               backend,
        vnm_terminal::internal::Terminal_launch_config
                               launch_config);

    struct backend_callback_drain_result_t
    {
        // Zero-init reads as Backend_callback_drain_stop::COMPLETE; the
        // null-session drain path returns a default-constructed result.
        vnm_terminal::internal::Backend_callback_drain_stop stop{};
        bool deliver_notifications         = true;
        bool callbacks_pending_after_drain = false;
    };

    enum class Backend_callback_incomplete_follow_up
    {
        POSTED_DRAIN,
        FRAME_UPDATE,
    };

    backend_callback_drain_result_t process_backend_callback_events_recorded(
        vnm_terminal::internal::Terminal_session*
                               session,
        std::optional<std::chrono::steady_clock::duration>
                               budget,
        bool                   use_budget_notification_boundary,
        std::optional<std::uint64_t>
                               target_backend_callback_epoch = std::nullopt,
        Backend_callback_incomplete_follow_up
                               pending_mouse_report_follow_up =
                                   Backend_callback_incomplete_follow_up::POSTED_DRAIN);
    void request_backend_callback_follow_up_after_incomplete_recorded_drain(
        vnm_terminal::internal::Terminal_session*
                               session,
        std::uint64_t          session_generation,
        vnm_terminal::internal::Backend_callback_drain_stop
                               stop,
        Backend_callback_incomplete_follow_up
                               follow_up);
    void drain_backend_callback_events();
    void drain_backend_callback_events(bool budgeted);
    void drain_backend_callback_events_for(std::chrono::steady_clock::duration budget);
    backend_callback_drain_result_t drain_backend_callback_events_until_epoch(
        std::uint64_t          target_epoch,
        std::optional<std::chrono::steady_clock::duration>
                               budget);
    void drain_backend_callback_events_with_budget(std::optional<std::chrono::steady_clock::duration> budget);
    void drain_backend_callback_events_for_posted_work();
    void queue_backend_callback_pressure_drain();
    void queue_backend_callback_drain();
    void refresh_active_session_geometry();
    void sync_from_session(
        bool                                deliver_notifications = true);
    void sync_synchronized_output_recovery_timer();
    void handle_synchronized_output_recovery_timeout();
    void handle_synchronized_output_recovery_timeout(
        std::chrono::steady_clock::duration     budget);

    void replay_session_notification(
        const vnm_terminal::internal::Terminal_session_notification&
                               notification);
    void replay_text_area_resize_arbitration_event(
        const vnm_terminal::internal::Terminal_text_area_resize_arbitration_event&
                               event);

    void report_backend_error(
        vnm_terminal::internal::Terminal_backend_error
                               error,
        quint64                sequence = 0);

    void report_result_failure(
        const vnm_terminal::internal::Terminal_session_result&
                               result);

    void reset_session();
    enum class Empty_selection_copy_policy
    {
        COPY,
        SKIP_EMPTY_SELECTION,
    };
    bool copy_selected_text_to_clipboard(Empty_selection_copy_policy policy);
    std::optional<QString> read_clipboard_text_for_paste();
    void set_selection_state(Selection_state state);
    void set_search_state(
        QString             query,
        Search_result_state state,
        int                 match_count,
        int                 current_match);
    void set_hyperlink_hover_position(std::optional<QPointF> position);
    void refresh_hyperlink_hover_feedback();
    void dismiss_row_timestamp_tooltip();
    bool row_timestamp_tooltip_pointer_moved(const QPointF& position);
    void handle_row_timestamp_tooltip_timeout();
    void handle_text_area_resize_arbitration_timeout();

    QString                  m_font_family;
    qreal                    m_font_size                            = 13.0;
    QString                  m_color_scheme                         = QStringLiteral("Classic");
    Cursor_style             m_cursor_style                         = Cursor_style::BLOCK;
    bool                     m_cursor_blink_enabled                 = true;
    int                      m_scrollback_limit                     = 10000;
    std::size_t              m_retained_history_capacity_bytes     = 0U;
#if defined(Q_OS_WIN)
    bool                     m_primary_repaint_recovery_enabled     = true;
#else
    bool                     m_primary_repaint_recovery_enabled     = false;
#endif
    std::optional<vnm_terminal::Backend_output_capture_config>
                             m_backend_output_capture_config;
    QString                  m_transcript_capture_path;
    bool                     m_transcript_snapshot_diagnostics      = false;
    bool                     m_transcript_timing_diagnostics        = false;
    bool                     m_wheel_trace_enabled                  = false;
    bool                     m_selection_trace_enabled              = false;
    bool                     m_interaction_diagnostics_enabled      = false;
    QString                  m_interaction_diagnostics_error;
    int                      m_synchronized_output_stale_timeout_ms = 1000;
    Text_area_resize_policy m_text_area_resize_policy =
        Text_area_resize_policy::APPLICATION_CONTROLLED;
    bool                     m_text_area_resize_arbitration_enabled = false;
    // Long enough for a host to complete a window resize round trip on a loaded
    // compositor, short enough that a host that never answers does not read as a
    // frozen terminal. 0 removes the bound.
    int                      m_text_area_resize_arbitration_timeout_ms = 250;
    Synchronized_output_scroll_policy m_synchronized_output_scroll_policy =
        Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    Mouse_reporting_policy   m_mouse_reporting_policy =
        Mouse_reporting_policy::APPLICATION_CONTROLLED;
    Copy_shortcut_policy     m_copy_shortcut_policy =
        Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT;
    bool                     m_copy_on_select = false;
    Wheel_event_policy       m_wheel_event_policy =
        Wheel_event_policy::APPLICATION_CONTROLLED;
    Alternate_screen_wheel_policy m_alternate_screen_wheel_policy =
        Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    Bracketed_paste_policy   m_bracketed_paste_policy =
        Bracketed_paste_policy::APPLICATION_CONTROLLED;
    Bell_policy              m_audible_bell_policy       = Bell_policy::ENABLED;
    Bell_policy              m_visual_bell_policy        = Bell_policy::ENABLED;
    bool                     m_row_timestamp_tooltip_enabled = true;
    Text_renderer_mode       m_text_renderer_mode        = Text_renderer_mode::AUTO;
    Lcd_subpixel_order       m_lcd_subpixel_order        = Lcd_subpixel_order::AUTO;
    bool                     m_msdf_text_available       = true;
    bool                     m_msdf_text_checking        = false;
    unsigned long long       m_msdf_availability_generation = 0;
    void start_msdf_availability_check();
    void handle_msdf_availability_completion_timeout();
    void apply_msdf_availability_result(bool available, unsigned long long generation);
    QString                  m_terminal_title;
    QString                  m_terminal_icon_name;
    Process_state            m_process_state             = Process_state::NOT_STARTED;
    bool                     m_backend_ready             = false;
    bool                     m_backend_geometry_in_sync  = false;
    int                      m_rows                      = 0;
    int                      m_columns                   = 0;
    int                      m_scrollback_rows           = 0;
    int                      m_viewport_visible_rows     = 0;
    int                      m_viewport_offset_from_tail = 0;
    bool                     m_viewport_at_tail          = true;
    Selection_state          m_selection_state           = Selection_state::NONE;
    QString                  m_search_query;
    Search_result_state      m_search_result_state       = Search_result_state::INACTIVE;
    int                      m_search_match_count        = 0;
    int                      m_current_search_match      = 0;

    struct Private;
    std::unique_ptr<Private> m_private;
};
