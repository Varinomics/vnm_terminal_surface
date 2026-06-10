#pragma once

#include <QQuickItem>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <chrono>
#include <memory>

class QQuickWindow;
class QScreen;
class QHoverEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QInputMethodEvent;

namespace vnm_terminal::internal {
class Terminal_backend;
struct Terminal_backend_error;
struct Terminal_viewport_state;
struct Terminal_session_notification;
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
    Q_PROPERTY(Mouse_reporting_policy mouseReportingPolicy
        READ mouse_reporting_policy WRITE set_mouse_reporting_policy
        NOTIFY mouse_reporting_policy_changed)
    Q_PROPERTY(Copy_shortcut_policy copyShortcutPolicy
        READ copy_shortcut_policy WRITE set_copy_shortcut_policy
        NOTIFY copy_shortcut_policy_changed)
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
    // empty string, matching the unset QString these fields previously held, so
    // transcript and wheel-trace output is byte-for-byte unchanged. Hosts should
    // branch on the enum values, not on these strings.
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

    bool primary_repaint_recovery_enabled() const;
    void set_primary_repaint_recovery_enabled(bool enabled);

    QString backend_output_capture_path() const;
    void set_backend_output_capture_path(const QString& path);

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

    Mouse_reporting_policy mouse_reporting_policy() const;
    void set_mouse_reporting_policy(Mouse_reporting_policy policy);

    Copy_shortcut_policy copy_shortcut_policy() const;
    void set_copy_shortcut_policy(Copy_shortcut_policy policy);

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

    // Cumulative renderer/atlas frame counts the diagnostics metrics builder
    // pairs with host-side timing to derive frame rate. These mirror the
    // counters in vnm_terminal::diagnostics::append_renderer_metrics_json and
    // append_atlas_metrics_json so a host can read the counts without including
    // surface internal headers.
    quint64 paint_completed_frame_count() const;
    quint64 qsg_atlas_render_frame_count() const;

    void set_selection_trace_enabled(bool enabled);
    void set_dirty_row_stats_enabled(bool enabled);

    Q_INVOKABLE bool respond_clipboard_write(
        quint64                        request_id,
        Clipboard_response_decision    decision);

    Q_INVOKABLE QString selected_text();
    Q_INVOKABLE void    clear_selection();
    Q_INVOKABLE bool    paste_text(QString text);
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
    Q_INVOKABLE bool start_process(QStringList argv, QString working_directory = {});
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
    void primary_repaint_recovery_enabled_changed();
    void synchronized_output_stale_timeout_ms_changed();
    void synchronized_output_scroll_policy_changed();
    void mouse_reporting_policy_changed();
    void copy_shortcut_policy_changed();
    void wheel_event_policy_changed();
    void alternate_screen_wheel_policy_changed();
    void bracketed_paste_policy_changed();
    void audible_bell_policy_changed();
    void visual_bell_policy_changed();
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

private:
    friend class vnm_terminal::internal::VNM_TerminalSurface_render_bridge;

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void releaseResources() override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

    void refresh_grid_metrics();

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

    bool start_process_with_backend(
        std::unique_ptr<vnm_terminal::internal::Terminal_backend>
                               backend,
        QStringList            argv,
        QString                working_directory);

    void drain_backend_callback_events();
    void drain_backend_callback_events(bool budgeted);
    void drain_backend_callback_events_for_posted_work();
    void queue_backend_callback_drain();
    void refresh_active_session_geometry();
    void sync_from_session();
    void sync_synchronized_output_recovery_timer();
    void handle_synchronized_output_recovery_timeout();
    void handle_synchronized_output_recovery_timeout(
        std::chrono::steady_clock::duration     budget);

    void replay_session_notification(
        const vnm_terminal::internal::Terminal_session_notification&
                               notification);

    void report_backend_error(
        vnm_terminal::internal::Terminal_backend_error
                               error,
        quint64                sequence = 0);

    void report_result_failure(
        const vnm_terminal::internal::Terminal_session_result&
                               result);

    void reset_session();
    bool copy_selected_text_to_clipboard();
    void set_selection_state(Selection_state state);

    QString                  m_font_family;
    qreal                    m_font_size                            = 13.0;
    QString                  m_color_scheme                         = QStringLiteral("Campbell");
    Cursor_style             m_cursor_style                         = Cursor_style::BLOCK;
    bool                     m_cursor_blink_enabled                 = true;
    int                      m_scrollback_limit                     = 10000;
#if defined(Q_OS_WIN)
    bool                     m_primary_repaint_recovery_enabled     = true;
#else
    bool                     m_primary_repaint_recovery_enabled     = false;
#endif
    QString                  m_backend_output_capture_path;
    QString                  m_transcript_capture_path;
    bool                     m_transcript_snapshot_diagnostics      = false;
    bool                     m_transcript_timing_diagnostics        = false;
    bool                     m_wheel_trace_enabled                  = false;
    bool                     m_selection_trace_enabled              = false;
    int                      m_synchronized_output_stale_timeout_ms = 1000;
    Synchronized_output_scroll_policy m_synchronized_output_scroll_policy =
        Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    Mouse_reporting_policy   m_mouse_reporting_policy =
        Mouse_reporting_policy::APPLICATION_CONTROLLED;
    Copy_shortcut_policy     m_copy_shortcut_policy =
        Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT;
    Wheel_event_policy       m_wheel_event_policy =
        Wheel_event_policy::APPLICATION_CONTROLLED;
    Alternate_screen_wheel_policy m_alternate_screen_wheel_policy =
        Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    Bracketed_paste_policy   m_bracketed_paste_policy =
        Bracketed_paste_policy::APPLICATION_CONTROLLED;
    Bell_policy              m_audible_bell_policy       = Bell_policy::ENABLED;
    Bell_policy              m_visual_bell_policy        = Bell_policy::ENABLED;
    Text_renderer_mode       m_text_renderer_mode        = Text_renderer_mode::AUTO;
    Lcd_subpixel_order       m_lcd_subpixel_order        = Lcd_subpixel_order::AUTO;
    bool                     m_msdf_text_available       = true;
    bool                     m_msdf_text_checking        = false;
    unsigned long long       m_msdf_availability_generation = 0;
    void start_msdf_availability_check();
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

    struct Private;
    std::unique_ptr<Private> m_private;
};
