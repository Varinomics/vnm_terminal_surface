#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/interaction_trace.h"
#include "vnm_terminal/internal/posix_pty_backend.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_color_scheme.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_resize_controller.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/wheel_gesture.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"

#include <vnm_qt_dispatch/vnm_qt_dispatch.h>

#include <QColor>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethodEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QProcessEnvironment>
#include <QThreadPool>
#include <QQuickWindow>
#include <QScreen>
#include <QStringDecoder>
#include <QStringList>
#include <QThread>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>
#include <QWindow>
#include <QtGlobal>
#include <qpa/qplatformscreen.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace term = vnm_terminal::internal;

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall SystemParametersInfoW(
    unsigned int action,
    unsigned int parameter,
    void*        value,
    unsigned int update_flags);
extern "C" __declspec(dllimport) long __stdcall OleFlushClipboard();
extern "C" __declspec(dllimport) int __stdcall MessageBeep(unsigned int type);
#endif

namespace {

// The trace remains diagnostic; public signal delivery drains the session's
// durable notification channel during GUI-thread sync.
constexpr std::size_t k_surface_notification_trace_limit         = 0U;
constexpr int         k_min_synchronized_output_stale_timeout_ms = 1;
constexpr int         k_row_timestamp_tooltip_delay_ms           = 1000;
constexpr int         k_msdf_availability_completion_poll_ms     = 10;
constexpr int         k_msdf_availability_completion_timeout_ms  = 30000;
constexpr std::size_t k_surface_output_queue_high_water_bytes    = 1024U * 1024U;
constexpr std::size_t k_surface_output_queue_hard_limit_bytes    = 2U * 1024U * 1024U;
constexpr std::chrono::milliseconds k_backend_callback_drain_budget{4};
// Posted drains are the primary backend-output pump; keep each wakeup bounded
// but large enough that high-volume output does not depend on pointer redelivery.
constexpr std::chrono::milliseconds k_backend_callback_posted_drain_budget{32};
constexpr std::chrono::milliseconds k_backend_callback_frame_pending_posted_drain_budget =
    k_backend_callback_drain_budget;
constexpr std::chrono::milliseconds k_backend_callback_frame_catchup_budget_default =
    k_backend_callback_drain_budget;
// Six nominal 60 Hz frame intervals absorb ordinary compositor jitter while
// bounding recovery when an accepted frame request produces no callback progress.
constexpr int k_backend_callback_frame_progress_watchdog_ms = 100;
constexpr char k_backend_callback_frame_catchup_budget_env[] =
    "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_BUDGET_MS";

QPointer<VNM_TerminalSurface>& interaction_trace_owner()
{
    static QPointer<VNM_TerminalSurface> owner;
    return owner;
}

struct Msdf_availability_completion
{
    static constexpr int k_dispatch_pending = -1;

    unsigned long long generation = 0;
    std::atomic<int>   dispatch_result{k_dispatch_pending};
};

#if defined(_WIN32)
constexpr unsigned int k_win_spi_get_font_smoothing             = 0x004AU;
constexpr unsigned int k_win_spi_get_font_smoothing_type        = 0x200AU;
constexpr unsigned int k_win_spi_get_font_smoothing_orientation = 0x2012U;
constexpr unsigned int k_win_font_smoothing_cleartype           = 0x0002U;
constexpr unsigned int k_win_font_smoothing_orientation_bgr     = 0x0000U;
constexpr unsigned int k_win_font_smoothing_orientation_rgb     = 0x0001U;
#endif

void play_platform_bell()
{
#if defined(_WIN32)
    constexpr unsigned int k_default_beep = 0U;
    (void)MessageBeep(k_default_beep);
#else
    std::fputc('\a', stderr);
    std::fflush(stderr);
#endif
}

bool flush_clipboard_after_terminal_write()
{
#if defined(_WIN32)
    const long result = OleFlushClipboard();
    if (result != 0L) {
        qWarning(
            "VNM_TerminalSurface: OleFlushClipboard failed after clipboard write; "
            "data may not outlive the process: 0x%08lx",
            static_cast<unsigned long>(result));
        return false;
    }
#endif
    return true;
}

std::uint64_t elapsed_ns_count(std::chrono::steady_clock::duration elapsed)
{
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::max(elapsed, std::chrono::steady_clock::duration::zero()));
    return static_cast<std::uint64_t>(elapsed_ns.count());
}

std::chrono::steady_clock::duration configured_backend_callback_frame_catchup_budget()
{
    if (!qEnvironmentVariableIsSet(k_backend_callback_frame_catchup_budget_env)) {
        return k_backend_callback_frame_catchup_budget_default;
    }

    bool ok = false;
    const QString budget_text =
        qEnvironmentVariable(k_backend_callback_frame_catchup_budget_env);
    const int budget_ms = budget_text.toInt(&ok);
    if (!ok || budget_ms < 0) {
        const QByteArray budget_bytes = budget_text.toLocal8Bit();
        qWarning(
            "VNM_TerminalSurface: ignoring invalid %s=%s; using %lldms",
            k_backend_callback_frame_catchup_budget_env,
            budget_bytes.constData(),
            static_cast<long long>(
                k_backend_callback_frame_catchup_budget_default.count()));
        return k_backend_callback_frame_catchup_budget_default;
    }

    return std::chrono::milliseconds{budget_ms};
}

bool set_terminal_clipboard_text(const QString& text)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        qWarning("VNM_TerminalSurface: no application clipboard is available");
        return false;
    }

    clipboard->setText(text, QClipboard::Clipboard);
    (void)flush_clipboard_after_terminal_write();
    return true;
}

bool same_property_value(qreal lhs, qreal rhs)
{
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

qreal normalized_font_pixel_size(qreal font_size)
{
    if (!std::isfinite(font_size) || font_size <= 0.0) {
        return font_size;
    }

    // The ceiling is applied while the value is still a qreal: fontSize accepts
    // any finite number a host cares to set, and rounding one that does not fit
    // an int and then converting it is undefined rather than merely large.
    const qreal bounded_font_size = std::min(
        font_size,
        static_cast<qreal>(term::k_vnm_terminal_max_font_pixel_size));
    return static_cast<qreal>(std::max(1, static_cast<int>(std::round(bounded_font_size))));
}

int vertical_wheel_direction(const QWheelEvent& event)
{
    const int delta = event.angleDelta().y() != 0
        ? event.angleDelta().y()
        : event.pixelDelta().y();

    if (delta > 0) {
        return 1;
    }

    if (delta < 0) {
        return -1;
    }

    return 0;
}

bool is_plain_copy_shortcut(const QKeyEvent& event)
{
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);

    return event.key() == Qt::Key_C && modifiers == Qt::ControlModifier;
}

bool is_unmodified_page_scroll_key(const QKeyEvent& event)
{
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);

    return modifiers == Qt::NoModifier &&
        (event.key() == Qt::Key_PageUp || event.key() == Qt::Key_PageDown);
}

bool snapshot_has_sgr_mouse_reporting(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    return
        snapshot != nullptr                &&
        snapshot->modes.sgr_mouse_encoding &&
        snapshot->modes.mouse_tracking != term::Terminal_mouse_tracking_mode::NONE;
}

bool snapshot_has_terminal_mouse_tracking(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    return
        snapshot                       != nullptr &&
        snapshot->modes.mouse_tracking != term::Terminal_mouse_tracking_mode::NONE;
}

term::Terminal_input_mouse_tracking_mode input_mouse_tracking_mode(
    term::Terminal_mouse_tracking_mode mode)
{
    switch (mode) {
        case term::Terminal_mouse_tracking_mode::NONE:
            return term::Terminal_input_mouse_tracking_mode::NONE;
        case term::Terminal_mouse_tracking_mode::BUTTON:
            return term::Terminal_input_mouse_tracking_mode::NORMAL;
        case term::Terminal_mouse_tracking_mode::DRAG:
            return term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT;
        case term::Terminal_mouse_tracking_mode::ANY:
            return term::Terminal_input_mouse_tracking_mode::ANY_EVENT;
    }

    return term::Terminal_input_mouse_tracking_mode::NONE;
}

term::Terminal_input_mode_state input_modes_from_render_snapshot(
    const term::Terminal_render_snapshot& snapshot)
{
    term::Terminal_input_mode_state modes;
    modes.application_cursor_keys = snapshot.modes.application_cursor_keys;
    modes.mouse_tracking          = input_mouse_tracking_mode(snapshot.modes.mouse_tracking);
    modes.sgr_mouse_encoding      = snapshot.modes.sgr_mouse_encoding;
    modes.bracketed_paste         = snapshot.modes.bracketed_paste;
    return modes;
}

struct Published_mouse_report_write_result
{
    std::optional<term::Terminal_session_result> result;
    std::optional<term::terminal_grid_position_t> position;
    QByteArray                                    bytes;
    bool                                          encoded = false;
};

enum class Published_mouse_report_attempt_status
{
    WRITTEN,
    CALLBACKS_PENDING,
    NOT_ENCODED,
    SESSION_INACTIVE,
    NON_WRITABLE,
};

struct Published_mouse_report_attempt
{
    Published_mouse_report_attempt_status        status =
        Published_mouse_report_attempt_status::SESSION_INACTIVE;
    std::optional<term::Terminal_session_result> result;
    std::optional<term::terminal_grid_position_t> position;
};

bool mouse_report_write_is_non_writable_noop(
    const term::Terminal_session_result& result)
{
    return
        result.code == term::Terminal_session_result_code::INVALID_STATE &&
        result.error.has_value()                                         &&
        result.error->code == term::Terminal_backend_error_code::WRITE_FAILED;
}

bool viewport_state_can_scroll_locally(
    const term::Terminal_viewport_state&   viewport,
    int                                    scroll_direction)
{
    if (viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE) {
        return false;
    }

    if (scroll_direction > 0) {
        return viewport.offset_from_tail < viewport.scrollback_rows;
    }

    if (scroll_direction < 0) {
        return viewport.offset_from_tail > 0;
    }

    return false;
}

bool color_is_light(quint32 rgba)
{
    const QColor color = QColor::fromRgba(rgba);
    // Rec. 601 luma; drives only the light/dark preedit and visual-bell overlays
    // chosen for the active scheme.
    const double luma =
        0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue();
    return luma > 127.5;
}

const term::Terminal_color_scheme& resolve_surface_color_scheme(
    const VNM_TerminalSurface& surface)
{
    const term::Terminal_color_scheme* scheme =
        term::find_color_scheme(surface.color_scheme());
    return scheme != nullptr ? *scheme : term::default_color_scheme();
}

qreal current_device_pixel_ratio(const QQuickWindow* window)
{
    if (window == nullptr) {
        return 1.0;
    }

    const qreal device_pixel_ratio = window->effectiveDevicePixelRatio();
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return device_pixel_ratio;
}

std::unique_ptr<term::Terminal_backend> make_native_backend()
{
#if defined(_WIN32)
    return term::make_windows_conpty_backend();
#elif defined(__linux__) || defined(__APPLE__)
    return term::make_posix_pty_backend();
#else
    return nullptr;
#endif
}

VNM_TerminalSurface::Process_state surface_process_state(
    term::Terminal_process_state state)
{
    switch (state) {
        case term::Terminal_process_state::NOT_STARTED: return VNM_TerminalSurface::Process_state::NOT_STARTED;
        case term::Terminal_process_state::STARTING:    return VNM_TerminalSurface::Process_state::STARTING;
        case term::Terminal_process_state::RUNNING:     return VNM_TerminalSurface::Process_state::RUNNING;
        case term::Terminal_process_state::EXITED:      return VNM_TerminalSurface::Process_state::EXITED;
        case term::Terminal_process_state::FAILED:      return VNM_TerminalSurface::Process_state::FAILED;
    }

    return VNM_TerminalSurface::Process_state::FAILED;
}

VNM_TerminalSurface::Search_result_state surface_search_result_state(
    term::Terminal_search_result_status status)
{
    using Public = VNM_TerminalSurface::Search_result_state;
    switch (status) {
        case term::Terminal_search_result_status::INACTIVE:
            return Public::INACTIVE;
        case term::Terminal_search_result_status::SOURCE_UNAVAILABLE:
            return Public::SOURCE_UNAVAILABLE;
        case term::Terminal_search_result_status::NO_MATCH:
            return Public::NO_MATCH;
        case term::Terminal_search_result_status::MATCH:
            return Public::MATCH;
    }

    return Public::SOURCE_UNAVAILABLE;
}

VNM_TerminalSurface::Exit_reason surface_exit_reason(term::Terminal_exit_reason reason)
{
    switch (reason) {
        case term::Terminal_exit_reason::EXITED:          return VNM_TerminalSurface::Exit_reason::EXITED;
        case term::Terminal_exit_reason::INTERRUPTED:     return VNM_TerminalSurface::Exit_reason::INTERRUPTED;
        case term::Terminal_exit_reason::TERMINATED:      return VNM_TerminalSurface::Exit_reason::TERMINATED;
        case term::Terminal_exit_reason::FAILED_TO_START: return VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    }

    return VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
}

VNM_TerminalSurface::Backend_error_code surface_backend_error_code(
    term::Terminal_backend_error_code code)
{
    switch (code) {
        case term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG:
            return VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG;
        case term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE:
            return VNM_TerminalSurface::Backend_error_code::INVALID_INITIAL_GRID_SIZE;
        case term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE:
            return VNM_TerminalSurface::Backend_error_code::WORKING_DIRECTORY_UNAVAILABLE;
        case term::Terminal_backend_error_code::START_FAILED:
            return VNM_TerminalSurface::Backend_error_code::START_FAILED;
        case term::Terminal_backend_error_code::WRITE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::WRITE_FAILED;
        case term::Terminal_backend_error_code::RESIZE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::RESIZE_FAILED;
        case term::Terminal_backend_error_code::INTERRUPT_FAILED:
            return VNM_TerminalSurface::Backend_error_code::INTERRUPT_FAILED;
        case term::Terminal_backend_error_code::TERMINATE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::TERMINATE_FAILED;
        case term::Terminal_backend_error_code::OUTPUT_OVERFLOW:
            return VNM_TerminalSurface::Backend_error_code::OUTPUT_OVERFLOW;
        case term::Terminal_backend_error_code::CALLBACK_MISSING:
            return VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING;
        case term::Terminal_backend_error_code::READ_FAILED:
            return VNM_TerminalSurface::Backend_error_code::READ_FAILED;
    }

    return VNM_TerminalSurface::Backend_error_code::START_FAILED;
}

bool is_live_process_state(term::Terminal_process_state state)
{
    return
        state == term::Terminal_process_state::STARTING ||
        state == term::Terminal_process_state::RUNNING;
}

bool is_accepted(term::Terminal_session_result_code code)
{
    return code == term::Terminal_session_result_code::ACCEPTED;
}

bool backend_drain_reached_notification_boundary(
    term::Backend_callback_drain_stop  stop,
    const term::Terminal_session&      session)
{
    // Complete drains and explicit synchronized-output release boundaries
    // publish coherent states, so they deliver coalesced notifications.
    return
        stop == term::Backend_callback_drain_stop::COMPLETE ||
        stop == term::Backend_callback_drain_stop::SYNCHRONIZED_OUTPUT_RELEASED ||
        !session.has_pending_backend_callback_events();
}

term::Terminal_paste_framing_policy paste_framing_policy(
    VNM_TerminalSurface::Bracketed_paste_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Bracketed_paste_policy::DISABLED:
            return term::Terminal_paste_framing_policy::DISABLED;
        case VNM_TerminalSurface::Bracketed_paste_policy::APPLICATION_CONTROLLED:
            return term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED;
        case VNM_TerminalSurface::Bracketed_paste_policy::ENABLED:
            return term::Terminal_paste_framing_policy::ENABLED;
    }

    return term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED;
}

term::Terminal_mouse_button terminal_mouse_button(Qt::MouseButton button)
{
    switch (button) {
        case Qt::LeftButton:   return term::Terminal_mouse_button::LEFT;
        case Qt::MiddleButton: return term::Terminal_mouse_button::MIDDLE;
        case Qt::RightButton:  return term::Terminal_mouse_button::RIGHT;
        default:               return term::Terminal_mouse_button::NONE;
    }
}

term::Terminal_mouse_button terminal_mouse_button(Qt::MouseButtons buttons)
{
    if ((buttons & Qt::LeftButton)   != Qt::NoButton) { return term::Terminal_mouse_button::LEFT;   }
    if ((buttons & Qt::MiddleButton) != Qt::NoButton) { return term::Terminal_mouse_button::MIDDLE; }
    if ((buttons & Qt::RightButton)  != Qt::NoButton) { return term::Terminal_mouse_button::RIGHT;  }
    return term::Terminal_mouse_button::NONE;
}

Qt::MouseButton qt_mouse_button(term::Terminal_mouse_button button)
{
    switch (button) {
        case term::Terminal_mouse_button::LEFT:   return Qt::LeftButton;
        case term::Terminal_mouse_button::MIDDLE: return Qt::MiddleButton;
        case term::Terminal_mouse_button::RIGHT:  return Qt::RightButton;
        default:                                  return Qt::NoButton;
    }
}

term::Terminal_mouse_button terminal_mouse_button(
    Qt::MouseButtons               buttons,
    term::Terminal_mouse_button    preferred_button)
{
    const Qt::MouseButton preferred_qt_button = qt_mouse_button(preferred_button);
    if (preferred_qt_button             != Qt::NoButton &&
        (buttons & preferred_qt_button) != Qt::NoButton)
    {
        return preferred_button;
    }

    return terminal_mouse_button(buttons);
}

std::optional<term::terminal_grid_position_t> grid_position_for_local_point(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_cell_metrics_t                                  metrics,
    QPointF                                                        point)
{
    if (snapshot  == nullptr                           ||
        !term::is_valid_grid_size(snapshot->grid_size) ||
        !term::is_valid_cell_metrics(metrics)          ||
        !std::isfinite(point.x())                      ||
        !std::isfinite(point.y())                      ||
        point.x() <  0.0                               ||
        point.y() <  0.0)
    {
        return std::nullopt;
    }

    // The grid bounds are tested before the conversion, not after it:
    // explicit_hyperlink_at() takes its coordinates straight from the caller,
    // and converting a finite but enormous quotient to int is undefined, so a
    // range check on the converted value would already be too late.
    const qreal column_index = std::floor(point.x() / metrics.width);
    const qreal row_index    = std::floor(point.y() / metrics.height);
    const qreal column_limit = static_cast<qreal>(snapshot->grid_size.columns);
    const qreal row_limit    = static_cast<qreal>(snapshot->grid_size.rows);
    if (row_index <  0.0       || column_index <  0.0          ||
        row_index >= row_limit || column_index >= column_limit)
    {
        return std::nullopt;
    }

    return term::terminal_grid_position_t{
        static_cast<int>(row_index),
        static_cast<int>(column_index)};
}

struct Surface_hyperlink_target
{
    QByteArray identity_key;
    QByteArray uri;
};

std::optional<Surface_hyperlink_target> explicit_hyperlink_for_local_point(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot,
    term::terminal_cell_metrics_t                                metrics,
    QPointF                                                      point)
{
    const std::optional<term::terminal_grid_position_t> position =
        grid_position_for_local_point(snapshot, metrics, point);
    if (!position.has_value()) {
        return std::nullopt;
    }

    const term::Terminal_render_snapshot_row_content_view rows(*snapshot);
    const term::Terminal_render_cell* cell = rows.cell_at(
        position->row,
        position->column);
    if (cell == nullptr || cell->hyperlink_id == term::k_no_terminal_hyperlink_id) {
        return std::nullopt;
    }

    const auto metadata = std::find_if(
        snapshot->hyperlinks.begin(),
        snapshot->hyperlinks.end(),
        [hyperlink_id = cell->hyperlink_id](
            const term::Terminal_render_hyperlink_metadata& candidate) {
            return candidate.hyperlink_id == hyperlink_id;
        });
    if (metadata == snapshot->hyperlinks.end()) {
        return std::nullopt;
    }

    return Surface_hyperlink_target{metadata->identity_key, metadata->uri};
}

bool same_surface_hyperlink_target(
    const Surface_hyperlink_target& left,
    const Surface_hyperlink_target& right)
{
    return left.identity_key == right.identity_key && left.uri == right.uri;
}

bool explicit_hyperlink_activation_modifiers(Qt::KeyboardModifiers modifiers)
{
    constexpr Qt::KeyboardModifiers k_relevant_modifiers =
        Qt::ControlModifier |
        Qt::ShiftModifier   |
        Qt::AltModifier     |
        Qt::MetaModifier;
    return (modifiers & k_relevant_modifiers) == Qt::ControlModifier;
}

std::optional<term::terminal_grid_position_t> clamped_grid_position_for_local_point(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_cell_metrics_t                                  metrics,
    QPointF                                                        point)
{
    if (snapshot == nullptr ||
        !term::is_valid_grid_size(snapshot->grid_size) ||
        !term::is_valid_cell_metrics(metrics) ||
        !std::isfinite(point.x()) ||
        !std::isfinite(point.y()))
    {
        return std::nullopt;
    }

    // Clamped in qreal for the same reason the unclamped lookup range-checks in
    // qreal: the conversion is what is undefined for an out-of-range value, so
    // it has to come after the clamp rather than inside it.
    const qreal column_index = std::clamp(
        std::floor(point.x() / metrics.width),
        0.0,
        static_cast<qreal>(snapshot->grid_size.columns - 1));
    const qreal row_index = std::clamp(
        std::floor(point.y() / metrics.height),
        0.0,
        static_cast<qreal>(snapshot->grid_size.rows - 1));
    return term::terminal_grid_position_t{
        static_cast<int>(row_index),
        static_cast<int>(column_index)};
}

std::optional<term::terminal_grid_position_t> logical_grid_position_for_viewport_cell(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_grid_position_t                                 viewport_position)
{
    if (snapshot                 == nullptr                  ||
        !term::is_valid_grid_size(snapshot->grid_size)       ||
        viewport_position.row    <  0                        ||
        viewport_position.row    >= snapshot->grid_size.rows ||
        viewport_position.column <  0                        ||
        viewport_position.column >= snapshot->grid_size.columns)
    {
        return std::nullopt;
    }

    const int first_visible_logical_row =
        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
            ? 0
            : snapshot->viewport.scrollback_rows - snapshot->viewport.offset_from_tail;
    return term::terminal_grid_position_t{
        first_visible_logical_row + viewport_position.row,
        viewport_position.column,
    };
}

bool position_is_before(
    term::terminal_grid_position_t lhs,
    term::terminal_grid_position_t rhs)
{
    return lhs.row < rhs.row || (lhs.row == rhs.row && lhs.column < rhs.column);
}

bool selection_source_matches_snapshot(
    const term::terminal_selection_source_identity_t& source,
    const term::Terminal_render_snapshot&             snapshot)
{
    return
        source.buffer_id == snapshot.viewport.active_buffer &&
        source.row_origin_generation == snapshot.metadata.row_origin_generation &&
        term::grid_sizes_match(source.grid_size, snapshot.grid_size) &&
        term::viewport_mappings_match(source.viewport_mapping, snapshot.viewport);
}

void write_selection_trace(bool enabled, const QString& message)
{
    term::record_interaction_trace("selection", "state", message);
    if (!enabled) {
        return;
    }

    std::fprintf(stderr, "[vnm-terminal-selection] %s\n", qPrintable(message));
}

QString selection_trace_bool(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

QString selection_trace_mouse_button(Qt::MouseButton button)
{
    return QString::number(static_cast<int>(button));
}

QString selection_trace_mouse_buttons(Qt::MouseButtons buttons)
{
    return QString::number(static_cast<int>(buttons));
}

QString selection_trace_modifiers(Qt::KeyboardModifiers modifiers)
{
    return QString::number(static_cast<int>(modifiers));
}

QString selection_trace_point(QPointF point)
{
    return QStringLiteral("(%1,%2)")
        .arg(point.x(), 0, 'f', 1)
        .arg(point.y(), 0, 'f', 1);
}

QString selection_trace_grid_position(term::terminal_grid_position_t position)
{
    return QStringLiteral("%1:%2").arg(position.row).arg(position.column);
}

QString selection_trace_grid_position(
    const std::optional<term::terminal_grid_position_t>& position)
{
    return position.has_value()
        ? selection_trace_grid_position(*position)
        : QStringLiteral("none");
}

QString selection_trace_range(const term::Terminal_selection_range& range)
{
    return QStringLiteral("%1->%2,mode=%3")
        .arg(selection_trace_grid_position(range.start))
        .arg(selection_trace_grid_position(range.end))
        .arg(static_cast<int>(range.mode));
}

QString selection_trace_grid_size(term::terminal_grid_size_t grid_size)
{
    return QStringLiteral("%1x%2").arg(grid_size.rows).arg(grid_size.columns);
}

QString selection_trace_content_basis(
    term::terminal_selection_content_basis_t content_basis)
{
    return QStringLiteral("content=%1,reflow=%2")
        .arg(static_cast<qulonglong>(content_basis.content_generation))
        .arg(static_cast<qulonglong>(content_basis.grid_reflow_generation));
}

QString selection_trace_viewport(const term::Terminal_viewport_state& viewport)
{
    return QStringLiteral("buffer=%1,visible=%2,scrollback=%3,offset=%4")
        .arg(static_cast<int>(viewport.active_buffer))
        .arg(viewport.visible_rows)
        .arg(viewport.scrollback_rows)
        .arg(viewport.offset_from_tail);
}

QString selection_trace_source_identity(
    const term::terminal_selection_source_identity_t& source)
{
    return QStringLiteral(
        "source{basis={%1},epoch=%2,buffer=%3,grid_reflow=%4,row_origin=%5,"
        "grid=%6,viewport={%7}}")
        .arg(selection_trace_content_basis(source.source_content_basis))
        .arg(static_cast<qulonglong>(source.session_epoch))
        .arg(static_cast<int>(source.buffer_id))
        .arg(static_cast<qulonglong>(source.grid_reflow_basis))
        .arg(static_cast<qulonglong>(source.row_origin_generation))
        .arg(selection_trace_grid_size(source.grid_size))
        .arg(selection_trace_viewport(source.viewport_mapping));
}

QString selection_trace_source_identity(
    const std::optional<term::terminal_selection_source_identity_t>& source)
{
    return source.has_value()
        ? selection_trace_source_identity(*source)
        : QStringLiteral("source{none}");
}

QString selection_trace_snapshot_identity(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    if (snapshot == nullptr) {
        return QStringLiteral("snapshot{none}");
    }

    return QStringLiteral("snapshot{seq=%1,row_origin=%2,grid=%3,viewport={%4}}")
        .arg(static_cast<qulonglong>(snapshot->metadata.sequence))
        .arg(static_cast<qulonglong>(snapshot->metadata.row_origin_generation))
        .arg(selection_trace_grid_size(snapshot->grid_size))
        .arg(selection_trace_viewport(snapshot->viewport));
}

void append_selection_trace_reason(QString& reasons, const QString& reason)
{
    if (!reasons.isEmpty()) {
        reasons += QLatin1Char(',');
    }
    reasons += reason;
}

QString selection_trace_drag_coordinate_mismatch_reason(
    const term::terminal_selection_source_identity_t& left,
    const term::terminal_selection_source_identity_t& right)
{
    QString reasons;
    if (left.source_content_basis.grid_reflow_generation !=
        right.source_content_basis.grid_reflow_generation)
    {
        append_selection_trace_reason(reasons, QStringLiteral("content-reflow"));
    }
    if (left.session_epoch != right.session_epoch) {
        append_selection_trace_reason(reasons, QStringLiteral("epoch"));
    }
    if (left.buffer_id != right.buffer_id) {
        append_selection_trace_reason(reasons, QStringLiteral("buffer"));
    }
    if (left.grid_reflow_basis != right.grid_reflow_basis) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-reflow"));
    }
    if (!term::grid_sizes_match(left.grid_size, right.grid_size)) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-size"));
    }
    return reasons.isEmpty() ? QStringLiteral("none") : reasons;
}

QString selection_trace_source_snapshot_mismatch_reason(
    const std::optional<term::terminal_selection_source_identity_t>& source,
    const std::shared_ptr<const term::Terminal_render_snapshot>&     snapshot)
{
    if (!source.has_value()) {
        return QStringLiteral("source-missing");
    }
    if (snapshot == nullptr) {
        return QStringLiteral("snapshot-missing");
    }

    QString reasons;
    if (source->buffer_id != snapshot->viewport.active_buffer) {
        append_selection_trace_reason(reasons, QStringLiteral("buffer"));
    }
    if (source->row_origin_generation != snapshot->metadata.row_origin_generation) {
        append_selection_trace_reason(reasons, QStringLiteral("row-origin"));
    }
    if (!term::grid_sizes_match(source->grid_size, snapshot->grid_size)) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-size"));
    }
    if (!term::viewport_mappings_match(source->viewport_mapping, snapshot->viewport)) {
        append_selection_trace_reason(reasons, QStringLiteral("viewport-mapping"));
    }
    return reasons.isEmpty() ? QStringLiteral("none") : reasons;
}

QString selection_trace_drag_state(
    bool active,
    bool moved,
    bool cancelled)
{
    return QStringLiteral("active=%1,moved=%2,cancelled=%3")
        .arg(selection_trace_bool(active))
        .arg(selection_trace_bool(moved))
        .arg(selection_trace_bool(cancelled));
}

void trace_surface_mouse_decision(
    bool                                                      trace_enabled,
    const QString&                                            phase,
    const QString&                                            reason,
    QPointF                                                   local_point,
    const std::optional<term::terminal_grid_position_t>&      viewport_position,
    const std::optional<term::terminal_grid_position_t>&      logical_position,
    Qt::MouseButton                                           button,
    Qt::MouseButtons                                          buttons,
    Qt::KeyboardModifiers                                     modifiers,
    bool                                                      drag_active,
    bool                                                      drag_moved,
    bool                                                      drag_cancelled,
    bool                                                      accepted)
{
    if (!trace_enabled) {
        return;
    }

    QString message = QStringLiteral("surface ") + phase;
    message += QStringLiteral(" reason=") + reason;
    message += QStringLiteral(" local=") + selection_trace_point(local_point);
    message += QStringLiteral(" viewport=") + selection_trace_grid_position(viewport_position);
    message += QStringLiteral(" logical=") + selection_trace_grid_position(logical_position);
    message += QStringLiteral(" button=") + selection_trace_mouse_button(button);
    message += QStringLiteral(" buttons=") + selection_trace_mouse_buttons(buttons);
    message += QStringLiteral(" modifiers=") + selection_trace_modifiers(modifiers);
    message += QStringLiteral(" drag=") +
        selection_trace_drag_state(drag_active, drag_moved, drag_cancelled);
    message += QStringLiteral(" accepted=") + selection_trace_bool(accepted);
    write_selection_trace(trace_enabled, message);
}

term::Terminal_selection_range selection_range_for_drag(
    term::terminal_grid_position_t anchor,
    term::terminal_grid_position_t current)
{
    if (position_is_before(current, anchor)) {
        ++anchor.column;
        return {anchor, current, term::Terminal_selection_mode::NORMAL};
    }

    ++current.column;
    return {anchor, current, term::Terminal_selection_mode::NORMAL};
}

enum class Selection_session_mutation
{
    CLEAR_SELECTION,
    DETACH_SELECTION_VISUAL_ATTACHMENT,
    SET_SELECTION_RANGE,
};

struct Selection_drag_session_mutation
{
    Selection_session_mutation              mutation =
        Selection_session_mutation::CLEAR_SELECTION;
    std::optional<term::Terminal_selection_range>
                                             range;
};

Selection_drag_session_mutation selection_drag_session_mutation_for_drag(
    term::terminal_grid_position_t anchor,
    term::terminal_grid_position_t current,
    bool                           drag_moved)
{
    if (!drag_moved && current == anchor) {
        return {};
    }

    return {
        Selection_session_mutation::SET_SELECTION_RANGE,
        selection_range_for_drag(anchor, current),
    };
}

bool local_selection_override(Qt::KeyboardModifiers modifiers)
{
    return (modifiers & Qt::ShiftModifier) != Qt::NoModifier;
}

term::Terminal_cursor_shape terminal_cursor_shape(VNM_TerminalSurface::Cursor_style style)
{
    switch (style) {
        case VNM_TerminalSurface::Cursor_style::BLOCK:
            return term::Terminal_cursor_shape::BLOCK;
        case VNM_TerminalSurface::Cursor_style::BAR:
            return term::Terminal_cursor_shape::BAR;
        case VNM_TerminalSurface::Cursor_style::UNDERLINE:
            return term::Terminal_cursor_shape::UNDERLINE;
    }

    return term::Terminal_cursor_shape::BLOCK;
}

term::Terminal_text_renderer_policy terminal_text_renderer_policy(
    VNM_TerminalSurface::Text_renderer_mode mode)
{
    switch (mode) {
        case VNM_TerminalSurface::Text_renderer_mode::AUTO:
            return term::Terminal_text_renderer_policy::AUTO;
        case VNM_TerminalSurface::Text_renderer_mode::MSDF:
            return term::Terminal_text_renderer_policy::MSDF;
        case VNM_TerminalSurface::Text_renderer_mode::GLYPH:
            return term::Terminal_text_renderer_policy::GLYPH;
    }

    return term::Terminal_text_renderer_policy::AUTO;
}

term::Terminal_lcd_subpixel_order terminal_lcd_subpixel_order_from_qt(
    QQuickWindow* window)
{
    if (window == nullptr) {
        return term::Terminal_lcd_subpixel_order::NONE;
    }

    const QPlatformScreen* const platform_screen =
        QPlatformScreen::platformScreenForWindow(window);
    if (platform_screen == nullptr) {
        return term::Terminal_lcd_subpixel_order::NONE;
    }

    switch (platform_screen->subpixelAntialiasingTypeHint()) {
        case QPlatformScreen::Subpixel_RGB:
            return term::Terminal_lcd_subpixel_order::RGB;
        case QPlatformScreen::Subpixel_BGR:
            return term::Terminal_lcd_subpixel_order::BGR;
        case QPlatformScreen::Subpixel_VRGB:
            return term::Terminal_lcd_subpixel_order::VRGB;
        case QPlatformScreen::Subpixel_VBGR:
            return term::Terminal_lcd_subpixel_order::VBGR;
        case QPlatformScreen::Subpixel_None:
            return term::Terminal_lcd_subpixel_order::NONE;
    }

    return term::Terminal_lcd_subpixel_order::NONE;
}

term::Terminal_lcd_subpixel_order terminal_lcd_subpixel_order_from_windows()
{
#if defined(_WIN32)
    int font_smoothing_enabled = 0;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing,
            0U,
            &font_smoothing_enabled,
            0U) == 0 ||
        font_smoothing_enabled == 0)
    {
        return term::Terminal_lcd_subpixel_order::NONE;
    }

    unsigned int font_smoothing_type = 0U;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_type,
            0U,
            &font_smoothing_type,
            0U) == 0 ||
        font_smoothing_type != k_win_font_smoothing_cleartype)
    {
        return term::Terminal_lcd_subpixel_order::NONE;
    }

    unsigned int font_smoothing_orientation = 0U;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_orientation,
            0U,
            &font_smoothing_orientation,
            0U) == 0)
    {
        return term::Terminal_lcd_subpixel_order::NONE;
    }

    switch (font_smoothing_orientation) {
        case k_win_font_smoothing_orientation_rgb:
            return term::Terminal_lcd_subpixel_order::RGB;
        case k_win_font_smoothing_orientation_bgr:
            return term::Terminal_lcd_subpixel_order::BGR;
    }
#endif

    return term::Terminal_lcd_subpixel_order::NONE;
}

term::Terminal_lcd_subpixel_order terminal_lcd_subpixel_order_from_mode(
    VNM_TerminalSurface::Lcd_subpixel_order order)
{
    switch (order) {
        case VNM_TerminalSurface::Lcd_subpixel_order::AUTO:
        case VNM_TerminalSurface::Lcd_subpixel_order::NONE:
            return term::Terminal_lcd_subpixel_order::NONE;
        case VNM_TerminalSurface::Lcd_subpixel_order::RGB:
            return term::Terminal_lcd_subpixel_order::RGB;
        case VNM_TerminalSurface::Lcd_subpixel_order::BGR:
            return term::Terminal_lcd_subpixel_order::BGR;
        case VNM_TerminalSurface::Lcd_subpixel_order::VRGB:
            return term::Terminal_lcd_subpixel_order::VRGB;
        case VNM_TerminalSurface::Lcd_subpixel_order::VBGR:
            return term::Terminal_lcd_subpixel_order::VBGR;
    }

    return term::Terminal_lcd_subpixel_order::NONE;
}

term::Terminal_lcd_subpixel_order terminal_lcd_subpixel_order(
    VNM_TerminalSurface::Lcd_subpixel_order order,
    QQuickWindow*                           window)
{
    if (order != VNM_TerminalSurface::Lcd_subpixel_order::AUTO) {
        return terminal_lcd_subpixel_order_from_mode(order);
    }

    const term::Terminal_lcd_subpixel_order qt_order =
        terminal_lcd_subpixel_order_from_qt(window);
    if (qt_order != term::Terminal_lcd_subpixel_order::NONE) {
        return qt_order;
    }

    return terminal_lcd_subpixel_order_from_windows();
}

term::Terminal_render_options render_options_for_surface(const VNM_TerminalSurface& surface)
{
    term::Terminal_render_options options;

    const term::Terminal_color_scheme& scheme = resolve_surface_color_scheme(surface);
    options.default_background   = QColor::fromRgba(scheme.background_rgba);
    options.default_foreground   = QColor::fromRgba(scheme.foreground_rgba);
    options.cursor_color         = QColor::fromRgba(scheme.cursor_rgba);
    options.selection_background = QColor::fromRgba(scheme.selection_rgba);
    options.selection_foreground =
        term::terminal_selection_foreground_for_background(
            options.selection_background);

    const bool light_scheme = color_is_light(scheme.background_rgba);
    options.preedit_background = light_scheme
        ? QColor(220, 220, 205, 170)
        : QColor(96,  96,  96,  120);
    options.visual_bell_color = light_scheme
        ? QColor(255, 255, 255, 96)
        : QColor(255, 255, 255, 70);

    options.cursor_shape_override         = terminal_cursor_shape(surface.cursor_style());
    options.cursor_blink_enabled_override = surface.cursor_blink_enabled();
    options.underline_hyperlinks           = true;
    options.visual_bell_enabled =
        surface.visual_bell_policy() == VNM_TerminalSurface::Bell_policy::ENABLED;
    options.text_renderer_policy =
        terminal_text_renderer_policy(surface.text_renderer_mode());
    options.msdf_lcd_subpixel_order =
        terminal_lcd_subpixel_order(
            surface.lcd_subpixel_order(),
            surface.window());
    return options;
}

std::uint64_t atlas_frame_snapshot_sequence(const term::Captured_atlas_frame& frame)
{
    return frame.snapshot != nullptr
        ? frame.snapshot->metadata.sequence
        : 0U;
}

std::uint64_t atlas_frame_publication_generation(const term::Captured_atlas_frame& frame)
{
    return frame.publication_generation != 0U
        ? frame.publication_generation
        : frame.snapshot != nullptr
            ? frame.snapshot->metadata.publication_generation
            : 0U;
}

int preedit_cursor_position_from_event(const QInputMethodEvent& event)
{
    const int text_size = static_cast<int>(
        std::min<qsizetype>(event.preeditString().size(), std::numeric_limits<int>::max()));
    int cursor_position = text_size;
    for (const QInputMethodEvent::Attribute& attribute : event.attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            cursor_position = attribute.start;
            break;
        }
    }

    return std::clamp(cursor_position, 0, text_size);
}

QString terminal_commit_text_from_event(const QInputMethodEvent& event)
{
    // Replacement ranges target widgets with an editable local document. The
    // terminal surface only owns an input byte stream, so non-default ranges
    // are deliberately folded into the commit text and sent as ordinary input.
    return event.commitString();
}

void warm_text_layout(
    const QFont&   font,
    const QString& text)
{
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(1024.0 * 1024.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
}

void warm_prompt_glyph_text_layouts(const QFont& font)
{
    static const QStringList k_prompt_glyphs = {
        QStringLiteral("\u203A"), // U+203A single right angle quote
        QStringLiteral("\u276F"), // U+276F heavy right angle quote ornament
        QStringLiteral("\u276E"), // U+276E heavy left angle quote ornament
        QStringLiteral("\u2192"), // U+2192 right arrow
        QStringLiteral("\u2190"), // U+2190 left arrow
        QStringLiteral("\u2713"), // U+2713 check mark
        QStringLiteral("\u2717"), // U+2717 ballot x
        QStringLiteral("\u2026"), // U+2026 horizontal ellipsis
        QStringLiteral("\u26A1"), // U+26A1 high voltage sign
        QStringLiteral("\u25B8"), // U+25B8 black right-pointing small triangle
        QStringLiteral("\u25BE"), // U+25BE black down-pointing small triangle
        QStringLiteral("\uE0B0"), // powerline separator
        QStringLiteral("\uE0B1"),
        QStringLiteral("\uE0B2"),
        QStringLiteral("\uE0B3"),
    };

    for (const QString& glyph : k_prompt_glyphs) {
        warm_text_layout(font, glyph);
    }
}

int display_width_for_preedit_cursor(const QString& text, int cursor_position)
{
    const int text_size = static_cast<int>(
        std::min<qsizetype>(text.size(), std::numeric_limits<int>::max()));
    const int clamped_cursor_position = std::clamp(cursor_position, 0, text_size);
    return term::measure_utf8_width(text.left(clamped_cursor_position).toUtf8()).cells;
}

QString wheel_trace_buffer_name(term::Terminal_buffer_id buffer)
{
    switch (buffer) {
        case term::Terminal_buffer_id::PRIMARY:
            return QStringLiteral("primary");
        case term::Terminal_buffer_id::ALTERNATE:
            return QStringLiteral("alternate");
    }

    return QStringLiteral("unknown");
}

QString wheel_trace_alternate_scroll_policy_name(
    term::Terminal_alternate_screen_scroll_policy policy)
{
    switch (policy) {
        case term::Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL:
            return QStringLiteral("keep_at_tail");
        case term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT:
            return QStringLiteral("wheel_to_terminal_input");
    }

    return QStringLiteral("unknown");
}

QString wheel_event_policy_name(VNM_TerminalSurface::Wheel_event_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Wheel_event_policy::APPLICATION_CONTROLLED:
            return QStringLiteral("application_controlled");
        case VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST:
            return QStringLiteral("local_scrollback_first");
        case VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_ONLY:
            return QStringLiteral("local_scrollback_only");
    }

    return QStringLiteral("unknown");
}

QString alternate_screen_wheel_policy_name(
    VNM_TerminalSurface::Alternate_screen_wheel_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST:
            return QStringLiteral("mouse_reporting_first");
        case VNM_TerminalSurface::Alternate_screen_wheel_policy::CURSOR_KEYS:
            return QStringLiteral("cursor_keys");
        case VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS:
            return QStringLiteral("page_keys");
    }

    return QStringLiteral("unknown");
}

QString mouse_reporting_policy_name(VNM_TerminalSurface::Mouse_reporting_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Mouse_reporting_policy::DISABLED:
            return QStringLiteral("disabled");
        case VNM_TerminalSurface::Mouse_reporting_policy::APPLICATION_CONTROLLED:
            return QStringLiteral("application_controlled");
    }

    return QStringLiteral("unknown");
}

term::Terminal_bell_policy terminal_bell_policy_for_surface(
    VNM_TerminalSurface::Bell_policy audible_policy,
    VNM_TerminalSurface::Bell_policy visual_policy)
{
    term::Terminal_bell_policy policy;
    policy.audible_enabled = audible_policy == VNM_TerminalSurface::Bell_policy::ENABLED;
    policy.visual_enabled  = visual_policy  == VNM_TerminalSurface::Bell_policy::ENABLED;
    return policy;
}

term::Terminal_synchronized_output_scroll_policy terminal_synchronized_output_scroll_policy(
    VNM_TerminalSurface::Synchronized_output_scroll_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION:
            return term::Terminal_synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION;
        case VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION:
            return term::Terminal_synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION;
    }

    return term::Terminal_synchronized_output_scroll_policy::
        DEFER_UNTIL_CONTENT_PUBLICATION;
}

term::Terminal_text_area_resize_policy terminal_text_area_resize_policy(
    VNM_TerminalSurface::Text_area_resize_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Text_area_resize_policy::APPLICATION_CONTROLLED:
            return term::Terminal_text_area_resize_policy::APPLICATION_CONTROLLED;
        case VNM_TerminalSurface::Text_area_resize_policy::DISABLED:
            return term::Terminal_text_area_resize_policy::DISABLED;
    }

    return term::Terminal_text_area_resize_policy::APPLICATION_CONTROLLED;
}

VNM_TerminalSurface::Text_area_resize_arbitration_outcome
surface_text_area_resize_arbitration_outcome(
    term::Terminal_text_area_resize_arbitration_outcome outcome)
{
    using Surface_outcome = VNM_TerminalSurface::Text_area_resize_arbitration_outcome;
    switch (outcome) {
        case term::Terminal_text_area_resize_arbitration_outcome::ACCEPTED:
            return Surface_outcome::ACCEPTED;
        case term::Terminal_text_area_resize_arbitration_outcome::REJECTED:
            return Surface_outcome::REJECTED;
        case term::Terminal_text_area_resize_arbitration_outcome::HOLD_LIMIT_REACHED:
            return Surface_outcome::HOLD_LIMIT_REACHED;
        case term::Terminal_text_area_resize_arbitration_outcome::TEXT_AREA_RESIZE_DISABLED:
            return Surface_outcome::TEXT_AREA_RESIZE_DISABLED;
        case term::Terminal_text_area_resize_arbitration_outcome::ARBITRATION_DISABLED:
            return Surface_outcome::ARBITRATION_DISABLED;
        case term::Terminal_text_area_resize_arbitration_outcome::PROCESS_EXITED:
            return Surface_outcome::PROCESS_EXITED;
        case term::Terminal_text_area_resize_arbitration_outcome::TIMED_OUT:
            return Surface_outcome::TIMED_OUT;
    }

    return Surface_outcome::REJECTED;
}

// Marks the notification delivery loop that owns the current batch. Lowering the
// latch from the destructor matters: the loop returns early whenever the active
// session changes under it, and a latch left raised would silence every later
// notification.
class Session_notification_delivery_scope
{
public:
    explicit Session_notification_delivery_scope(bool& delivering)
    :
        m_delivering(delivering)
    {
        m_delivering = true;
    }

    Session_notification_delivery_scope(const Session_notification_delivery_scope&) = delete;
    Session_notification_delivery_scope& operator=(
        const Session_notification_delivery_scope&) = delete;

    ~Session_notification_delivery_scope()
    {
        m_delivering = false;
    }

private:
    bool& m_delivering;
};

QString surface_synchronized_output_scroll_policy_name(
    VNM_TerminalSurface::Synchronized_output_scroll_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION:
            return QStringLiteral("defer_until_content_publication");
        case VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION:
            return QStringLiteral("immediate_public_projection");
    }

    return QStringLiteral("unknown");
}

VNM_TerminalSurface::Scroll_action public_scroll_action(
    term::Terminal_viewport_scroll_action action)
{
    using Public = VNM_TerminalSurface::Scroll_action;
    switch (action) {
        case term::Terminal_viewport_scroll_action::VIEWPORT_MOVED:           return Public::VIEWPORT_MOVED;
        case term::Terminal_viewport_scroll_action::AT_BOUNDARY:              return Public::AT_BOUNDARY;
        case term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED: return Public::DEFERRED_INTENT_RECORDED;
        case term::Terminal_viewport_scroll_action::TERMINAL_INPUT:           return Public::TERMINAL_INPUT;
    }

    return Public::NONE;
}

QJsonObject wheel_trace_viewport_object(const term::Terminal_viewport_state& viewport)
{
    return {
        {QStringLiteral("active_buffer"),    wheel_trace_buffer_name(viewport.active_buffer)},
        {QStringLiteral("scrollback_rows"),  viewport.scrollback_rows},
        {QStringLiteral("visible_rows"),     viewport.visible_rows},
        {QStringLiteral("offset_from_tail"), viewport.offset_from_tail},
        {QStringLiteral("follow_tail"),      viewport.follow_tail},
        {QStringLiteral("alternate_screen_scroll_policy"),
            wheel_trace_alternate_scroll_policy_name(
                viewport.alternate_screen_scroll_policy)},
    };
}

bool wheel_trace_viewport_valid(const term::Terminal_viewport_state& viewport)
{
    return viewport.visible_rows > 0;
}

void insert_wheel_trace_viewport(
    QJsonObject&                         object,
    const QString&                       field_name,
    const term::Terminal_viewport_state& viewport)
{
    if (wheel_trace_viewport_valid(viewport)) {
        object.insert(field_name, wheel_trace_viewport_object(viewport));
    }
}

QString wheel_trace_local_scroll_block_reason(
    const term::Terminal_viewport_state& viewport,
    int                                  scroll_direction)
{
    if (viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE) {
        return QStringLiteral("alternate_screen");
    }

    if (scroll_direction == 0) {
        return QStringLiteral("zero_vertical_delta");
    }

    if (scroll_direction > 0 && viewport.offset_from_tail >= viewport.scrollback_rows) {
        return QStringLiteral("top_boundary");
    }

    if (scroll_direction < 0 && viewport.offset_from_tail <= 0) {
        return QStringLiteral("tail_boundary");
    }

    return QStringLiteral("no_local_scroll");
}

QString wheel_trace_local_scroll_block_outcome(const QString& block_reason)
{
    if (block_reason == QStringLiteral("alternate_screen")) {
        return QStringLiteral("alternate_screen");
    }

    if (block_reason == QStringLiteral("no_local_scroll")) {
        return QStringLiteral("no_publication");
    }

    return QStringLiteral("boundary_or_clamp");
}

QString wheel_trace_local_scroll_noop_outcome(
    const term::Terminal_viewport_state&          viewport,
    term::Terminal_viewport_scroll_action         action,
    bool                                          render_publication_blocked,
    bool                                          published_synchronized_output)
{
    if (render_publication_blocked) {
        return QStringLiteral("synchronized_output_deferred");
    }

    if (published_synchronized_output) {
        return QStringLiteral("synchronized_output_published");
    }

    if (viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE) {
        return QStringLiteral("alternate_screen");
    }

    if (action == term::Terminal_viewport_scroll_action::AT_BOUNDARY) {
        return QStringLiteral("boundary_or_clamp");
    }

    return QStringLiteral("no_publication");
}

QJsonObject wheel_trace_event_object(const QString& source, const QWheelEvent& event)
{
    return {
        {QStringLiteral("source"),        source},
        {QStringLiteral("angle_delta_x"), event.angleDelta().x()},
        {QStringLiteral("angle_delta_y"), event.angleDelta().y()},
        {QStringLiteral("pixel_delta_x"), event.pixelDelta().x()},
        {QStringLiteral("pixel_delta_y"), event.pixelDelta().y()},
        {QStringLiteral("modifiers"),     static_cast<int>(event.modifiers())},
    };
}

void record_surface_wheel_trace_transcript(
    const std::shared_ptr<term::Terminal_transcript_recorder>& recorder,
    QJsonObject                                                object)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (recorder == nullptr) {
        return;
    }

    (void)recorder->record_surface_wheel_trace(std::move(object));
#else
    (void)recorder;
    (void)object;
#endif
}

void record_surface_wheel_ingress_transcript(
    const std::shared_ptr<term::Terminal_transcript_recorder>& recorder,
    const QString&                                             source,
    const QWheelEvent&                                         event)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (recorder == nullptr) {
        return;
    }

    QJsonObject object = wheel_trace_event_object(source, event);
    object.insert(QStringLiteral("phase"), QStringLiteral("ingress"));
    object.insert(QStringLiteral("accepted_on_entry"), event.isAccepted());
    object.insert(QStringLiteral("position_x"), event.position().x());
    object.insert(QStringLiteral("position_y"), event.position().y());
    (void)recorder->record_surface_wheel_ingress(std::move(object));
#else
    (void)recorder;
    (void)source;
    (void)event;
#endif
}

void record_surface_scroll_transcript(
    const std::shared_ptr<term::Terminal_transcript_recorder>& recorder,
    const QString&                                             source,
    int                                                        requested_line_delta,
    std::optional<int>                                         requested_offset_from_tail,
    const term::Terminal_viewport_scroll_result&               result,
    const term::Terminal_viewport_state&                       viewport_before,
    const term::Terminal_viewport_state&                       viewport_after)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    const bool should_record =
        result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED ||
        result.action == term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED;
    if (recorder == nullptr || !should_record) {
        return;
    }

    (void)recorder->record_surface_scroll({
        source,
        requested_line_delta,
        requested_offset_from_tail,
        result,
        viewport_before,
        viewport_after,
    });
#else
    (void)recorder;
    (void)source;
    (void)requested_line_delta;
    (void)requested_offset_from_tail;
    (void)result;
    (void)viewport_before;
    (void)viewport_after;
#endif
}

void record_surface_scroll_intent_transcript(
    const std::shared_ptr<term::Terminal_transcript_recorder>& recorder,
    const QString&                                             source,
    int                                                        requested_line_delta,
    std::optional<int>                                         requested_offset_from_tail,
    const term::Terminal_viewport_state&                       viewport_before)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (recorder == nullptr) {
        return;
    }

    (void)recorder->record_surface_scroll_intent({
        source,
        requested_line_delta,
        requested_offset_from_tail,
        viewport_before,
    });
#else
    (void)recorder;
    (void)source;
    (void)requested_line_delta;
    (void)requested_offset_from_tail;
    (void)viewport_before;
#endif
}

void record_surface_selection_drag_transcript(
    const std::shared_ptr<term::Terminal_transcript_recorder>& recorder,
    const QString&                                             phase,
    std::optional<term::terminal_grid_position_t>              anchor,
    std::optional<term::terminal_grid_position_t>              focus,
    std::optional<term::Terminal_selection_range>              range,
    bool                                                       moved)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (recorder == nullptr) {
        return;
    }

    (void)recorder->record_surface_selection_drag({
        phase,
        anchor,
        focus,
        range,
        moved,
    });
#else
    (void)recorder;
    (void)phase;
    (void)anchor;
    (void)focus;
    (void)range;
    (void)moved;
#endif
}

}

struct VNM_TerminalSurface::Private
{
    using Renderer_lifecycle_recorder = term::Terminal_renderer_lifecycle_recorder;

    std::shared_ptr<Renderer_lifecycle_recorder> lifecycle_recorder() const
    {
        if (!renderer_lifecycle_recorder_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(renderer_lifecycle_recorder_mutex);
        return renderer_lifecycle_recorder;
    }

    std::shared_ptr<Renderer_lifecycle_recorder> ensure_lifecycle_recorder()
    {
        const std::lock_guard<std::mutex> lock(renderer_lifecycle_recorder_mutex);
        if (renderer_lifecycle_recorder == nullptr) {
            renderer_lifecycle_recorder = std::make_shared<Renderer_lifecycle_recorder>();
            renderer_lifecycle_recorder_enabled.store(true, std::memory_order_release);
        }

        return renderer_lifecycle_recorder;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    std::shared_ptr<term::Hierarchical_profiler> render_profiler_handle() const
    {
        if (!render_profiler_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        return render_profiler;
    }

    void set_render_profiler(std::shared_ptr<term::Hierarchical_profiler> profiler)
    {
        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        render_profiler = std::move(profiler);
        render_profiler_snapshot = {};
        if (render_profiler != nullptr) {
            if (slow_text_layout_recorder == nullptr) {
                slow_text_layout_recorder =
                    std::make_shared<term::Terminal_text_layout_slow_diagnostics_recorder>();
            }
            else {
                slow_text_layout_recorder->reset();
            }
        }
        else {
            slow_text_layout_recorder.reset();
        }
        render_profiler_enabled.store(render_profiler != nullptr, std::memory_order_release);
    }

    std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
    slow_text_layout_recorder_handle() const
    {
        if (!render_profiler_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        return slow_text_layout_recorder;
    }

    term::Render_profile_snapshot_t current_render_profiler_snapshot() const
    {
        std::shared_ptr<term::Hierarchical_profiler> profiler;
        std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
            slow_recorder;
        {
            const std::lock_guard<std::mutex> lock(render_profiler_mutex);
            profiler      = render_profiler;
            slow_recorder = slow_text_layout_recorder;
        }

        if (profiler == nullptr) {
            const std::lock_guard<std::mutex> lock(render_profiler_mutex);
            return render_profiler_snapshot;
        }

        term::Render_profile_snapshot_t snapshot;
        if (qsg_atlas_recorder != nullptr) {
            const term::Qsg_atlas_frame_report atlas_report =
                qsg_atlas_recorder->snapshot();
            snapshot.sequence = atlas_report.render_count > 0U
                ? atlas_report.render_snapshot_sequence
                : 0U;
        }
        snapshot.root     = profiler->root_snapshot();
        snapshot.timeline = profiler->timeline_snapshot();
        if (slow_recorder != nullptr) {
            snapshot.slow_text_layouts = slow_recorder->snapshot();
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        if (render_profiler == profiler) {
            render_profiler_snapshot = std::move(snapshot);
        }
        return render_profiler_snapshot;
    }
#else
    void set_render_profiler(std::shared_ptr<term::Hierarchical_profiler> profiler)
    {
        Q_UNUSED(profiler);
    }

    term::Render_profile_snapshot_t current_render_profiler_snapshot() const
    {
        return {};
    }
#endif

    void request_render_update(VNM_TerminalSurface& surface)
    {
        ++render_invalidation_stats.update_requests;
        QQuickWindow* render_window = surface.window();
        if (render_window == nullptr) {
            render_update_pending = false;
            render_update_window = nullptr;
            reset_atlas_completion();
            return;
        }

        if (render_node_release_pending && render_snapshot != nullptr) {
            render_node_release_requeue_update = true;
        }

        if (render_update_pending) {
            if (render_update_window == render_window) {
                ++render_invalidation_stats.coalesced_requests;
                return;
            }

            render_update_pending = false;
            render_update_window = nullptr;
        }

        render_update_pending = true;
        render_update_window = render_window;
        ++render_invalidation_stats.scheduled_updates;
        if (surface.thread() == QThread::currentThread()) {
            surface.polish();
        }
        surface.update();
    }

    bool backend_callback_frame_target_live_visible(
        const VNM_TerminalSurface& surface) const
    {
        return
            session != nullptr                                  &&
            is_live_process_state(session->process_state())     &&
            surface.window() != nullptr                         &&
            surface.isVisible();
    }

    void request_backend_callback_frame_update_or_queue_posted_drain(
        VNM_TerminalSurface& surface)
    {
        Q_ASSERT(surface.thread() == QThread::currentThread());

        if (session == nullptr) {
            return;
        }
        if (!session->has_pending_backend_callback_events()) {
            stop_backend_callback_frame_progress_watchdog();
            return;
        }
        if (!backend_callback_frame_target_live_visible(surface)) {
            stop_backend_callback_frame_progress_watchdog();
            surface.queue_backend_callback_drain();
            return;
        }
        if (session->output_backpressure_active()) {
            // Once callback ingress has paused the backend, frame-paced slices
            // cannot safely retain ownership: a following accepted burst can
            // overlap the incomplete callback and reach the hard limit. The
            // existing coalesced posted pump exclusively owns pressured catch-up
            // until the queue falls below high water.
            stop_backend_callback_frame_progress_watchdog();
            surface.queue_backend_callback_drain();
            return;
        }
        if (backend_callback_frame_progress_deadline_expired()) {
            handle_backend_callback_frame_progress_watchdog_timeout(surface);
            return;
        }

        if (render_update_pending &&
            render_update_window == surface.window())
        {
            queue_backend_callback_frame_update_after_frame(surface);
            return;
        }

        request_render_update(surface);
        arm_backend_callback_frame_progress_watchdog();
    }

    void queue_backend_callback_frame_update_on_surface_thread(
        VNM_TerminalSurface& surface)
    {
        if (shutting_down.load()) {
            return;
        }

        bool expected = false;
        if (!backend_callback_frame_update_queued.compare_exchange_strong(
                expected,
                true))
        {
            return;
        }

        const bool queued = vnm::qt::post(
            &surface,
            [surface_ptr = QPointer<VNM_TerminalSurface>(&surface)] {
                if (surface_ptr == nullptr) {
                    return;
                }

                VNM_TerminalSurface& surface = *surface_ptr;
                auto& surface_private = *surface.m_private;
                surface_private.backend_callback_frame_update_queued.store(false);
                if (!surface_private.shutting_down.load()) {
                    surface_private.request_backend_callback_frame_update_or_queue_posted_drain(
                        surface);
                }
            }) == vnm::qt::Post_result::QUEUED;
        if (!queued) {
            backend_callback_frame_update_queued.store(false);
        }
    }

    void queue_backend_callback_frame_update_after_frame(
        VNM_TerminalSurface& surface)
    {
        if (shutting_down.load()) {
            return;
        }

        bool expected = false;
        if (!backend_callback_frame_update_queued.compare_exchange_strong(
                expected,
                true))
        {
            return;
        }
        const std::uint64_t after_frame_wait_generation =
            ++backend_callback_after_frame_wait_generation;
        const auto update_after_frame =
            [
                surface_ptr = QPointer<VNM_TerminalSurface>(&surface),
                after_frame_wait_generation
            ] {
                if (surface_ptr == nullptr) {
                    return;
                }

                VNM_TerminalSurface& surface = *surface_ptr;
                auto& surface_private = *surface.m_private;
                if (!surface_private.backend_callback_after_frame_wait_active ||
                    surface_private.backend_callback_after_frame_wait_generation !=
                        after_frame_wait_generation)
                {
                    ++surface_private.
                        stale_after_frame_callback_ignored_count_for_testing;
                    return;
                }

                surface_private.backend_callback_after_frame_wait_active = false;
                surface_private.backend_callback_after_frame_connection = {};
                surface_private.backend_callback_frame_update_queued.store(false);
                if (!surface_private.shutting_down.load()) {
                    if (surface_private.backend_callback_frame_progress_deadline_expired()) {
                        surface_private.
                            handle_backend_callback_frame_progress_watchdog_timeout(
                                surface);
                        return;
                    }
                    surface_private.request_backend_callback_frame_update_or_queue_posted_drain(
                        surface);
                }
            };

        if (QQuickWindow* render_window = surface.window(); render_window != nullptr) {
            backend_callback_after_frame_wait_active = true;
            backend_callback_after_frame_connection = QObject::connect(
                render_window,
                &QQuickWindow::afterFrameEnd,
                &surface,
                update_after_frame,
                static_cast<Qt::ConnectionType>(
                    Qt::QueuedConnection | Qt::SingleShotConnection));
            arm_backend_callback_frame_progress_watchdog();
            return;
        }

        backend_callback_after_frame_wait_active = true;
        const bool queued = vnm::qt::post(
            &surface,
            update_after_frame) == vnm::qt::Post_result::QUEUED;
        if (!queued) {
            clear_backend_callback_after_frame_wait();
            backend_callback_frame_update_queued.store(false);
        }
    }

    void schedule_backend_callback_frame_or_posted_drain(
        VNM_TerminalSurface& surface)
    {
        if (shutting_down.load()) {
            return;
        }

        queue_backend_callback_frame_update_on_surface_thread(surface);
    }

    void arm_backend_callback_frame_progress_watchdog()
    {
        // Qt may decline a requested frame, and afterFrameEnd can run without
        // this surface reaching updatePolish. Callback-epoch completion, not
        // partial snapshot publication, is the ownership progress that resets
        // this deadline.
        if (!backend_callback_frame_progress_deadline.has_value()) {
            restart_backend_callback_frame_progress_watchdog();
        }
    }

    void restart_backend_callback_frame_progress_watchdog()
    {
        backend_callback_frame_progress_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds{
                k_backend_callback_frame_progress_watchdog_ms};
        backend_callback_frame_progress_watchdog.start(
            k_backend_callback_frame_progress_watchdog_ms);
    }

    bool backend_callback_frame_progress_deadline_expired() const
    {
        return
            backend_callback_frame_progress_deadline.has_value() &&
            std::chrono::steady_clock::now() >=
                *backend_callback_frame_progress_deadline;
    }

    void clear_backend_callback_after_frame_wait()
    {
        if (!backend_callback_after_frame_wait_active) {
            return;
        }

        backend_callback_after_frame_wait_active = false;
        ++backend_callback_after_frame_wait_generation;
        QObject::disconnect(backend_callback_after_frame_connection);
        backend_callback_after_frame_connection = {};
    }

    void stop_backend_callback_frame_progress_watchdog()
    {
        backend_callback_frame_progress_watchdog.stop();
        backend_callback_frame_progress_deadline.reset();
        if (backend_callback_after_frame_wait_active) {
            clear_backend_callback_after_frame_wait();
            backend_callback_frame_update_queued.store(false);
        }
    }

    bool cancel_backend_callback_frame_update_wait()
    {
        const bool frame_update_queued =
            backend_callback_frame_update_queued.exchange(false);
        const bool watchdog_active =
            backend_callback_frame_progress_deadline.has_value();
        backend_callback_frame_progress_watchdog.stop();
        backend_callback_frame_progress_deadline.reset();
        clear_backend_callback_after_frame_wait();
        return frame_update_queued || watchdog_active;
    }

    void handle_backend_callback_frame_progress_watchdog_timeout(
        VNM_TerminalSurface& surface)
    {
        Q_ASSERT(surface.thread() == QThread::currentThread());

        if (!backend_callback_frame_progress_deadline.has_value()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < *backend_callback_frame_progress_deadline) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                *backend_callback_frame_progress_deadline - now);
            backend_callback_frame_progress_watchdog.start(
                static_cast<int>(std::max<std::int64_t>(
                    remaining.count(),
                    1)));
            return;
        }

        backend_callback_frame_progress_watchdog.stop();
        backend_callback_frame_progress_deadline.reset();

        if (backend_callback_after_frame_wait_active) {
            clear_backend_callback_after_frame_wait();
            backend_callback_frame_update_queued.store(false);
        }
        if (shutting_down.load() ||
            session == nullptr   ||
            !session->has_pending_backend_callback_events())
        {
            return;
        }

        ++backend_drain_stats.frame_progress_watchdog_firings;
        surface.queue_backend_callback_drain();
    }

    void cancel_backend_callback_frame_update_or_queue_posted_drain(
        VNM_TerminalSurface& surface)
    {
        if (!cancel_backend_callback_frame_update_wait()) {
            return;
        }

        if (shutting_down.load() ||
            session == nullptr   ||
            !session->has_pending_backend_callback_events())
        {
            return;
        }

        surface.queue_backend_callback_drain();
    }

    void reset_render_update_schedule()
    {
        render_update_pending = false;
        render_update_window = nullptr;
    }

    void reset_atlas_completion()
    {
        atlas_completion_pending                       = false;
        atlas_completion_complete_for_testing          = false;
        atlas_completion_snapshot_sequence             = 0U;
        atlas_completion_publication_generation        = 0U;
        atlas_completion_capture_sequence              = 0U;
        atlas_completion_report_generation_for_testing = 0U;
        atlas_completion_report_drew_for_testing       = false;
    }

    void publish_renderer_frame(
        std::uint64_t snapshot_sequence,
        std::uint64_t capture_sequence)
    {
        const std::lock_guard<std::mutex> lock(paint_completion_mutex);
        published_frame_snapshot_sequence = snapshot_sequence;
        published_frame_capture_sequence  = capture_sequence;
        published_frame_paint_completed   = false;
    }

    void request_render_node_release(VNM_TerminalSurface& surface)
    {
        render_node_release_pending = true;
        // Preserve requeue intent across repeated releaseResources() calls
        // before the release paint consumes the pending render update.
        render_node_release_requeue_update = render_node_release_requeue_update ||
            (render_update_pending                       &&
                render_update_window == surface.window() &&
                render_snapshot != nullptr);
        reset_render_update_schedule();
        reset_atlas_completion();
        if (!shutting_down.load() && surface.window() != nullptr) {
            surface.update();
        }
    }

    void consume_render_update(QQuickWindow* window)
    {
        if (!render_update_pending || render_update_window != window) {
            return;
        }

        render_update_pending = false;
        render_update_window = nullptr;
    }

    void wait_for_atlas_completion(
        VNM_TerminalSurface& surface,
        QQuickWindow*        window,
        std::uint64_t        snapshot_sequence,
        std::uint64_t        publication_generation,
        std::uint64_t        capture_sequence)
    {
        if (!render_update_pending || render_update_window != window) {
            return;
        }

        consume_render_update(window);
        atlas_completion_pending                = true;
        atlas_completion_snapshot_sequence      = snapshot_sequence;
        atlas_completion_publication_generation = publication_generation;
        atlas_completion_capture_sequence       = capture_sequence;

        const auto reconcile_after_frame =
            [surface_ptr = QPointer<VNM_TerminalSurface>(&surface)] {
                if (surface_ptr == nullptr) {
                    return;
                }

                VNM_TerminalSurface& surface = *surface_ptr;
                auto& surface_private = *surface.m_private;
                if (!surface_private.shutting_down.load()) {
                    surface_private.reconcile_atlas_completion(surface);
                }
            };
        (void)QObject::connect(
            window,
            &QQuickWindow::afterFrameEnd,
            &surface,
            reconcile_after_frame,
            static_cast<Qt::ConnectionType>(
                Qt::QueuedConnection | Qt::SingleShotConnection));
    }

    bool atlas_report_completes_pending_update(
        const term::Qsg_atlas_frame_report& report) const
    {
        return
            report.render_count             > 0U &&
            report.drew                          &&
            report.render_capture_sequence  >= atlas_completion_capture_sequence &&
            report.render_publication_generation >=
                atlas_completion_publication_generation;
    }

    void record_paint_completion(
        const term::Qsg_atlas_frame_report& report)
    {
        const std::lock_guard<std::mutex> lock(paint_completion_mutex);
        if (!published_frame_paint_completed                     &&
            published_frame_capture_sequence > 0U                &&
            report.render_count > 0U                              &&
            report.drew                                           &&
            report.render_capture_sequence >= published_frame_capture_sequence &&
            report.render_snapshot_sequence >= published_frame_snapshot_sequence)
        {
            published_frame_paint_completed = true;
            ++paint_completed_frame_count;
        }
    }

    std::uint64_t current_paint_completed_frame_count() const
    {
        const std::lock_guard<std::mutex> lock(paint_completion_mutex);
        return paint_completed_frame_count;
    }

    void complete_atlas_completion(const term::Qsg_atlas_frame_report& report)
    {
        record_paint_completion(report);
        atlas_completion_pending = false;
        ++render_invalidation_stats.consumed_updates;
        render_invalidation_stats.last_rendered_snapshot_sequence =
            report.render_snapshot_sequence;
        render_invalidation_stats.last_rendered_publication_generation =
            report.render_publication_generation;
        if (session != nullptr) {
            session->mark_render_publication_rendered(
                report.render_publication_generation);
        }
    }

    void reconcile_atlas_completion(VNM_TerminalSurface& surface)
    {
        if (!atlas_completion_pending || qsg_atlas_recorder == nullptr) {
            if (!atlas_completion_complete_for_testing) {
                return;
            }
        }

        if (atlas_completion_complete_for_testing) {
            term::Qsg_atlas_frame_report synthetic_report;
            synthetic_report.render_count = 1U;
            synthetic_report.render_capture_sequence = atlas_completion_capture_sequence;
            synthetic_report.render_snapshot_sequence = atlas_completion_snapshot_sequence;
            synthetic_report.render_publication_generation =
                atlas_completion_report_generation_for_testing;
            synthetic_report.drew = atlas_completion_report_drew_for_testing;
            if (!atlas_report_completes_pending_update(synthetic_report)) {
                return;
            }
            atlas_completion_complete_for_testing = false;
            complete_atlas_completion(synthetic_report);
            return;
        }

        const term::Qsg_atlas_frame_report atlas_report =
            qsg_atlas_recorder->snapshot();
        if (!atlas_report_completes_pending_update(atlas_report)) {
            return;
        }

        complete_atlas_completion(atlas_report);
    }

    term::Terminal_surface_render_invalidation_stats_t current_invalidation_stats(
        VNM_TerminalSurface& surface)
    {
        reconcile_atlas_completion(surface);
        term::Terminal_surface_render_invalidation_stats_t stats = render_invalidation_stats;
        stats.render_snapshot_callback_epoch =
            render_snapshot != nullptr
                ? render_snapshot->metadata.processed_backend_callback_epoch
                : 0U;
        stats.pending_update = frame_work_pending(surface);
        return stats;
    }

    bool frame_work_pending(VNM_TerminalSurface& surface)
    {
        reconcile_atlas_completion(surface);
        return render_update_pending || atlas_completion_pending;
    }

    QRectF input_method_cursor_rectangle(const VNM_TerminalSurface& surface) const
    {
        const qreal cell_width = term::is_valid_cell_metrics(cell_metrics)
            ? cell_metrics.width
            : 1.0;
        const qreal cell_height = term::is_valid_cell_metrics(cell_metrics)
            ? cell_metrics.height
            : 1.0;

        int row = 0;
        int column = 0;
        if (render_snapshot != nullptr && term::is_valid_grid_size(render_snapshot->grid_size)) {
            row    = std::clamp(
                render_snapshot->cursor.position.row,
                0,
                render_snapshot->grid_size.rows - 1);
            column = std::clamp(
                render_snapshot->cursor.position.column,
                0,
                render_snapshot->grid_size.columns - 1);
            if (ime_preedit.active) {
                column = std::clamp(
                    column + display_width_for_preedit_cursor(ime_preedit.text, ime_preedit.cursor_position),
                    0,
                    render_snapshot->grid_size.columns - 1);
            }
        }

        QRectF rectangle(
            static_cast<qreal>(column) *   cell_width,
            static_cast<qreal>(row) *      cell_height,
            std::max<qreal>(1.0, cell_width),
            std::max<qreal>(1.0, cell_height));
        const QRectF bounds = surface.boundingRect();
        if (bounds.width() > 0.0) {
            rectangle.moveLeft(std::clamp(
                rectangle.left(),
                bounds.left(),
                std::max(bounds.left(), bounds.right() - rectangle.width())));
        }
        if (bounds.height() > 0.0) {
            rectangle.moveTop(std::clamp(
                rectangle.top(),
                bounds.top(),
                std::max(bounds.top(), bounds.bottom() - rectangle.height())));
        }
        return rectangle;
    }

    void set_ime_preedit_state(
        VNM_TerminalSurface&       surface,
        term::Ime_preedit_state    state)
    {
        if (term::same_ime_preedit_state(ime_preedit, state)) {
            return;
        }

        ime_preedit = std::move(state);
        surface.updateInputMethod(Qt::ImCursorRectangle);
        request_render_update(surface);
    }

    void clear_mouse_reporting_state()
    {
        mouse_reporting_pressed_buttons = Qt::NoButton;
        mouse_reporting_drag_button = term::Terminal_mouse_button::NONE;
        mouse_reporting_last_position.reset();
    }

    struct Pending_published_mouse_report
    {
        term::Terminal_session*         session = nullptr;
        std::uint64_t                   session_generation = 0U;
        term::Terminal_mouse_event_kind kind =
            term::Terminal_mouse_event_kind::PRESS;
        QByteArray                      bytes;
        term::terminal_grid_position_t  position;
    };

    bool pending_report_session_is_active(
        const Pending_published_mouse_report& report) const
    {
        return
            session != nullptr                         &&
            session.get() == report.session             &&
            session_generation == report.session_generation;
    }

    bool active_session_matches(
        const term::Terminal_session*   candidate,
        std::uint64_t                   candidate_generation) const
    {
        return
            session.get() == candidate &&
            session_generation == candidate_generation;
    }

    Published_mouse_report_write_result encode_published_mouse_report(
        term::Terminal_mouse_event_kind                              kind,
        term::Terminal_mouse_button                                  button,
        term::terminal_grid_position_t                               position,
        Qt::KeyboardModifiers                                        modifiers,
        const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot) const
    {
        if (snapshot == nullptr) {
            return {};
        }

        const term::Terminal_mouse_event mouse_event{
            kind,
            button,
            position.row,
            position.column,
            modifiers,
        };
        const QByteArray bytes = term::encode_terminal_mouse_event(
            mouse_event,
            input_modes_from_render_snapshot(*snapshot));
        if (bytes.isEmpty()) {
            return {};
        }

        return {std::nullopt, position, bytes, true};
    }

    Pending_published_mouse_report pending_published_mouse_report(
        term::Terminal_mouse_event_kind               kind,
        const Published_mouse_report_write_result&    encoded) const
    {
        Q_ASSERT(session != nullptr);
        Q_ASSERT(encoded.encoded);
        Q_ASSERT(encoded.position.has_value());
        return {
            session.get(),
            session_generation,
            kind,
            encoded.bytes,
            *encoded.position,
        };
    }

    bool force_pending_published_mouse_report_block_for_testing()
    {
        if (pending_published_mouse_report_block_count_for_testing <= 0) {
            return false;
        }

        --pending_published_mouse_report_block_count_for_testing;
        return true;
    }

    Published_mouse_report_attempt try_write_pending_published_mouse_report(
        const Pending_published_mouse_report& report)
    {
        if (!pending_report_session_is_active(report)) {
            return {
                Published_mouse_report_attempt_status::SESSION_INACTIVE,
                std::nullopt,
                report.position,
            };
        }
        if (report.bytes.isEmpty()) {
            return {
                Published_mouse_report_attempt_status::NOT_ENCODED,
                std::nullopt,
                report.position,
            };
        }

        std::optional<term::Terminal_session_result> result =
            report.session->try_write_user_bytes_without_backend_drain_if_callbacks_empty(
                report.bytes);
        if (!result.has_value()) {
            return {
                Published_mouse_report_attempt_status::CALLBACKS_PENDING,
                std::nullopt,
                report.position,
            };
        }
        if (mouse_report_write_is_non_writable_noop(*result)) {
            return {
                Published_mouse_report_attempt_status::NON_WRITABLE,
                std::move(result),
                report.position,
            };
        }
        return {
            Published_mouse_report_attempt_status::WRITTEN,
            std::move(result),
            report.position,
        };
    }

    void handle_pending_published_mouse_report_invalidated(
        const Pending_published_mouse_report& report)
    {
        if (report.kind == term::Terminal_mouse_event_kind::PRESS) {
            clear_mouse_reporting_state();
        }
    }

    bool retry_pending_published_mouse_reports(
        VNM_TerminalSurface&                   surface,
        bool                                   drain_once,
        Backend_callback_incomplete_follow_up  follow_up =
            Backend_callback_incomplete_follow_up::POSTED_DRAIN)
    {
        if (retrying_pending_published_mouse_reports) {
            return pending_published_mouse_reports.empty();
        }

        retrying_pending_published_mouse_reports = true;
        if (!pending_published_mouse_reports.empty() &&
            force_pending_published_mouse_report_block_for_testing())
        {
            retrying_pending_published_mouse_reports = false;
            return false;
        }

        while (!pending_published_mouse_reports.empty()) {
            const Pending_published_mouse_report report =
                pending_published_mouse_reports.front();
            const Published_mouse_report_attempt attempt =
                try_write_pending_published_mouse_report(report);
            if (attempt.status ==
                Published_mouse_report_attempt_status::CALLBACKS_PENDING)
            {
                if (drain_once) {
                    drain_once = false;
                    surface.drain_backend_callback_events();
                    continue;
                }

                if (follow_up == Backend_callback_incomplete_follow_up::FRAME_UPDATE) {
                    request_backend_callback_frame_update_or_queue_posted_drain(
                        surface);
                }
                else {
                    surface.queue_backend_callback_drain();
                }
                retrying_pending_published_mouse_reports = false;
                return false;
            }

            pending_published_mouse_reports.pop_front();
            if (attempt.status == Published_mouse_report_attempt_status::WRITTEN) {
                surface.sync_from_session();
                if (attempt.result.has_value()) {
                    if (!is_accepted(attempt.result->code)) {
                        retrying_pending_published_mouse_reports = false;
                        surface.report_result_failure(*attempt.result);
                        return false;
                    }
                }
            }
            else {
                handle_pending_published_mouse_report_invalidated(report);
            }
        }

        retrying_pending_published_mouse_reports = false;
        return true;
    }

    bool block_terminal_input_behind_pending_published_mouse_report(
        VNM_TerminalSurface& surface)
    {
        if (pending_published_mouse_reports.empty()) {
            return false;
        }

        return !retry_pending_published_mouse_reports(surface, true);
    }

    void resolve_pending_published_mouse_reports_before_terminal_input(
        VNM_TerminalSurface& surface)
    {
        if (pending_published_mouse_reports.empty()) {
            return;
        }

        if (pending_published_mouse_reports.size() == 1U &&
            retry_pending_published_mouse_reports(surface, true))
        {
            return;
        }

        // Non-mouse input must not publish only a prefix of a mouse-report
        // queue. Multi-report queues are cancelled as a unit before the newer
        // terminal input proceeds.
        clear_pending_published_mouse_reports();
    }

    void clear_pending_published_mouse_reports()
    {
        for (const Pending_published_mouse_report& report :
            pending_published_mouse_reports)
        {
            handle_pending_published_mouse_report_invalidated(report);
        }
        pending_published_mouse_reports.clear();
    }

    void clear_selection_drag_state()
    {
        if (session != nullptr) {
            session->clear_selection_drag_provenance();
        }
        selection_anchor.reset();
        selection_drag_press_provenance.reset();
        selection_drag_active    = false;
        selection_drag_moved     = false;
        selection_drag_cancelled = false;
    }

    void clear_hyperlink_activation_state()
    {
        hyperlink_activation_target.reset();
        hyperlink_activation_gesture_active = false;
    }

    bool has_copyable_selection_attachment() const
    {
        if (session == nullptr) {
            return false;
        }

        if (session->selection_visual_lease().has_value()) {
            return true;
        }

        return render_snapshot != nullptr && !render_snapshot->selection_spans.empty();
    }

    void clear_mouse_wheel_remainders()
    {
        wheel_mouse_angle_remainder = 0.0;
        wheel_mouse_pixel_remainder = 0.0;
    }

    void clear_zoom_wheel_remainders()
    {
        wheel_zoom_angle_remainder = 0.0;
        wheel_zoom_pixel_remainder = 0.0;
    }

    void clear_wheel_remainders()
    {
        wheel_scroll_angle_remainder = 0.0;
        wheel_scroll_pixel_remainder = 0.0;
        clear_zoom_wheel_remainders();
        clear_mouse_wheel_remainders();
    }

    void warm_prompt_text_layouts_for_render_font()
    {
        const QString font_key = render_font.toString();
        if (font_key == warmed_prompt_text_layout_font_key) {
            return;
        }

        warm_prompt_glyph_text_layouts(render_font);
        warmed_prompt_text_layout_font_key = font_key;
    }

    std::chrono::steady_clock::duration backend_callback_frame_catchup_budget() const
    {
        return backend_callback_frame_catchup_budget_override.value_or(
            configured_backend_callback_frame_catchup_budget());
    }

    void clear_selection_with_sync(VNM_TerminalSurface& surface)
    {
        if (session == nullptr) {
            return;
        }

        session->clear_selection();
        surface.sync_from_session();
    }

    void detach_selection_visual_attachment_with_sync(VNM_TerminalSurface& surface)
    {
        if (session == nullptr) {
            return;
        }

        session->detach_selection_visual_attachment();
        surface.sync_from_session();
    }

    void play_audible_bell()
    {
        if (audible_bell_handler) {
            audible_bell_handler();
            return;
        }

        play_platform_bell();
    }

    term::Qt_grid_metrics_provider                         grid_metrics_provider;
    term::terminal_cell_metrics_t                          cell_metrics;
    QFont                                                  render_font;
    qreal                                                  render_device_pixel_ratio             = 1.0;
    std::shared_ptr<const term::Terminal_render_snapshot>  render_snapshot;
    term::Ime_preedit_state                                ime_preedit;
    bool                                                   cursor_blink_visible                  = true;
    std::shared_ptr<term::Qsg_atlas_recorder>              qsg_atlas_recorder;
    mutable std::mutex                                     paint_completion_mutex;
    std::uint64_t                                          paint_completed_frame_count = 0U;
    std::uint64_t                                          published_frame_snapshot_sequence = 0U;
    std::uint64_t                                          published_frame_capture_sequence = 0U;
    bool                                                   published_frame_paint_completed = false;
#if VNM_TERMINAL_PROFILING_ENABLED
    mutable std::mutex                                     render_profiler_mutex;
    std::shared_ptr<term::Hierarchical_profiler>           render_profiler;
    mutable term::Render_profile_snapshot_t                 render_profiler_snapshot;
    std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
                                                           slow_text_layout_recorder;
    std::atomic_bool                                       render_profiler_enabled               = false;
#endif
    mutable std::mutex                                     renderer_lifecycle_recorder_mutex;
    std::shared_ptr<Renderer_lifecycle_recorder>           renderer_lifecycle_recorder;
    std::atomic_bool                                  renderer_lifecycle_recorder_enabled =
                                                           false;

    term::Terminal_surface_render_invalidation_stats_t     render_invalidation_stats;
    term::Terminal_surface_backend_drain_stats_t           backend_drain_stats;
    std::optional<std::chrono::steady_clock::duration>     backend_callback_frame_catchup_budget_override;
    int                                                    pending_published_mouse_report_block_count_for_testing = 0;
    std::function<void()>                                  audible_bell_handler;
    bool                                                   render_update_pending              = false;
    std::atomic_bool                                       backend_callback_frame_update_queued =
                                                           false;
    QTimer                                                 backend_callback_frame_progress_watchdog;
    std::optional<std::chrono::steady_clock::time_point>
                                                           backend_callback_frame_progress_deadline;
    bool                                                   backend_callback_after_frame_wait_active = false;
    std::uint64_t                                          backend_callback_after_frame_wait_generation = 0U;
    QMetaObject::Connection                                backend_callback_after_frame_connection;
    std::uint64_t                                          stale_after_frame_callback_ignored_count_for_testing = 0U;
    std::function<void()>
        before_backend_callback_frame_owner_release_handler_for_testing;
    bool                                                   render_node_release_pending        = false;
    bool                                                   render_node_release_requeue_update = false;
    bool                                                   atlas_completion_pending           = false;
    bool                                                   atlas_completion_complete_for_testing = false;
    bool                                                   qsg_atlas_render_node_live         = false;
    std::uint64_t                                          qsg_atlas_capture_sequence         = 0U;
    std::uint64_t                                          qsg_atlas_font_epoch               = 0U;
    std::uint64_t                                          atlas_completion_snapshot_sequence = 0U;
    std::uint64_t                                          atlas_completion_publication_generation = 0U;
    std::uint64_t                                          atlas_completion_capture_sequence  = 0U;
    std::uint64_t                                          atlas_completion_report_generation_for_testing = 0U;
    bool                                                   atlas_completion_report_drew_for_testing = false;
    QString                                                qsg_atlas_font_epoch_key;
    qreal                                                  wheel_scroll_angle_remainder       = 0.0;
    qreal                                                  wheel_scroll_pixel_remainder       = 0.0;
    qreal                                                  wheel_zoom_angle_remainder         = 0.0;
    qreal                                                  wheel_zoom_pixel_remainder         = 0.0;
    qreal                                                  wheel_mouse_angle_remainder        = 0.0;
    qreal                                                  wheel_mouse_pixel_remainder        = 0.0;
    Qt::MouseButtons                                  mouse_reporting_pressed_buttons =
        Qt::NoButton;
    term::Terminal_mouse_button                       mouse_reporting_drag_button =
        term::Terminal_mouse_button::NONE;
    std::optional<term::terminal_grid_position_t>          mouse_reporting_last_position;
    std::deque<Pending_published_mouse_report>             pending_published_mouse_reports;
    bool                                                   retrying_pending_published_mouse_reports = false;
    std::optional<Surface_hyperlink_target>                 hyperlink_activation_target;
    std::optional<QPointF>                                 hyperlink_hover_position;
    bool                                                   hyperlink_activation_gesture_active = false;
    bool                                                   hyperlink_cursor_active             = false;
    std::optional<term::terminal_grid_position_t>          selection_anchor;
    std::optional<term::terminal_selection_drag_press_provenance_t>
                                                           selection_drag_press_provenance;
    std::optional<term::Terminal_osc52_write_request>      pending_clipboard_write;
    std::optional<quint64>                                 pending_text_area_resize_arbitration;
    // Raised for the length of one notification delivery loop. A host is
    // allowed to answer a notification from inside the handler it was delivered
    // to, and every answer re-enters sync_from_session through the session call
    // it makes, so without this the delivery would nest once per answer.
    bool                                                   delivering_session_notifications    = false;
    std::function<std::optional<QString>()>                clipboard_text_reader;
    QString                                                warmed_prompt_text_layout_font_key;
    QTimer                                                 synchronized_output_recovery_timer;
    QTimer                                                 text_area_resize_arbitration_timer;
    QTimer                                                 row_timestamp_tooltip_timer;
    QTimer                                                 msdf_availability_completion_timer;
    std::shared_ptr<Msdf_availability_completion>          msdf_availability_completion;
    int                                                    msdf_availability_completion_polls_remaining = 0;
    std::optional<QPointF>                                 row_timestamp_tooltip_pointer_position;
    bool                                                   row_timestamp_tooltip_request_active  = false;
    bool                                                   selection_drag_active                 = false;
    bool                                                   selection_drag_moved                  = false;
    bool                                                   selection_drag_cancelled              = false;
    bool                                                   last_sgr_mouse_reporting_active       = false;
    bool                                                   last_alternate_scroll_active          = false;
    std::uint64_t                                          last_alternate_scroll_mode_generation = 0U;
    QPointer<QQuickWindow>                                 render_update_window;
    std::uint64_t                                          window_binding_generation             = 0U;
    QMetaObject::Connection                                window_screen_changed_connection;
    QMetaObject::Connection                                window_scene_graph_invalidated_connection;
    QMetaObject::Connection                                screen_dpi_changed_connection;
    QMetaObject::Connection                                screen_physical_dpi_changed_connection;
    QPointer<QQuickWindow>                                 bound_window;
    std::unique_ptr<term::Terminal_session>                session;
    std::uint64_t                                          session_generation = 0U;
    std::unique_ptr<term::Terminal_resize_controller>      resize_controller;
    std::shared_ptr<term::Terminal_transcript_recorder>    transcript_recorder;
    std::uint64_t                                          last_installed_render_publication_generation = 0U;
    std::uint64_t                                          last_ime_preedit_generation           = 0U;
    std::uint64_t                                          last_backend_error_signal_sequence    = 0U;
    std::uint64_t                                          next_clipboard_write_request_id       = 1U;
    bool                                                   dirty_row_stats_enabled               = false;
    // These atomics are read by backend callback threads through the notifier.
    // reset_session() must close the session before Private storage is destroyed.
    std::atomic_bool                                       session_drain_queued                  = false;
    std::atomic_bool                                       backend_callback_pressure_drain_requested =
                                                           false;
    std::atomic_bool                                       shutting_down                         = false;
};

QString VNM_TerminalSurface::scroll_noop_cause_name(Scroll_noop_cause cause)
{
    switch (cause) {
        case Scroll_noop_cause::NONE:                          return QString();
        case Scroll_noop_cause::ZERO_LINE_DELTA:               return QStringLiteral("zero_line_delta");
        case Scroll_noop_cause::NO_SESSION:                    return QStringLiteral("no_session");
        case Scroll_noop_cause::SYNCHRONIZED_OUTPUT_DEFERRED:  return QStringLiteral("synchronized_output_deferred");
        case Scroll_noop_cause::SYNCHRONIZED_OUTPUT_PUBLISHED: return QStringLiteral("synchronized_output_published");
        case Scroll_noop_cause::ALTERNATE_SCREEN:              return QStringLiteral("alternate_screen");
        case Scroll_noop_cause::BOUNDARY_OR_CLAMP:             return QStringLiteral("boundary_or_clamp");
        case Scroll_noop_cause::NO_PUBLICATION:                return QStringLiteral("no_publication");
    }

    return QString();
}

QString VNM_TerminalSurface::scroll_action_name(Scroll_action action)
{
    switch (action) {
        case Scroll_action::NONE:                     return QString();
        case Scroll_action::VIEWPORT_MOVED:           return QStringLiteral("viewport_moved");
        case Scroll_action::AT_BOUNDARY:              return QStringLiteral("at_boundary");
        case Scroll_action::DEFERRED_INTENT_RECORDED: return QStringLiteral("deferred_intent_recorded");
        case Scroll_action::TERMINAL_INPUT:           return QStringLiteral("terminal_input");
    }

    return QString();
}

VNM_TerminalSurface::VNM_TerminalSurface(QQuickItem* parent)
:
    QQuickItem(parent),
    m_private(std::make_unique<Private>())
{
    m_font_family = term::vnm_terminal_default_monospace_font_family();
    m_font_size = term::k_vnm_terminal_default_font_pixel_size;
    m_retained_history_capacity_bytes = default_retained_history_capacity_bytes();
    // The header default claims MSDF optimistically; start from the compiled
    // truth so a build without the MSDF renderer never reports it available.
    // The first set_font_family() refines this with the per-font async check.
    m_msdf_text_available = term::k_qsg_atlas_msdf_text_renderer_compiled;
    setFlag(ItemHasContents,        true);
    setFlag(ItemAcceptsInputMethod, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFocus(true);
    m_private->backend_callback_frame_progress_watchdog.setSingleShot(true);
    m_private->backend_callback_frame_progress_watchdog.setInterval(
        k_backend_callback_frame_progress_watchdog_ms);
    QObject::connect(
        &m_private->backend_callback_frame_progress_watchdog,
        &QTimer::timeout,
        this,
        [this] {
            m_private->handle_backend_callback_frame_progress_watchdog_timeout(
                *this);
        });
    m_private->synchronized_output_recovery_timer.setSingleShot(true);
    QObject::connect(
        &m_private->synchronized_output_recovery_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_synchronized_output_recovery_timeout();
        });
    m_private->text_area_resize_arbitration_timer.setSingleShot(true);
    QObject::connect(
        &m_private->text_area_resize_arbitration_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_text_area_resize_arbitration_timeout();
        });
    m_private->row_timestamp_tooltip_timer.setSingleShot(true);
    m_private->row_timestamp_tooltip_timer.setInterval(k_row_timestamp_tooltip_delay_ms);
    QObject::connect(
        &m_private->row_timestamp_tooltip_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_row_timestamp_tooltip_timeout();
        });
    m_private->msdf_availability_completion_timer.setInterval(
        k_msdf_availability_completion_poll_ms);
    QObject::connect(
        &m_private->msdf_availability_completion_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_msdf_availability_completion_timeout();
        });
    refresh_grid_metrics();
}

VNM_TerminalSurface::~VNM_TerminalSurface()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_interaction_diagnostics_enabled) {
        set_interaction_diagnostics_enabled(false);
    }

    QObject::disconnect(m_private->window_scene_graph_invalidated_connection);
    QObject::disconnect(m_private->window_screen_changed_connection);
    QObject::disconnect(m_private->screen_dpi_changed_connection);
    QObject::disconnect(m_private->screen_physical_dpi_changed_connection);
    m_private->shutting_down.store(true);
    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_item_destruction();
    }
    releaseResources();
    if (m_private->session != nullptr &&
        is_live_process_state(m_private->session->process_state()))
    {
        (void)m_private->session->terminate();
    }
    reset_session();
}

void VNM_TerminalSurface::releaseResources()
{
    Q_ASSERT(thread() == QThread::currentThread());

    const auto lifecycle_recorder = m_private->lifecycle_recorder();
    if (lifecycle_recorder != nullptr) {
        lifecycle_recorder->record_release_resources();
    }
    if (m_private->qsg_atlas_render_node_live) {
        if (lifecycle_recorder != nullptr) {
            lifecycle_recorder->record_root_node_destroyed();
            lifecycle_recorder->record_render_node_deleted();
        }
        m_private->qsg_atlas_render_node_live = false;
    }
    m_private->request_render_node_release(*this);
    QQuickItem::releaseResources();
}

QString VNM_TerminalSurface::font_family() const
{
    return m_font_family;
}

void VNM_TerminalSurface::set_font_family(const QString& font_family)
{
    if (m_font_family == font_family) {
        return;
    }

    m_font_family = font_family;
    emit font_family_changed();
    refresh_grid_metrics();
    start_msdf_availability_check();
}

bool VNM_TerminalSurface::msdf_text_available() const
{
    return m_msdf_text_available;
}

bool VNM_TerminalSurface::msdf_text_checking() const
{
    return m_msdf_text_checking;
}

void VNM_TerminalSurface::start_msdf_availability_check()
{
    // Bake the font's MSDF atlas on a worker thread to learn, truthfully,
    // whether MSDF can render this font. Coverage is size-independent, so the
    // family alone determines availability. A generation counter discards stale
    // results when the font changes again before a check finishes.
    const QFont font(m_font_family);
    const unsigned long long generation = ++m_msdf_availability_generation;

    if (!m_msdf_text_checking) {
        m_msdf_text_checking = true;
        emit msdf_text_checking_changed();
    }

    auto completion = std::make_shared<Msdf_availability_completion>();
    completion->generation = generation;
    m_private->msdf_availability_completion = completion;
    m_private->msdf_availability_completion_polls_remaining =
        k_msdf_availability_completion_timeout_ms /
        k_msdf_availability_completion_poll_ms;
    m_private->msdf_availability_completion_timer.start();

    QPointer<VNM_TerminalSurface> self = this;
    QThreadPool::globalInstance()->start([self, font, generation, completion]() {
        const bool available = term::qsg_atlas_msdf_text_available_for_font(font);
        const auto post_result = vnm::qt::post(
            qApp,
            [self, available, generation]() {
                if (self) {
                    self->apply_msdf_availability_result(available, generation);
                }
            });
        completion->dispatch_result.store(
            static_cast<int>(post_result),
            std::memory_order_release);
    });
}

void VNM_TerminalSurface::handle_msdf_availability_completion_timeout()
{
    const std::shared_ptr<Msdf_availability_completion> completion =
        m_private->msdf_availability_completion;
    if (completion == nullptr ||
        completion->generation != m_msdf_availability_generation)
    {
        m_private->msdf_availability_completion_timer.stop();
        m_private->msdf_availability_completion.reset();
        return;
    }

    const int dispatch_result =
        completion->dispatch_result.load(std::memory_order_acquire);
    const bool dispatch_rejected =
        dispatch_result != Msdf_availability_completion::k_dispatch_pending &&
        dispatch_result != static_cast<int>(vnm::qt::Post_result::QUEUED);
    const bool completion_timed_out =
        --m_private->msdf_availability_completion_polls_remaining <= 0;
    if (!dispatch_rejected && !completion_timed_out) {
        return;
    }

    m_private->msdf_availability_completion_timer.stop();
    m_private->msdf_availability_completion.reset();
    if (m_msdf_text_checking) {
        m_msdf_text_checking = false;
        emit msdf_text_checking_changed();
    }

    if (dispatch_rejected) {
        qWarning(
            "VNM_TerminalSurface: MSDF availability dispatch admission "
            "failed (result %d).",
            dispatch_result);
    }
    else {
        qWarning(
            "VNM_TerminalSurface: MSDF availability completion timed out.");
    }
}

void VNM_TerminalSurface::apply_msdf_availability_result(
    bool available, unsigned long long generation)
{
    if (generation != m_msdf_availability_generation) {
        return; // superseded by a newer font change
    }

    m_private->msdf_availability_completion_timer.stop();
    m_private->msdf_availability_completion.reset();
    if (m_msdf_text_checking) {
        m_msdf_text_checking = false;
        emit msdf_text_checking_changed();
    }
    if (m_msdf_text_available != available) {
        m_msdf_text_available = available;
        emit msdf_text_available_changed();
    }
}

qreal VNM_TerminalSurface::font_size() const
{
    return m_font_size;
}

void VNM_TerminalSurface::set_font_size(qreal font_size)
{
    const qreal normalized_font_size = normalized_font_pixel_size(font_size);
    if (same_property_value(m_font_size, normalized_font_size)) {
        return;
    }

    m_font_size = normalized_font_size;
    m_private->clear_zoom_wheel_remainders();
    emit font_size_changed();
    refresh_grid_metrics();
}

QString VNM_TerminalSurface::color_scheme() const
{
    return m_color_scheme;
}

void VNM_TerminalSurface::set_color_scheme(const QString& color_scheme)
{
    const term::Terminal_color_scheme* scheme = term::find_color_scheme(color_scheme);
    if (scheme == nullptr) {
        qWarning(
            "VNM_TerminalSurface: ignoring unknown color scheme \"%s\"",
            qPrintable(color_scheme));
        return;
    }
    if (m_color_scheme == scheme->name) {
        return;
    }

    m_color_scheme = scheme->name;
    emit color_scheme_changed();
    if (m_private->session != nullptr) {
        m_private->session->set_color_state(
            term::make_terminal_color_state(*scheme));
        sync_from_session();
    }
    m_private->request_render_update(*this);
}

QStringList VNM_TerminalSurface::available_color_schemes() const
{
    const std::vector<term::Terminal_color_scheme>& schemes = term::builtin_color_schemes();
    QStringList names;
    names.reserve(static_cast<int>(schemes.size()));
    for (const term::Terminal_color_scheme& scheme : schemes) {
        names.push_back(scheme.name);
    }
    return names;
}

QVariantMap VNM_TerminalSurface::color_scheme_preview(const QString& color_scheme) const
{
    const term::Terminal_color_scheme* scheme = term::find_color_scheme(color_scheme);
    if (scheme == nullptr) {
        return {};
    }

    QVariantList ansi;
    ansi.reserve(static_cast<int>(scheme->ansi_palette_rgba.size()));
    for (quint32 rgba : scheme->ansi_palette_rgba) {
        ansi.push_back(QColor::fromRgba(rgba));
    }

    QVariantMap preview;
    preview.insert(QStringLiteral("name"),       scheme->name);
    preview.insert(QStringLiteral("background"), QColor::fromRgba(scheme->background_rgba));
    preview.insert(QStringLiteral("foreground"), QColor::fromRgba(scheme->foreground_rgba));
    preview.insert(QStringLiteral("cursor"),     QColor::fromRgba(scheme->cursor_rgba));
    preview.insert(QStringLiteral("selection"),  QColor::fromRgba(scheme->selection_rgba));
    preview.insert(QStringLiteral("ansi"),       ansi);
    return preview;
}

VNM_TerminalSurface::Cursor_style VNM_TerminalSurface::cursor_style() const
{
    return m_cursor_style;
}

void VNM_TerminalSurface::set_cursor_style(Cursor_style cursor_style)
{
    if (m_cursor_style == cursor_style) {
        return;
    }

    m_cursor_style = cursor_style;
    emit cursor_style_changed();
    m_private->request_render_update(*this);
}

bool VNM_TerminalSurface::cursor_blink_enabled() const
{
    return m_cursor_blink_enabled;
}

void VNM_TerminalSurface::set_cursor_blink_enabled(bool enabled)
{
    if (m_cursor_blink_enabled == enabled) {
        return;
    }

    m_cursor_blink_enabled = enabled;
    emit cursor_blink_enabled_changed();
    m_private->request_render_update(*this);
}

int VNM_TerminalSurface::scrollback_limit() const
{
    return m_scrollback_limit;
}

void VNM_TerminalSurface::set_scrollback_limit(int limit)
{
    const int bounded_limit = std::max(0, limit);

    if (m_scrollback_limit == bounded_limit) {
        return;
    }

    m_scrollback_limit = bounded_limit;
    emit scrollback_limit_changed();
    if (m_private->session != nullptr) {
        m_private->session->set_scrollback_limit(m_scrollback_limit);
        sync_from_session();
    }
}

std::size_t VNM_TerminalSurface::default_retained_history_capacity_bytes()
{
    return term::k_terminal_default_retained_history_capacity_bytes;
}

std::size_t VNM_TerminalSurface::minimum_retained_history_capacity_bytes()
{
    return term::k_terminal_min_retained_history_capacity_bytes;
}

std::size_t VNM_TerminalSurface::maximum_retained_history_capacity_bytes()
{
    return term::k_terminal_max_retained_history_capacity_bytes;
}

std::size_t VNM_TerminalSurface::retained_history_capacity_bytes() const
{
    return m_retained_history_capacity_bytes;
}

void VNM_TerminalSurface::set_retained_history_capacity_bytes(
    std::size_t capacity_bytes)
{
    if (capacity_bytes < minimum_retained_history_capacity_bytes() ||
        capacity_bytes > maximum_retained_history_capacity_bytes())
    {
        qWarning(
            "retained-history capacity must be between %zu and %zu bytes",
            minimum_retained_history_capacity_bytes(),
            maximum_retained_history_capacity_bytes());
        return;
    }

    if (m_private->session != nullptr) {
        qWarning("retained-history capacity must be set before starting a session");
        return;
    }

    m_retained_history_capacity_bytes =
        term::terminal_history_ring_aligned_capacity(capacity_bytes);
}

bool VNM_TerminalSurface::interaction_diagnostics_enabled() const
{
    return m_interaction_diagnostics_enabled;
}

void VNM_TerminalSurface::set_interaction_diagnostics_enabled(bool enabled)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (enabled == m_interaction_diagnostics_enabled) {
        return;
    }

    QPointer<VNM_TerminalSurface>& owner = interaction_trace_owner();
    if (enabled && owner != nullptr && owner != this) {
        m_interaction_diagnostics_error =
            QStringLiteral("interaction diagnostics are already enabled by another terminal surface");
        emit interaction_diagnostics_error_changed();
        qWarning().noquote() << m_interaction_diagnostics_error;
        return;
    }

    QString error;
    if (enabled) {
        const QPointer<VNM_TerminalSurface> surface(this);
        term::set_interaction_trace_failure_handler(
            [surface](QString failure) {
                const auto post_result = vnm::qt::post(
                    QGuiApplication::instance(),
                    [surface, failure = std::move(failure)] {
                        if (surface == nullptr                         ||
                            interaction_trace_owner() != surface      ||
                            !surface->m_interaction_diagnostics_enabled)
                        {
                            return;
                        }
                        interaction_trace_owner().clear();
                        term::set_interaction_trace_failure_handler({});
                        surface->m_interaction_diagnostics_enabled = false;
                        surface->m_interaction_diagnostics_error = failure;
                        emit surface->interaction_diagnostics_error_changed();
                        emit surface->interaction_diagnostics_enabled_changed();
                    });
                if (post_result != vnm::qt::Post_result::QUEUED) {
                    qWarning(
                        "VNM_TerminalSurface: interaction-trace failure "
                        "dispatch admission failed (result %d).",
                        static_cast<int>(post_result));
                }
            });
    }
    if (!term::set_interaction_trace_enabled(enabled, &error)) {
        if (enabled) {
            term::set_interaction_trace_failure_handler({});
        }
        m_interaction_diagnostics_error = error;
        emit interaction_diagnostics_error_changed();
        qWarning().noquote() << error;
        return;
    }

    if (enabled) {
        owner = this;
    }
    else
    if (owner == this) {
        owner.clear();
        term::set_interaction_trace_failure_handler({});
    }

    m_interaction_diagnostics_error.clear();
    m_interaction_diagnostics_enabled = enabled;
    emit interaction_diagnostics_error_changed();
    emit interaction_diagnostics_enabled_changed();
    if (enabled) {
        term::record_interaction_trace(
            "trace",
            "enabled",
            QStringLiteral(
                "path=%1 trace_capacity_bytes=%2 retained_history_capacity_bytes=%3 "
                "scrollback_row_limit=%4")
                .arg(interaction_diagnostics_path())
                .arg(term::k_interaction_trace_total_capacity_bytes)
                .arg(m_retained_history_capacity_bytes)
                .arg(m_scrollback_limit));
    }
}

QString VNM_TerminalSurface::interaction_diagnostics_path() const
{
    return term::interaction_trace_path();
}

QString VNM_TerminalSurface::interaction_diagnostics_error() const
{
    return m_interaction_diagnostics_error;
}

void VNM_TerminalSurface::record_interaction_diagnostic(
    const char*    category,
    const char*    event,
    const QString& details,
    std::uint64_t  correlation_id) const
{
    term::record_interaction_trace(category, event, details, correlation_id);
}

void VNM_TerminalSurface::record_key_interaction_diagnostic(
    const char*      category,
    const char*      event,
    const QKeyEvent& key_event,
    std::uint64_t    correlation_id) const
{
    if (!term::interaction_trace_enabled()) {
        return;
    }
    term::record_interaction_trace(
        category,
        event,
        term::interaction_trace_key_summary(key_event),
        correlation_id);
}

bool VNM_TerminalSurface::primary_repaint_recovery_enabled() const
{
    return m_primary_repaint_recovery_enabled;
}

void VNM_TerminalSurface::set_primary_repaint_recovery_enabled(bool enabled)
{
    if (m_primary_repaint_recovery_enabled == enabled) {
        return;
    }

    m_primary_repaint_recovery_enabled = enabled;
    emit primary_repaint_recovery_enabled_changed();
    if (m_private->session != nullptr) {
        m_private->session->set_primary_repaint_recovery_enabled(enabled);
        sync_from_session();
    }
}

std::optional<vnm_terminal::Backend_output_capture_config>
VNM_TerminalSurface::backend_output_capture_config() const
{
    return m_backend_output_capture_config;
}

void VNM_TerminalSurface::set_backend_output_capture_config(
    std::optional<vnm_terminal::Backend_output_capture_config> config)
{
    m_backend_output_capture_config = std::move(config);
}

QString VNM_TerminalSurface::transcript_capture_path() const
{
    return m_transcript_capture_path;
}

void VNM_TerminalSurface::set_transcript_capture_path(const QString& path)
{
    m_transcript_capture_path = path;
}

bool VNM_TerminalSurface::transcript_snapshot_diagnostics() const
{
    return m_transcript_snapshot_diagnostics;
}

void VNM_TerminalSurface::set_transcript_snapshot_diagnostics(bool enabled)
{
    m_transcript_snapshot_diagnostics = enabled;
}

bool VNM_TerminalSurface::transcript_timing_diagnostics() const
{
    return m_transcript_timing_diagnostics;
}

void VNM_TerminalSurface::set_transcript_timing_diagnostics(bool enabled)
{
    m_transcript_timing_diagnostics = enabled;
}

bool VNM_TerminalSurface::wheel_trace_enabled() const
{
    return m_wheel_trace_enabled;
}

void VNM_TerminalSurface::set_wheel_trace_enabled(bool enabled)
{
    m_wheel_trace_enabled = enabled;
}

void VNM_TerminalSurface::record_wheel_trace_event(
    const QString&     source,
    const QWheelEvent& event,
    const QString&     route,
    const QString&     outcome,
    bool               accepted,
    int                wheel_steps,
    int                effective_line_delta,
    qreal              angle_remainder,
    qreal              pixel_remainder,
    int                backend_drain_calls,
    qint64             backend_drain_elapsed_ns,
    bool               local_scroll_intent_recorded,
    const QString&     local_scroll_block_reason,
    const QString&     scroll_action,
    int                applied_line_delta,
    bool               deferred_intent_recorded)
{
    if (!m_wheel_trace_enabled) {
        return;
    }

    QJsonObject object = wheel_trace_event_object(source, event);
    object.insert(QStringLiteral("route"), route);
    object.insert(QStringLiteral("outcome"), outcome);
    object.insert(QStringLiteral("accepted"), accepted);
    object.insert(QStringLiteral("wheel_steps"), wheel_steps);
    object.insert(QStringLiteral("effective_line_delta"), effective_line_delta);
    object.insert(QStringLiteral("angle_remainder"), angle_remainder);
    object.insert(QStringLiteral("pixel_remainder"), pixel_remainder);
    object.insert(QStringLiteral("wheel_event_policy"), wheel_event_policy_name(m_wheel_event_policy));
    object.insert(
        QStringLiteral("alternate_screen_wheel_policy"),
        alternate_screen_wheel_policy_name(m_alternate_screen_wheel_policy));
    object.insert(
        QStringLiteral("mouse_reporting_policy"),
        mouse_reporting_policy_name(m_mouse_reporting_policy));

    const bool session_present = m_private->session != nullptr;
    object.insert(QStringLiteral("session_present"), session_present);
    const bool render_publication_blocked =
        session_present && m_private->session->render_publication_blocked();
    object.insert(
        QStringLiteral("render_publication_blocked"),
        render_publication_blocked);
    object.insert(
        QStringLiteral("published_synchronized_output"),
        m_private->render_snapshot != nullptr &&
            m_private->render_snapshot->modes.synchronized_output);

    bool alternate_screen = false;
    bool live_sgr_mouse_reporting = false;
    if (session_present) {
        const term::Terminal_viewport_state live_viewport =
            m_private->session->viewport_state();
        alternate_screen =
            live_viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE;
        live_sgr_mouse_reporting = m_private->session->mouse_reporting_active();
        insert_wheel_trace_viewport(object, QStringLiteral("live_viewport"), live_viewport);
    }

    const bool published_sgr_mouse_reporting =
        snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
    const bool published_mouse_tracking =
        snapshot_has_terminal_mouse_tracking(m_private->render_snapshot);
    if (m_private->render_snapshot != nullptr) {
        const term::Terminal_viewport_state& published_viewport =
            m_private->render_snapshot->viewport;
        alternate_screen =
            alternate_screen ||
            published_viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE;
        insert_wheel_trace_viewport(
            object,
            QStringLiteral("published_viewport"),
            published_viewport);
    }

    const bool local_scroll_route = route == QStringLiteral("local_scroll");
    object.insert(QStringLiteral("alternate_screen"), alternate_screen);
    object.insert(QStringLiteral("local_scroll_attempted"), local_scroll_route);
    object.insert(
        QStringLiteral("local_scroll_intent_recorded"),
        local_scroll_intent_recorded);
    const bool local_scroll_applied = applied_line_delta != 0;
    std::optional<bool> visible_scroll_applied;
    if (local_scroll_applied &&
        render_publication_blocked &&
        m_private->render_snapshot != nullptr &&
        m_private->render_snapshot->basis ==
            term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
        m_private->render_snapshot->purpose ==
            term::Terminal_render_snapshot_purpose::SCROLL)
    {
        visible_scroll_applied =
            m_private->render_snapshot->public_scroll_diagnostics.visible_scroll_applied;
    }
    term::insert_wheel_trace_scroll_publication_fields(
        object,
        local_scroll_applied,
        render_publication_blocked,
        visible_scroll_applied,
        deferred_intent_recorded);
    object.insert(QStringLiteral("live_sgr_mouse_reporting"), live_sgr_mouse_reporting);
    object.insert(QStringLiteral("published_sgr_mouse_reporting"), published_sgr_mouse_reporting);
    object.insert(QStringLiteral("published_mouse_tracking"), published_mouse_tracking);
    object.insert(QStringLiteral("backend_drain_calls"), backend_drain_calls);
    object.insert(QStringLiteral("backend_drain_elapsed_ns"), backend_drain_elapsed_ns);
    if (!local_scroll_block_reason.isEmpty()) {
        object.insert(QStringLiteral("local_scroll_block_reason"), local_scroll_block_reason);
    }
    if (!scroll_action.isEmpty()) {
        object.insert(QStringLiteral("scroll_action"), scroll_action);
        object.insert(QStringLiteral("applied_line_delta"), applied_line_delta);
    }
    object.insert(
        QStringLiteral("synchronized_output_scroll_policy"),
        surface_synchronized_output_scroll_policy_name(
            m_synchronized_output_scroll_policy));

    record_surface_wheel_trace_transcript(
        m_private->transcript_recorder,
        std::move(object));
}

int VNM_TerminalSurface::synchronized_output_stale_timeout_ms() const
{
    return m_synchronized_output_stale_timeout_ms;
}

void VNM_TerminalSurface::set_synchronized_output_stale_timeout_ms(int timeout_ms)
{
    const int bounded_timeout_ms =
        std::max(k_min_synchronized_output_stale_timeout_ms, timeout_ms);

    if (m_synchronized_output_stale_timeout_ms == bounded_timeout_ms) {
        return;
    }

    m_synchronized_output_stale_timeout_ms = bounded_timeout_ms;
    emit synchronized_output_stale_timeout_ms_changed();
    sync_synchronized_output_recovery_timer();
}

VNM_TerminalSurface::Synchronized_output_scroll_policy
VNM_TerminalSurface::synchronized_output_scroll_policy() const
{
    return m_synchronized_output_scroll_policy;
}

void VNM_TerminalSurface::set_synchronized_output_scroll_policy(
    Synchronized_output_scroll_policy policy)
{
    if (m_synchronized_output_scroll_policy == policy) {
        return;
    }

    m_synchronized_output_scroll_policy = policy;
    if (m_private->session != nullptr) {
        m_private->session->set_synchronized_output_scroll_policy(
            terminal_synchronized_output_scroll_policy(policy));
    }
    emit synchronized_output_scroll_policy_changed();
}

VNM_TerminalSurface::Text_area_resize_policy
VNM_TerminalSurface::text_area_resize_policy() const
{
    return m_text_area_resize_policy;
}

void VNM_TerminalSurface::set_text_area_resize_policy(Text_area_resize_policy policy)
{
    if (m_text_area_resize_policy == policy) {
        return;
    }

    m_text_area_resize_policy = policy;
    if (m_private->session != nullptr) {
        m_private->session->set_text_area_resize_policy(
            terminal_text_area_resize_policy(policy));
        // The session drains pending work across that call, which can advance
        // the model and publish a snapshot, so republish before the notify.
        sync_from_session();
    }
    emit text_area_resize_policy_changed();
}

bool VNM_TerminalSurface::text_area_resize_arbitration_enabled() const
{
    return m_text_area_resize_arbitration_enabled;
}

void VNM_TerminalSurface::set_text_area_resize_arbitration_enabled(bool enabled)
{
    if (m_text_area_resize_arbitration_enabled == enabled) {
        return;
    }

    m_text_area_resize_arbitration_enabled = enabled;
    if (m_private->session != nullptr) {
        m_private->session->set_text_area_resize_arbitration(
            enabled
                ? std::optional<term::terminal_text_area_resize_arbitration_config_t>(
                    term::terminal_text_area_resize_arbitration_config_t{})
                : std::nullopt);
        // Removing the capability settles any request in flight, which drains
        // pending work and can publish, so republish before the notify.
        sync_from_session();
    }
    emit text_area_resize_arbitration_enabled_changed();
}

int VNM_TerminalSurface::text_area_resize_arbitration_timeout_ms() const
{
    return m_text_area_resize_arbitration_timeout_ms;
}

void VNM_TerminalSurface::set_text_area_resize_arbitration_timeout_ms(int timeout_ms)
{
    const int bounded_timeout_ms = std::max(timeout_ms, 0);
    if (m_text_area_resize_arbitration_timeout_ms == bounded_timeout_ms) {
        return;
    }

    m_text_area_resize_arbitration_timeout_ms = bounded_timeout_ms;
    // A request already in flight was armed under the previous deadline; the new
    // one is what the host just asked for, so it takes effect immediately.
    if (m_private->pending_text_area_resize_arbitration.has_value()) {
        m_private->text_area_resize_arbitration_timer.stop();
        if (bounded_timeout_ms > 0) {
            m_private->text_area_resize_arbitration_timer.start(bounded_timeout_ms);
        }
    }
    emit text_area_resize_arbitration_timeout_ms_changed();
}

VNM_TerminalSurface::Mouse_reporting_policy VNM_TerminalSurface::mouse_reporting_policy() const
{
    return m_mouse_reporting_policy;
}

void VNM_TerminalSurface::set_mouse_reporting_policy(Mouse_reporting_policy policy)
{
    if (m_mouse_reporting_policy == policy) {
        return;
    }

    m_mouse_reporting_policy = policy;
    m_private->clear_pending_published_mouse_reports();
    m_private->clear_mouse_reporting_state();
    m_private->clear_mouse_wheel_remainders();
    emit mouse_reporting_policy_changed();
}

VNM_TerminalSurface::Copy_shortcut_policy VNM_TerminalSurface::copy_shortcut_policy() const
{
    return m_copy_shortcut_policy;
}

void VNM_TerminalSurface::set_copy_shortcut_policy(Copy_shortcut_policy policy)
{
    if (m_copy_shortcut_policy == policy) {
        return;
    }

    m_copy_shortcut_policy = policy;
    emit copy_shortcut_policy_changed();
}

bool VNM_TerminalSurface::copy_on_select() const
{
    return m_copy_on_select;
}

void VNM_TerminalSurface::set_copy_on_select(bool enabled)
{
    if (m_copy_on_select == enabled) {
        return;
    }

    m_copy_on_select = enabled;
    emit copy_on_select_changed();
}

VNM_TerminalSurface::Wheel_event_policy VNM_TerminalSurface::wheel_event_policy() const
{
    return m_wheel_event_policy;
}

void VNM_TerminalSurface::set_wheel_event_policy(Wheel_event_policy policy)
{
    if (m_wheel_event_policy == policy) {
        return;
    }

    m_wheel_event_policy = policy;
    m_private->clear_wheel_remainders();
    emit wheel_event_policy_changed();
}

VNM_TerminalSurface::Alternate_screen_wheel_policy
VNM_TerminalSurface::alternate_screen_wheel_policy() const
{
    return m_alternate_screen_wheel_policy;
}

void VNM_TerminalSurface::set_alternate_screen_wheel_policy(
    Alternate_screen_wheel_policy policy)
{
    if (m_alternate_screen_wheel_policy == policy) {
        return;
    }

    m_alternate_screen_wheel_policy = policy;
    m_private->clear_mouse_wheel_remainders();
    emit alternate_screen_wheel_policy_changed();
}

VNM_TerminalSurface::Bracketed_paste_policy VNM_TerminalSurface::bracketed_paste_policy() const
{
    return m_bracketed_paste_policy;
}

void VNM_TerminalSurface::set_bracketed_paste_policy(Bracketed_paste_policy policy)
{
    if (m_bracketed_paste_policy == policy) {
        return;
    }

    m_bracketed_paste_policy = policy;
    emit bracketed_paste_policy_changed();
}

VNM_TerminalSurface::Bell_policy VNM_TerminalSurface::audible_bell_policy() const
{
    return m_audible_bell_policy;
}

void VNM_TerminalSurface::set_audible_bell_policy(Bell_policy policy)
{
    if (m_audible_bell_policy == policy) {
        return;
    }

    m_audible_bell_policy = policy;
    if (m_private->session) {
        m_private->session->set_bell_policy(
            terminal_bell_policy_for_surface(m_audible_bell_policy, m_visual_bell_policy));
    }
    emit audible_bell_policy_changed();
}

VNM_TerminalSurface::Bell_policy VNM_TerminalSurface::visual_bell_policy() const
{
    return m_visual_bell_policy;
}

void VNM_TerminalSurface::set_visual_bell_policy(Bell_policy policy)
{
    if (m_visual_bell_policy == policy) {
        return;
    }

    m_visual_bell_policy = policy;
    if (m_private->session) {
        m_private->session->set_bell_policy(
            terminal_bell_policy_for_surface(m_audible_bell_policy, m_visual_bell_policy));
    }
    emit visual_bell_policy_changed();
    m_private->request_render_update(*this);
}

bool VNM_TerminalSurface::row_timestamp_tooltip_enabled() const
{
    return m_row_timestamp_tooltip_enabled;
}

void VNM_TerminalSurface::set_row_timestamp_tooltip_enabled(bool enabled)
{
    if (m_row_timestamp_tooltip_enabled == enabled) {
        return;
    }

    m_row_timestamp_tooltip_enabled = enabled;
    if (!enabled) {
        dismiss_row_timestamp_tooltip();
    }
    emit row_timestamp_tooltip_enabled_changed();
}

void VNM_TerminalSurface::dismiss_row_timestamp_tooltip()
{
    m_private->row_timestamp_tooltip_timer.stop();
    if (!m_private->row_timestamp_tooltip_request_active) {
        return;
    }

    m_private->row_timestamp_tooltip_request_active = false;
    if (!m_private->shutting_down.load()) {
        emit row_timestamp_tooltip_dismissed();
    }
}

bool VNM_TerminalSurface::row_timestamp_tooltip_pointer_moved(const QPointF& position)
{
    // Records the pointer position and reports whether it moved at least one
    // logical pixel. Qt Quick re-delivers pointer-move events at the unchanged
    // cursor position whenever scene content changes under a stationary
    // pointer, and the tooltip appearing in the host's chrome is exactly such
    // a change, so a same-position event is not user activity: it must neither
    // dismiss a shown tooltip nor restart the hover-idle timer.
    std::optional<QPointF>& last_position =
        m_private->row_timestamp_tooltip_pointer_position;
    if (last_position.has_value() &&
        (position - *last_position).manhattanLength() < 1.0)
    {
        return false;
    }

    last_position = position;
    return true;
}

void VNM_TerminalSurface::handle_row_timestamp_tooltip_timeout()
{
    const QPointF pointer_position = *m_private->row_timestamp_tooltip_pointer_position;
    const std::optional<term::terminal_grid_position_t> position =
        grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            pointer_position);
    if (!position.has_value()) {
        return;
    }

    // Synthetic and suppressed-provenance snapshots publish an empty
    // visible_line_provenance vector. The contract is full-and-valid or
    // absent, so require the validated per-row vector instead of a bare size
    // check; a partial vector must not yield a wrong-row timestamp.
    const term::Terminal_render_snapshot& snapshot = *m_private->render_snapshot;
    if (!term::render_snapshot_visible_line_provenance_is_valid(snapshot)) {
        return;
    }

    const qint64 stamp_ms = snapshot
        .visible_line_provenance[static_cast<std::size_t>(position->row)]
        .content_stamp_ms;
    if (stamp_ms == 0) {
        // Never-written rows carry no stamp and request no tooltip; that is
        // the contract, not a degradation.
        return;
    }

    m_private->row_timestamp_tooltip_request_active = true;
    emit row_timestamp_tooltip_requested(
        pointer_position.x(),
        pointer_position.y(),
        QDateTime::fromMSecsSinceEpoch(stamp_ms));
}

VNM_TerminalSurface::Text_renderer_mode VNM_TerminalSurface::text_renderer_mode() const
{
    return m_text_renderer_mode;
}

void VNM_TerminalSurface::set_text_renderer_mode(Text_renderer_mode mode)
{
    if (m_text_renderer_mode == mode) {
        return;
    }

    m_text_renderer_mode = mode;
    emit text_renderer_mode_changed();
    m_private->request_render_update(*this);
}

VNM_TerminalSurface::Lcd_subpixel_order VNM_TerminalSurface::lcd_subpixel_order() const
{
    return m_lcd_subpixel_order;
}

void VNM_TerminalSurface::set_lcd_subpixel_order(Lcd_subpixel_order order)
{
    if (m_lcd_subpixel_order == order) {
        return;
    }

    m_lcd_subpixel_order = order;
    emit lcd_subpixel_order_changed();
    m_private->request_render_update(*this);
}

QString VNM_TerminalSurface::terminal_title() const
{
    return m_terminal_title;
}

QString VNM_TerminalSurface::terminal_icon_name() const
{
    return m_terminal_icon_name;
}

VNM_TerminalSurface::Process_state VNM_TerminalSurface::process_state() const
{
    return m_process_state;
}

bool VNM_TerminalSurface::backend_ready() const
{
    return m_backend_ready;
}

bool VNM_TerminalSurface::backend_geometry_in_sync() const
{
    return m_backend_geometry_in_sync;
}

int VNM_TerminalSurface::rows() const
{
    return m_rows;
}

int VNM_TerminalSurface::columns() const
{
    return m_columns;
}

quint64 VNM_TerminalSurface::paint_completed_frame_count() const
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_private->reconcile_atlas_completion(
        const_cast<VNM_TerminalSurface&>(*this));
    return m_private->current_paint_completed_frame_count();
}

quint64 VNM_TerminalSurface::qsg_atlas_render_frame_count() const
{
    Q_ASSERT(thread() == QThread::currentThread());
    return m_private->qsg_atlas_recorder != nullptr
        ? m_private->qsg_atlas_recorder->snapshot().render_count
        : term::Qsg_atlas_frame_report{}.render_count;
}

void VNM_TerminalSurface::set_selection_trace_enabled(bool enabled)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_selection_trace_enabled = enabled;
}

void VNM_TerminalSurface::set_dirty_row_stats_enabled(bool enabled)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_private->dirty_row_stats_enabled = enabled;
    if (m_private->session != nullptr) {
        m_private->session->set_dirty_row_stats_enabled(enabled);
    }
}

void VNM_TerminalSurface::set_clipboard_text_reader(
    std::function<std::optional<QString>()> reader)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_private->clipboard_text_reader = std::move(reader);
}

int VNM_TerminalSurface::scrollback_rows() const
{
    return m_scrollback_rows;
}

int VNM_TerminalSurface::viewport_visible_rows() const
{
    return m_viewport_visible_rows;
}

int VNM_TerminalSurface::viewport_offset_from_tail() const
{
    return m_viewport_offset_from_tail;
}

bool VNM_TerminalSurface::viewport_at_tail() const
{
    return m_viewport_at_tail;
}

VNM_TerminalSurface::Selection_state VNM_TerminalSurface::selection_state() const
{
    return m_selection_state;
}

QString VNM_TerminalSurface::search_query() const
{
    return m_search_query;
}

VNM_TerminalSurface::Search_result_state
VNM_TerminalSurface::search_result_state() const
{
    return m_search_result_state;
}

int VNM_TerminalSurface::search_match_count() const
{
    return m_search_match_count;
}

int VNM_TerminalSurface::current_search_match() const
{
    return m_current_search_match;
}

bool VNM_TerminalSurface::respond_clipboard_write(
    quint64                        request_id,
    Clipboard_response_decision    decision)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (!m_private->pending_clipboard_write.has_value() ||
        m_private->pending_clipboard_write->request_id != request_id)
    {
        emit backend_error(
            Backend_error_code::CALLBACK_MISSING,
            QStringLiteral("no active clipboard write request"));
        return false;
    }

    term::Terminal_osc52_write_request request =
        std::move(*m_private->pending_clipboard_write);
    m_private->pending_clipboard_write.reset();

    if (decision == Clipboard_response_decision::DENY) {
        return true;
    }

    if (request.target_selection != QStringLiteral("c") &&
        request.target_selection != QStringLiteral("clipboard"))
    {
        emit backend_error(
            Backend_error_code::CALLBACK_MISSING,
            QStringLiteral("unsupported clipboard write target"));
        return false;
    }

    return set_terminal_clipboard_text(QString::fromUtf8(request.decoded_payload));
}

bool VNM_TerminalSurface::respond_text_area_resize(
    quint64                                 request_id,
    Text_area_resize_arbitration_decision   decision,
    int                                     effective_rows,
    int                                     effective_columns)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (!m_private->pending_text_area_resize_arbitration.has_value() ||
        *m_private->pending_text_area_resize_arbitration != request_id ||
        m_private->session == nullptr)
    {
        emit backend_error(
            Backend_error_code::CALLBACK_MISSING,
            QStringLiteral("no active text-area resize arbitration"));
        return false;
    }

    m_private->text_area_resize_arbitration_timer.stop();
    m_private->pending_text_area_resize_arbitration.reset();

    const term::Terminal_session_result result =
        m_private->session->settle_text_area_resize_arbitration({
            request_id,
            decision == Text_area_resize_arbitration_decision::ACCEPT
                ? term::Terminal_text_area_resize_arbitration_outcome::ACCEPTED
                : term::Terminal_text_area_resize_arbitration_outcome::REJECTED,
            term::terminal_grid_size_t{effective_rows, effective_columns},
        });
    // The session drains pending work across that call, which can advance the
    // model and publish a snapshot, so republish before returning.
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
        return false;
    }

    return true;
}

void VNM_TerminalSurface::handle_text_area_resize_arbitration_timeout()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (!m_private->pending_text_area_resize_arbitration.has_value() ||
        m_private->session == nullptr)
    {
        return;
    }

    const quint64 request_id = *m_private->pending_text_area_resize_arbitration;
    m_private->pending_text_area_resize_arbitration.reset();
    (void)m_private->session->settle_text_area_resize_arbitration({
        request_id,
        term::Terminal_text_area_resize_arbitration_outcome::TIMED_OUT,
        {},
    });
    sync_from_session();
}

QByteArray VNM_TerminalSurface::explicit_hyperlink_at(qreal x, qreal y) const
{
    Q_ASSERT(thread() == QThread::currentThread());

    const std::optional<Surface_hyperlink_target> hyperlink =
        explicit_hyperlink_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            QPointF(x, y));
    return hyperlink.has_value() ? hyperlink->uri : QByteArray{};
}

void VNM_TerminalSurface::set_hyperlink_hover_position(std::optional<QPointF> position)
{
    m_private->hyperlink_hover_position = std::move(position);
    refresh_hyperlink_hover_feedback();
}

void VNM_TerminalSurface::refresh_hyperlink_hover_feedback()
{
    Q_ASSERT(thread() == QThread::currentThread());

    const bool cursor_active =
        m_private->hyperlink_hover_position.has_value() &&
        explicit_hyperlink_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            *m_private->hyperlink_hover_position).has_value();
    if (cursor_active == m_private->hyperlink_cursor_active) {
        return;
    }

    m_private->hyperlink_cursor_active = cursor_active;
    if (cursor_active) {
        setCursor(QCursor(Qt::PointingHandCursor));
    }
    else {
        unsetCursor();
    }
}

QString VNM_TerminalSurface::selected_text()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface selected-text reason=no-session"));
        }
        return {};
    }

    drain_backend_callback_events();
    const term::Terminal_selection_result result = m_private->session->selected_text();
    if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface selected-text result=%1 size=%2")
                .arg(static_cast<int>(result.code))
                .arg(result.text.size()));
    }
    return result.code == term::Terminal_selection_result_code::OK
        ? result.text
        : QString();
}

bool VNM_TerminalSurface::copy_selected_text_to_clipboard(
    Empty_selection_copy_policy policy)
{
    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface copy-selected-text reason=no-session"));
        }
        return false;
    }

    const term::Terminal_selection_result result = m_private->session->selected_text();
    if (result.code != term::Terminal_selection_result_code::OK ||
        (policy == Empty_selection_copy_policy::SKIP_EMPTY_SELECTION && result.text.isEmpty()))
    {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral("surface copy-selected-text result=%1 size=%2")
                    .arg(static_cast<int>(result.code))
                    .arg(result.text.size()));
        }
        return false;
    }

    const bool clipboard_write_persisted = set_terminal_clipboard_text(result.text);
    if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface copy-selected-text result=%1 size=%2 clipboard=%3")
                .arg(static_cast<int>(result.code))
                .arg(result.text.size())
                .arg(selection_trace_bool(clipboard_write_persisted)));
    }
    return clipboard_write_persisted;
}

void VNM_TerminalSurface::clear_selection()
{
    Q_ASSERT(thread() == QThread::currentThread());

    m_private->clear_selection_drag_state();

    if (m_private->session == nullptr) {
        return;
    }
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("clear"),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        false);
    m_private->clear_selection_with_sync(*this);
}

void VNM_TerminalSurface::set_search_query(QString query)
{
    Q_ASSERT(thread() == QThread::currentThread());

    drain_backend_callback_events();
    if (m_private->session == nullptr) {
        const Search_result_state state = query.isEmpty()
            ? Search_result_state::INACTIVE
            : Search_result_state::SOURCE_UNAVAILABLE;
        set_search_state(std::move(query), state, 0, 0);
        return;
    }

    m_private->session->set_search_query(std::move(query));
    sync_from_session();
}

void VNM_TerminalSurface::clear_search()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        set_search_state({}, Search_result_state::INACTIVE, 0, 0);
        return;
    }

    m_private->session->clear_search();
    sync_from_session();
}

bool VNM_TerminalSurface::search_next()
{
    Q_ASSERT(thread() == QThread::currentThread());

    drain_backend_callback_events();
    if (m_private->session == nullptr) {
        return false;
    }

    const bool selected = m_private->session->search_next();
    sync_from_session();
    return selected;
}

bool VNM_TerminalSurface::search_previous()
{
    Q_ASSERT(thread() == QThread::currentThread());

    drain_backend_callback_events();
    if (m_private->session == nullptr) {
        return false;
    }

    const bool selected = m_private->session->search_previous();
    sync_from_session();
    return selected;
}

bool VNM_TerminalSurface::paste_text(QString text)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return false;
    }
    m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
    if (m_private->session == nullptr) {
        return false;
    }

    const std::uint64_t trace_id = term::interaction_trace_enabled()
        ? term::next_interaction_trace_correlation_id()
        : 0U;
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "surface",
            "paste",
            QStringLiteral("text_units=%1").arg(text.size()),
            trace_id);
    }

    term::Terminal_session* const route_session = m_private->session.get();
    const term::Terminal_paste_text_result paste_result =
        route_session->write_paste_text(
            std::move(text),
            paste_framing_policy(m_bracketed_paste_policy),
            trace_id);
    sync_from_session();
    if (!paste_result.handled) {
        return false;
    }

    if (!is_accepted(paste_result.result.code)) {
        report_result_failure(paste_result.result);
        return false;
    }

    return true;
}

vnm_terminal::Terminal_message_submission_result
VNM_TerminalSurface::submit_utf8_message(QByteArray message_utf8)
{
    Q_ASSERT(thread() == QThread::currentThread());

    using Outcome = vnm_terminal::Terminal_message_submission_outcome;
    if (message_utf8.isEmpty()) {
        return {Outcome::EMPTY_MESSAGE, QStringLiteral("The message is empty.")};
    }
    if (message_utf8.size() >
        vnm_terminal::k_terminal_message_utf8_hard_limit_bytes)
    {
        return {
            Outcome::MESSAGE_TOO_LARGE,
            QStringLiteral("The message exceeds the terminal message hard limit."),
        };
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(message_utf8);
    const QStringDecoder::FinalizeResult finalization = decoder.finalize();
    if (finalization.error != QStringDecoder::FinalizeResult::Error::NoError) {
        return {
            Outcome::INVALID_UTF8,
            QStringLiteral("The message is not valid UTF-8."),
        };
    }
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if ((code <= 0x001fU && code != u'\n' && code != u'\t') ||
            code == 0x007fU ||
            (code >= 0x0080U && code <= 0x009fU))
        {
            return {
                Outcome::INVALID_MESSAGE,
                QStringLiteral(
                    "The message contains unsupported terminal control characters."),
            };
        }
    }
    if (m_private->session == nullptr) {
        return {
            Outcome::NOT_RUNNING,
            QStringLiteral("The terminal session is not running."),
        };
    }

    m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
    if (m_private->session == nullptr) {
        return {
            Outcome::NOT_RUNNING,
            QStringLiteral("The terminal session is not running."),
        };
    }

    const std::uint64_t trace_id = term::interaction_trace_enabled()
        ? term::next_interaction_trace_correlation_id()
        : 0U;
    term::Terminal_session* const route_session = m_private->session.get();
    const term::Terminal_paste_text_result submission =
        route_session->write_submitted_text(
            std::move(text),
            paste_framing_policy(m_bracketed_paste_policy),
            trace_id);
    sync_from_session();
    if (!submission.handled) {
        return {
            Outcome::EMPTY_MESSAGE,
            QStringLiteral("The message has no terminal-safe text."),
        };
    }

    const QString error = submission.result.error
        ? submission.result.error->message
        : QString{};
    switch (submission.result.code) {
        case term::Terminal_session_result_code::ACCEPTED:
            return {Outcome::ACCEPTED, {}};
        case term::Terminal_session_result_code::INVALID_STATE:
            return {Outcome::NOT_RUNNING, error};
        case term::Terminal_session_result_code::INVALID_ARGUMENT:
            return {Outcome::MESSAGE_TOO_LARGE, error};
        case term::Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED:
            return {Outcome::QUEUE_LIMIT, error};
        case term::Terminal_session_result_code::BACKEND_REJECTED:
            return {Outcome::BACKEND_REJECTED, error};
    }

    Q_UNREACHABLE();
    return {Outcome::BACKEND_REJECTED, error};
}

std::optional<QString> VNM_TerminalSurface::read_clipboard_text_for_paste()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->clipboard_text_reader) {
        return m_private->clipboard_text_reader();
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return std::nullopt;
    }

    return clipboard->text(QClipboard::Clipboard);
}

bool VNM_TerminalSurface::paste_clipboard_text()
{
    Q_ASSERT(thread() == QThread::currentThread());

    const std::optional<QString> text = read_clipboard_text_for_paste();
    if (!text.has_value()) {
        return false;
    }

    return paste_text(*text);
}

bool VNM_TerminalSurface::scroll_viewport_lines(int line_delta)
{
    return scroll_viewport_lines_with_diagnostics(line_delta).event_accepted;
}

VNM_TerminalSurface::wheel_scroll_diagnostic_result_t
VNM_TerminalSurface::scroll_viewport_lines_with_diagnostics(int line_delta)
{
    return scroll_viewport_lines_with_diagnostics(
        line_delta,
        QStringLiteral("api.lines"));
}

VNM_TerminalSurface::wheel_scroll_diagnostic_result_t
VNM_TerminalSurface::scroll_viewport_lines_with_diagnostics(
    int     line_delta,
    QString source)
{
    Q_ASSERT(thread() == QThread::currentThread());

    wheel_scroll_diagnostic_result_t diagnostic;
    diagnostic.session_present = m_private->session != nullptr;
    if (line_delta == 0) {
        diagnostic.no_op_cause = Scroll_noop_cause::ZERO_LINE_DELTA;
        return diagnostic;
    }

    if (!diagnostic.session_present) {
        diagnostic.no_op_cause = Scroll_noop_cause::NO_SESSION;
        return diagnostic;
    }

    diagnostic.render_publication_blocked =
        m_private->session->render_publication_blocked();
    diagnostic.published_synchronized_output =
        m_private->render_snapshot != nullptr &&
        m_private->render_snapshot->modes.synchronized_output;

    const term::Terminal_viewport_state viewport_before =
        m_private->render_snapshot != nullptr
            ? m_private->render_snapshot->viewport
            : m_private->session->viewport_state();
    diagnostic.alternate_screen =
        viewport_before.active_buffer == term::Terminal_buffer_id::ALTERNATE;
    if (source.isEmpty()) {
        source = QStringLiteral("api.lines");
    }
    record_surface_scroll_intent_transcript(
        m_private->transcript_recorder,
        source,
        line_delta,
        std::nullopt,
        viewport_before);
    diagnostic.local_scroll_intent_recorded = true;
    term::Terminal_session* const route_session = m_private->session.get();
    const std::uint64_t route_session_generation = m_private->session_generation;
    const term::Terminal_viewport_scroll_result scroll_result =
        route_session->scroll_published_viewport_lines(line_delta);
    diagnostic.scroll_action = public_scroll_action(scroll_result.action);
    diagnostic.applied_line_delta = scroll_result.applied_line_delta;
    sync_from_session();
    if (!m_private->active_session_matches(route_session, route_session_generation)) {
        return diagnostic;
    }
    diagnostic.local_scroll_applied =
        scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
    diagnostic.deferred_intent_recorded =
        scroll_result.action ==
            term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED;
    diagnostic.event_accepted =
        diagnostic.local_scroll_applied || diagnostic.deferred_intent_recorded;

    if (diagnostic.event_accepted)
    {
        const term::Terminal_viewport_state viewport_after =
            m_private->render_snapshot != nullptr
                ? m_private->render_snapshot->viewport
                : m_private->session->viewport_state();
        const bool render_publication_blocked_after_scroll =
            m_private->session->render_publication_blocked();
        const bool public_projection_scroll_visible =
            render_publication_blocked_after_scroll &&
            m_private->render_snapshot != nullptr &&
            m_private->render_snapshot->basis ==
                term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            m_private->render_snapshot->purpose ==
                term::Terminal_render_snapshot_purpose::SCROLL &&
            m_private->render_snapshot->public_scroll_diagnostics.visible_scroll_applied;
        diagnostic.visible_scroll_applied =
            diagnostic.local_scroll_applied &&
            (!render_publication_blocked_after_scroll || public_projection_scroll_visible);
        record_surface_scroll_transcript(
            m_private->transcript_recorder,
            source,
            line_delta,
            std::nullopt,
            scroll_result,
            viewport_before,
            viewport_after);
        return diagnostic;
    }

    if (diagnostic.render_publication_blocked) {
        diagnostic.no_op_cause = Scroll_noop_cause::SYNCHRONIZED_OUTPUT_DEFERRED;
    }
    else
    if (diagnostic.published_synchronized_output) {
        diagnostic.no_op_cause = Scroll_noop_cause::SYNCHRONIZED_OUTPUT_PUBLISHED;
    }
    else
    if (diagnostic.alternate_screen) {
        diagnostic.no_op_cause = Scroll_noop_cause::ALTERNATE_SCREEN;
    }
    else
    if (scroll_result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY) {
        diagnostic.no_op_cause = Scroll_noop_cause::BOUNDARY_OR_CLAMP;
    }
    else {
        diagnostic.no_op_cause = Scroll_noop_cause::NO_PUBLICATION;
    }

    return diagnostic;
}

bool VNM_TerminalSurface::scroll_to_offset_from_tail(int offset_from_tail)
{
    return scroll_to_offset_from_tail_with_diagnostics(
        offset_from_tail,
        QStringLiteral("api.offset")).event_accepted;
}

VNM_TerminalSurface::wheel_scroll_diagnostic_result_t
VNM_TerminalSurface::scroll_to_offset_from_tail_with_diagnostics(int offset_from_tail)
{
    return scroll_to_offset_from_tail_with_diagnostics(
        offset_from_tail,
        QStringLiteral("api.offset"));
}

VNM_TerminalSurface::wheel_scroll_diagnostic_result_t
VNM_TerminalSurface::scroll_to_offset_from_tail_with_diagnostics(
    int     offset_from_tail,
    QString source)
{
    Q_ASSERT(thread() == QThread::currentThread());

    wheel_scroll_diagnostic_result_t diagnostic;
    diagnostic.session_present = m_private->session != nullptr;
    if (m_private->session == nullptr) {
        diagnostic.no_op_cause = Scroll_noop_cause::NO_SESSION;
        return diagnostic;
    }

    diagnostic.render_publication_blocked =
        m_private->session->render_publication_blocked();
    diagnostic.published_synchronized_output =
        m_private->render_snapshot != nullptr &&
        m_private->render_snapshot->modes.synchronized_output;
    const term::Terminal_viewport_state viewport_before =
        m_private->render_snapshot != nullptr
            ? m_private->render_snapshot->viewport
            : m_private->session->viewport_state();
    diagnostic.alternate_screen =
        viewport_before.active_buffer == term::Terminal_buffer_id::ALTERNATE;
    if (source.isEmpty()) {
        source = QStringLiteral("api.offset");
    }
    // The session clamps the offset itself, but this difference is taken here
    // from the caller's raw value, and it is the one place on this path where
    // that value still reaches int arithmetic: an offset near the bottom of the
    // range against any scrolled-back viewport underflows, which is undefined
    // rather than merely a wrong number in the transcript.
    const int requested_line_delta = static_cast<int>(std::clamp<long long>(
        static_cast<long long>(offset_from_tail) -
            static_cast<long long>(viewport_before.offset_from_tail),
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::max()));
    record_surface_scroll_intent_transcript(
        m_private->transcript_recorder,
        source,
        requested_line_delta,
        offset_from_tail,
        viewport_before);
    diagnostic.local_scroll_intent_recorded = true;
    term::Terminal_session* const route_session = m_private->session.get();
    const std::uint64_t route_session_generation = m_private->session_generation;
    const term::Terminal_viewport_scroll_result scroll_result =
        route_session->scroll_published_viewport_to_offset_from_tail(
            offset_from_tail);
    diagnostic.scroll_action = public_scroll_action(scroll_result.action);
    diagnostic.applied_line_delta = scroll_result.applied_line_delta;
    sync_from_session();
    if (!m_private->active_session_matches(route_session, route_session_generation)) {
        return diagnostic;
    }
    diagnostic.local_scroll_applied =
        scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
    diagnostic.deferred_intent_recorded =
        scroll_result.action ==
            term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED;
    diagnostic.event_accepted =
        scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED ||
        scroll_result.action ==
            term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED;
    if (diagnostic.event_accepted) {
        const term::Terminal_viewport_state viewport_after =
            m_private->render_snapshot != nullptr
                ? m_private->render_snapshot->viewport
                : m_private->session->viewport_state();
        const bool render_publication_blocked_after_scroll =
            m_private->session->render_publication_blocked();
        const bool public_projection_scroll_visible =
            render_publication_blocked_after_scroll &&
            m_private->render_snapshot != nullptr &&
            m_private->render_snapshot->basis ==
                term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            m_private->render_snapshot->purpose ==
                term::Terminal_render_snapshot_purpose::SCROLL &&
            m_private->render_snapshot->public_scroll_diagnostics.visible_scroll_applied;
        diagnostic.visible_scroll_applied =
            diagnostic.local_scroll_applied &&
            (!render_publication_blocked_after_scroll || public_projection_scroll_visible);
        record_surface_scroll_transcript(
            m_private->transcript_recorder,
            source,
            requested_line_delta,
            offset_from_tail,
            scroll_result,
            viewport_before,
            viewport_after);
        return diagnostic;
    }

    if (diagnostic.render_publication_blocked) {
        diagnostic.no_op_cause = Scroll_noop_cause::SYNCHRONIZED_OUTPUT_DEFERRED;
    }
    else
    if (diagnostic.published_synchronized_output) {
        diagnostic.no_op_cause = Scroll_noop_cause::SYNCHRONIZED_OUTPUT_PUBLISHED;
    }
    else
    if (diagnostic.alternate_screen) {
        diagnostic.no_op_cause = Scroll_noop_cause::ALTERNATE_SCREEN;
    }
    else
    if (scroll_result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY) {
        diagnostic.no_op_cause = Scroll_noop_cause::BOUNDARY_OR_CLAMP;
    }
    else {
        diagnostic.no_op_cause = Scroll_noop_cause::NO_PUBLICATION;
    }

    return diagnostic;
}

bool VNM_TerminalSurface::scroll_to_offset_from_tail_from_source(
    int     offset_from_tail,
    QString source)
{
    return scroll_to_offset_from_tail_with_diagnostics(
        offset_from_tail,
        std::move(source)).event_accepted;
}

bool VNM_TerminalSurface::start_process(QStringList argv, QString working_directory)
{
    Q_ASSERT(thread() == QThread::currentThread());

    term::Terminal_launch_config launch_config;
    launch_config.argv              = std::move(argv);
    launch_config.working_directory = std::move(working_directory);
    return start_process_with_native_backend(std::move(launch_config));
}

bool VNM_TerminalSurface::start_process_with_exact_environment(
    QStringList                argv,
    const QProcessEnvironment& environment_snapshot,
    QString                    working_directory)
{
    Q_ASSERT(thread() == QThread::currentThread());

    term::Terminal_launch_config launch_config;
    launch_config.argv                = std::move(argv);
    launch_config.working_directory   = std::move(working_directory);
    launch_config.inherit_environment = false;

    const QStringList environment_names = environment_snapshot.keys();
    launch_config.environment_edits.reserve(
        static_cast<std::size_t>(environment_names.size()));
    for (const QString& name : environment_names) {
        launch_config.environment_edits.push_back({
            term::Terminal_environment_operation::SET,
            name,
            environment_snapshot.value(name),
        });
    }

    return start_process_with_native_backend(std::move(launch_config));
}

bool VNM_TerminalSurface::start_process_with_native_backend(
    term::Terminal_launch_config launch_config)
{
    if (m_private->session != nullptr) {
        drain_backend_callback_events();
        if (is_live_process_state(m_private->session->process_state())) {
            report_backend_error({
                term::Terminal_backend_error_code::START_FAILED,
                QStringLiteral("start requires no active terminal process"),
            });
            return false;
        }
    }

    if (launch_config.argv.isEmpty() ||
        launch_config.argv.front().trimmed().isEmpty())
    {
        report_backend_error({
            term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
            QStringLiteral("argv must name an executable"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    std::unique_ptr<term::Terminal_backend> backend = make_native_backend();
    if (backend == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::START_FAILED,
            QStringLiteral("no native terminal backend is available on this platform"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    return start_process_with_backend(
        std::move(backend),
        std::move(launch_config));
}

bool VNM_TerminalSurface::interrupt_process()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::INTERRUPT_FAILED,
            QStringLiteral("interrupt requires an active terminal session"),
        });
        return false;
    }

    const term::Terminal_session_result result =
        m_private->session->interrupt();
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
        return false;
    }

    return true;
}

bool VNM_TerminalSurface::terminate_process()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::TERMINATE_FAILED,
            QStringLiteral("terminate requires an active terminal session"),
        });
        return false;
    }

    const term::Terminal_session_result result =
        m_private->session->terminate();
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
        return false;
    }

    return true;
}

QVariant VNM_TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        return true;
    }

    if (query == Qt::ImCursorRectangle) {
        return m_private->input_method_cursor_rectangle(*this);
    }

    return QQuickItem::inputMethodQuery(query);
}

void VNM_TerminalSurface::geometryChange(
    const QRectF&  new_geometry,
    const QRectF&  old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    refresh_grid_metrics();
}

void VNM_TerminalSurface::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);

    if (change == ItemActiveFocusHasChanged && term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "surface",
            "active-focus-changed",
            QStringLiteral("active_focus=%1 focus=%2 visible=%3 enabled=%4")
                .arg(hasActiveFocus())
                .arg(hasFocus())
                .arg(isVisible())
                .arg(isEnabled()));
    }

    if (change == ItemSceneChange) {
        const bool window_identity_changed = value.window != m_private->bound_window;
        if (auto lifecycle_recorder = m_private->lifecycle_recorder();
            lifecycle_recorder != nullptr)
        {
            lifecycle_recorder->record_item_scene_change();
            if (value.window == nullptr) {
                lifecycle_recorder->record_item_scene_detach();
            }
        }
        if (window_identity_changed) {
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
        }
        if (value.window == nullptr) {
            m_private->cancel_backend_callback_frame_update_or_queue_posted_drain(
                *this);
        }
        bind_window_signals(value.window);
        refresh_grid_metrics();
    }
    else
    if (change == ItemDevicePixelRatioHasChanged) {
        refresh_grid_metrics();
    }
    else
    if (change == ItemActiveFocusHasChanged && !hasActiveFocus()) {
        if (m_private->session != nullptr) {
            m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
            if (m_private->session == nullptr) {
                term::Ime_preedit_state state;
                m_private->set_ime_preedit_state(*this, std::move(state));
                return;
            }
            term::Terminal_session* const route_session = m_private->session.get();
            const term::Terminal_focus_event_result focus_result =
                route_session->write_focus_event(false);
            route_session->cancel_ime_preedit();
            sync_from_session();
            if (focus_result.handled) {
                if (!is_accepted(focus_result.result.code)) {
                    report_result_failure(focus_result.result);
                    return;
                }
            }
            return;
        }

        term::Ime_preedit_state state;
        m_private->set_ime_preedit_state(*this, std::move(state));
    }
    else
    if (change == ItemActiveFocusHasChanged) {
        if (m_private->session != nullptr) {
            m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
            if (m_private->session == nullptr) {
                return;
            }
            term::Terminal_session* const route_session = m_private->session.get();
            const term::Terminal_focus_event_result focus_result =
                route_session->write_focus_event(true);
            sync_from_session();
            if (focus_result.handled) {
                if (!is_accepted(focus_result.result.code)) {
                    report_result_failure(focus_result.result);
                    return;
                }
            }
        }
    }
}

bool VNM_TerminalSurface::event(QEvent* event)
{
    const bool trace_event =
        term::interaction_trace_enabled()             &&
        (event->type() == QEvent::ShortcutOverride    ||
         event->type() == QEvent::KeyRelease          ||
         event->type() == QEvent::FocusIn             ||
         event->type() == QEvent::FocusOut);
    QString details;
    if (trace_event) {
        details = QStringLiteral("type=%1 active_focus=%2 accepted_before=%3")
            .arg(static_cast<int>(event->type()))
            .arg(hasActiveFocus())
            .arg(event->isAccepted());
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyRelease) {
            const auto* key_event = static_cast<const QKeyEvent*>(event);
            details += QLatin1Char(' ') + term::interaction_trace_key_summary(*key_event);
        }
    }

    const bool handled = QQuickItem::event(event);
    if (trace_event) {
        details += QStringLiteral(" handled=%1 accepted_after=%2")
            .arg(handled)
            .arg(event->isAccepted());
        term::record_interaction_trace("surface", "event", details);
    }
    return handled;
}

void VNM_TerminalSurface::keyPressEvent(QKeyEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const std::uint64_t trace_id = term::interaction_trace_enabled()
        ? term::next_interaction_trace_correlation_id()
        : 0U;
    record_key_interaction_diagnostic("surface", "key-press", *event, trace_id);

    if (is_plain_copy_shortcut(*event)) {
        if (m_copy_shortcut_policy ==
            Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT)
        {
            if (m_private->has_copyable_selection_attachment() &&
                copy_selected_text_to_clipboard(Empty_selection_copy_policy::COPY))
            {
                event->accept();
                term::record_interaction_trace("surface", "key-route", QStringLiteral("copy"), trace_id);
                return;
            }
        }
        else
        if (m_copy_shortcut_policy == Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE) {
            if (m_private->has_copyable_selection_attachment()) {
                (void)copy_selected_text_to_clipboard(
                    Empty_selection_copy_policy::COPY);
            }
            event->accept();
            term::record_interaction_trace("surface", "key-route", QStringLiteral("copy-or-ignore"), trace_id);
            return;
        }
    }

    if (m_private->session != nullptr) {
        if (is_unmodified_page_scroll_key(*event)) {
            drain_backend_callback_events();
            if (m_private->session == nullptr) {
                event->ignore();
                return;
            }

            const int direction = event->key() == Qt::Key_PageUp ? 1 : -1;
            const std::shared_ptr<const term::Terminal_render_snapshot> published_snapshot =
                m_private->render_snapshot;
            if (published_snapshot != nullptr &&
                published_snapshot->viewport.active_buffer   == term::Terminal_buffer_id::PRIMARY &&
                published_snapshot->viewport.scrollback_rows >  0)
            {
                const int visible_rows =
                    std::max(1, published_snapshot->viewport.visible_rows);
                (void)scroll_viewport_lines_with_diagnostics(
                    direction * visible_rows,
                    QStringLiteral("key.page"));
                event->accept();
                term::record_interaction_trace("surface", "key-route", QStringLiteral("page-scroll"), trace_id);
                return;
            }
        }

        m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
        if (m_private->session == nullptr) {
            event->ignore();
            return;
        }
        term::Terminal_session* const route_session = m_private->session.get();
        const term::Terminal_key_event_result key_result =
            route_session->write_key_event(*event, trace_id);
        if (!key_result.handled) {
            event->ignore();
            term::record_interaction_trace("surface", "key-route", QStringLiteral("unhandled"), trace_id);
            return;
        }

        event->accept();
        if (term::interaction_trace_enabled()) {
            term::record_interaction_trace(
                "surface",
                "key-route",
                QStringLiteral("session sequence=%1 result=%2")
                    .arg(key_result.result.sequence)
                    .arg(static_cast<int>(key_result.result.code)),
                trace_id);
        }
        sync_from_session();
        if (!is_accepted(key_result.result.code)) {
            report_result_failure(key_result.result);
            return;
        }
        return;
    }

    // No active session: there is no backend to deliver input to, but the surface still
    // owns keyboard focus. Encode the key to decide whether it is terminal input: if it
    // would produce bytes, accept (swallow) it so Qt does not fall back to default item
    // navigation or a system beep; if it encodes to nothing, ignore it so ordinary
    // shortcut/navigation handling still runs. The encoded bytes are intentionally
    // discarded because no session exists to receive them.
    const QByteArray bytes =
        term::encode_terminal_key_event(*event, term::Terminal_input_mode_state{});
    if (bytes.isEmpty()) {
        event->ignore();
        term::record_interaction_trace("surface", "key-route", QStringLiteral("no-session-unhandled"), trace_id);
        return;
    }

    event->accept();
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "surface",
            "key-route",
            QStringLiteral("no-session %1").arg(term::interaction_trace_byte_summary(bytes)),
            trace_id);
    }
}

void VNM_TerminalSurface::mousePressEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "mouse",
            "press",
            QStringLiteral("button=%1 buttons=%2 modifiers=%3 keep_grab=%4 tracking=%5")
                .arg(static_cast<int>(event->button()))
                .arg(static_cast<int>(event->buttons()))
                .arg(static_cast<int>(event->modifiers()))
                .arg(keepMouseGrab())
                .arg(snapshot_has_terminal_mouse_tracking(m_private->render_snapshot)));
    }
    event->ignore();
    set_hyperlink_hover_position(event->position());
    dismiss_row_timestamp_tooltip();

    forceActiveFocus(Qt::MouseFocusReason);
    drain_backend_callback_events();

    const term::Terminal_mouse_button button = terminal_mouse_button(event->button());
    std::shared_ptr<const term::Terminal_render_snapshot> published_snapshot =
        m_private->render_snapshot;
    std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        published_snapshot,
        m_private->cell_metrics,
        event->position());
    const bool force_local_selection = local_selection_override(event->modifiers());
    std::optional<term::terminal_grid_position_t> logical_position;
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled || term::interaction_trace_enabled(),
            QStringLiteral("mouse-press"),
            reason,
            event->position(),
            position,
            logical_position,
            event->button(),
            event->buttons(),
            event->modifiers(),
            m_private->selection_drag_active,
            m_private->selection_drag_moved,
            m_private->selection_drag_cancelled,
            event->isAccepted());
    };
    trace_decision(QStringLiteral("entry"));

    if (event->button() == Qt::LeftButton &&
        m_private->hyperlink_activation_gesture_active)
    {
        m_private->clear_hyperlink_activation_state();
    }

    if (event->button() == Qt::LeftButton &&
        explicit_hyperlink_activation_modifiers(event->modifiers()) &&
        m_private->mouse_reporting_pressed_buttons == Qt::NoButton)
    {
        const std::optional<Surface_hyperlink_target> hyperlink =
            explicit_hyperlink_for_local_point(
                published_snapshot,
                m_private->cell_metrics,
                event->position());
        if (hyperlink.has_value()) {
            m_private->clear_selection_drag_state();
            m_private->hyperlink_activation_target         = *hyperlink;
            m_private->hyperlink_activation_gesture_active = true;
            event->accept();
            trace_decision(QStringLiteral("explicit-hyperlink"));
            return;
        }
    }

    if (!force_local_selection &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr &&
        position.has_value()                &&
        button                   != term::Terminal_mouse_button::NONE)
    {
        const Published_mouse_report_write_result encoded =
            m_private->encode_published_mouse_report(
                term::Terminal_mouse_event_kind::PRESS,
                button,
                *position,
                event->modifiers(),
                published_snapshot);
        std::optional<VNM_TerminalSurface::Private::Pending_published_mouse_report> report;
        if (encoded.encoded) {
            report = m_private->pending_published_mouse_report(
                term::Terminal_mouse_event_kind::PRESS,
                encoded);
        }

        Published_mouse_report_attempt mouse_write{
            Published_mouse_report_attempt_status::NOT_ENCODED,
            std::nullopt,
            encoded.position,
        };
        if (report.has_value()) {
            if (m_private->force_pending_published_mouse_report_block_for_testing()) {
                mouse_write = {
                    Published_mouse_report_attempt_status::CALLBACKS_PENDING,
                    std::nullopt,
                    report->position,
                };
            }
            else
            if (m_private->pending_published_mouse_reports.empty()) {
                mouse_write =
                    m_private->try_write_pending_published_mouse_report(*report);
            }
            else {
                mouse_write = {
                    Published_mouse_report_attempt_status::CALLBACKS_PENDING,
                    std::nullopt,
                    report->position,
                };
            }
        }

        if ((mouse_write.status == Published_mouse_report_attempt_status::WRITTEN ||
             mouse_write.status == Published_mouse_report_attempt_status::CALLBACKS_PENDING) &&
            mouse_write.position.has_value())
        {
            m_private->mouse_reporting_pressed_buttons |= event->button();
            m_private->mouse_reporting_drag_button      = button;
            m_private->mouse_reporting_last_position    = *mouse_write.position;
            position                                    = mouse_write.position;
            m_private->clear_selection_drag_state();
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("clear"),
                std::nullopt,
                logical_position,
                std::nullopt,
                false);
            if (m_private->session != nullptr) {
                m_private->clear_selection_with_sync(*this);
            }
            event->accept();
            if (mouse_write.status ==
                Published_mouse_report_attempt_status::CALLBACKS_PENDING)
            {
                Q_ASSERT(report.has_value());
                m_private->pending_published_mouse_reports.push_back(*report);
                (void)m_private->retry_pending_published_mouse_reports(*this, true);
                sync_from_session();
            }
            else {
                sync_from_session();
                if (mouse_write.result.has_value()) {
                    if (!is_accepted(mouse_write.result->code)) {
                        report_result_failure(*mouse_write.result);
                        return;
                    }
                }
            }
            trace_decision(QStringLiteral("mouse-reporting"));
            return;
        }
        if (m_private->session != nullptr &&
            m_private->session->process_state() == term::Terminal_process_state::RUNNING &&
            snapshot_has_terminal_mouse_tracking(published_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(published_snapshot))
        {
            m_private->mouse_reporting_pressed_buttons |= event->button();
            m_private->mouse_reporting_drag_button      = button;
            m_private->mouse_reporting_last_position    = *position;
            m_private->clear_selection_drag_state();
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("clear"),
                std::nullopt,
                logical_position,
                std::nullopt,
                false);
            m_private->clear_selection_with_sync(*this);
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("mouse-reporting"));
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        if (paste_clipboard_text()) {
            event->accept();
            trace_decision(QStringLiteral("right-paste"));
            return;
        }
    }

    if (event->button() != Qt::LeftButton || m_private->session == nullptr) {
        trace_decision(QStringLiteral("not-left-or-no-session"));
        return;
    }

    if (m_private->session->render_publication_blocked()) {
        trace_decision(QStringLiteral("publication-blocked"));
        return;
    }

    if (!position.has_value()) {
        trace_decision(QStringLiteral("out-of-grid"));
        return;
    }

    logical_position = logical_grid_position_for_viewport_cell(
        m_private->render_snapshot,
        *position);
    if (!logical_position.has_value()) {
        trace_decision(QStringLiteral("viewport-mapping"));
        return;
    }

    const std::optional<term::terminal_selection_source_identity_t> source =
        m_private->session->published_selection_source_identity();
    if (!source.has_value() || m_private->render_snapshot == nullptr ||
        !selection_source_matches_snapshot(*source, *m_private->render_snapshot))
    {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-press source-mismatch reason=%1 anchor=source{none} current=%2 %3")
                    .arg(selection_trace_source_snapshot_mismatch_reason(
                        source,
                        m_private->render_snapshot))
                    .arg(selection_trace_source_identity(source))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
        }
        trace_decision(QStringLiteral("source-mismatch"));
        return;
    }

    const std::optional<term::terminal_selection_drag_press_provenance_t> provenance =
        m_private->session->begin_selection_drag_provenance(
            *logical_position,
            *source);
    if (!provenance.has_value()) {
        trace_decision(QStringLiteral("press-provenance-unavailable"));
        return;
    }

    m_private->selection_anchor                = *logical_position;
    m_private->selection_drag_press_provenance = *provenance;
    m_private->selection_drag_active           = true;
    m_private->selection_drag_moved            = false;
    m_private->selection_drag_cancelled        = false;
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("start"),
        m_private->selection_anchor,
        logical_position,
        std::nullopt,
        false);
    m_private->clear_selection_with_sync(*this);
    event->accept();
    sync_from_session();
    if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface mouse-press source anchor=%1 current=%2 %3")
                .arg(selection_trace_source_identity(
                    m_private->selection_drag_press_provenance->source))
                .arg(selection_trace_source_identity(source))
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
    }
    trace_decision(QStringLiteral("clear-on-click"));
}

void VNM_TerminalSurface::mouseMoveEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "mouse",
            "move",
            QStringLiteral("buttons=%1 modifiers=%2 keep_grab=%3 tracking=%4 drag=%5")
                .arg(static_cast<int>(event->buttons()))
                .arg(static_cast<int>(event->modifiers()))
                .arg(keepMouseGrab())
                .arg(snapshot_has_terminal_mouse_tracking(m_private->render_snapshot))
                .arg(m_private->selection_drag_active));
    }
    event->ignore();
    set_hyperlink_hover_position(event->position());
    // Same positional guard as hover moves: a move event repeated at an
    // unchanged position is not user motion and must not dismiss.
    if (row_timestamp_tooltip_pointer_moved(event->position())) {
        dismiss_row_timestamp_tooltip();
    }
    std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    std::optional<term::terminal_grid_position_t> viewport_position = position;
    std::optional<term::terminal_grid_position_t> logical_position;
    const bool force_local_selection = local_selection_override(event->modifiers());
    const bool terminal_mouse_grab_active =
        m_private->mouse_reporting_pressed_buttons != Qt::NoButton;
    const bool published_mouse_tracking =
        snapshot_has_terminal_mouse_tracking(m_private->render_snapshot);
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled || term::interaction_trace_enabled(),
            QStringLiteral("mouse-move"),
            reason,
            event->position(),
            viewport_position,
            logical_position,
            event->button(),
            event->buttons(),
            event->modifiers(),
            m_private->selection_drag_active,
            m_private->selection_drag_moved,
            m_private->selection_drag_cancelled,
            event->isAccepted());
    };
    trace_decision(QStringLiteral("entry"));

    if (m_private->hyperlink_activation_gesture_active &&
        (event->buttons() & Qt::LeftButton) != Qt::NoButton)
    {
        const std::optional<Surface_hyperlink_target> current_hyperlink =
            explicit_hyperlink_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
        if (!m_private->hyperlink_activation_target.has_value()          ||
            !current_hyperlink.has_value()                               ||
            !explicit_hyperlink_activation_modifiers(event->modifiers()) ||
            !same_surface_hyperlink_target(
                *m_private->hyperlink_activation_target,
                *current_hyperlink))
        {
            m_private->hyperlink_activation_target.reset();
        }
        event->accept();
        trace_decision(QStringLiteral("explicit-hyperlink"));
        return;
    }
    if (m_private->hyperlink_activation_gesture_active) {
        m_private->clear_hyperlink_activation_state();
    }

    if (!m_private->selection_drag_active &&
        (!force_local_selection || terminal_mouse_grab_active)       &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr                          &&
        (published_mouse_tracking || terminal_mouse_grab_active))
    {
        if (m_private->block_terminal_input_behind_pending_published_mouse_report(*this)) {
            trace_decision(QStringLiteral("mouse-reporting-callbacks-pending"));
            return;
        }
        if (m_private->session == nullptr) {
            trace_decision(QStringLiteral("no-session"));
            return;
        }

        std::optional<term::terminal_grid_position_t> report_position = position;
        if (!report_position.has_value() && terminal_mouse_grab_active) {
            report_position = m_private->mouse_reporting_last_position.has_value()
                ? m_private->mouse_reporting_last_position
                : clamped_grid_position_for_local_point(
                    m_private->render_snapshot,
                    m_private->cell_metrics,
                    event->position());
        }

        if (report_position.has_value()) {
            const Qt::MouseButtons active_buttons =
                event->buttons() & m_private->mouse_reporting_pressed_buttons;
            term::Terminal_mouse_button button = terminal_mouse_button(
                active_buttons,
                m_private->mouse_reporting_drag_button);
            if (button == term::Terminal_mouse_button::NONE) {
                button = terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
            }
            m_private->mouse_reporting_drag_button = button;

            const term::Terminal_mouse_event_kind kind =
                button == term::Terminal_mouse_button::NONE
                    ? term::Terminal_mouse_event_kind::MOVE
                    : term::Terminal_mouse_event_kind::DRAG;
            const std::optional<term::Terminal_mouse_event_result> mouse_result =
                m_private->session->try_write_mouse_event_without_backend_drain_if_callbacks_empty({
                    kind,
                    button,
                    report_position->row,
                    report_position->column,
                    event->modifiers(),
                });
            if (!mouse_result.has_value()) {
                viewport_position = report_position;
                trace_decision(QStringLiteral("mouse-reporting-callbacks-pending"));
                return;
            }
            if (mouse_result->handled) {
                m_private->mouse_reporting_last_position = *report_position;
                m_private->clear_selection_drag_state();
                record_surface_selection_drag_transcript(
                    m_private->transcript_recorder,
                    QStringLiteral("clear"),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    false);
                m_private->clear_selection_with_sync(*this);
                event->accept();
                sync_from_session();
                if (!is_accepted(mouse_result->result.code)) {
                    report_result_failure(mouse_result->result);
                    return;
                }
                viewport_position = report_position;
                trace_decision(QStringLiteral("mouse-reporting"));
                return;
            }
            if (m_private->session->process_state() == term::Terminal_process_state::RUNNING &&
                snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
                !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
            {
                m_private->mouse_reporting_last_position = *report_position;
                event->accept();
                viewport_position = report_position;
                trace_decision(QStringLiteral("mouse-reporting"));
                return;
            }
        }
    }

    if (!m_private->selection_drag_active                       ||
        !m_private->selection_anchor.has_value()                ||
        !m_private->selection_drag_press_provenance.has_value() ||
        m_private->session                  == nullptr          ||
        (event->buttons() & Qt::LeftButton) == Qt::NoButton)
    {
        trace_decision(QStringLiteral("drag-inactive"));
        return;
    }

    drain_backend_callback_events();
    if (m_private->session == nullptr) {
        trace_decision(QStringLiteral("drag-inactive"));
        return;
    }

    position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    viewport_position = position;

    if (m_private->selection_drag_cancelled) {
        event->accept();
        trace_decision(QStringLiteral("drag-cancelled"));
        return;
    }

    if (m_private->session->render_publication_blocked()) {
        event->accept();
        trace_decision(QStringLiteral("publication-blocked"));
        return;
    }

    const std::optional<term::terminal_selection_source_identity_t> source =
        m_private->session->published_selection_source_identity();
    if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface mouse-move source anchor=%1 current=%2 %3")
                .arg(selection_trace_source_identity(
                    m_private->selection_drag_press_provenance->source))
                .arg(selection_trace_source_identity(source))
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
    }
    if (!source.has_value() ||
        m_private->render_snapshot == nullptr ||
        !selection_source_matches_snapshot(*source, *m_private->render_snapshot))
    {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-move source-mismatch snapshot_reason=%1 anchor_reason=%2")
                    .arg(selection_trace_source_snapshot_mismatch_reason(
                        source,
                        m_private->render_snapshot))
                    .arg(QStringLiteral("resolver-owned")));
        }
        m_private->selection_drag_cancelled = true;
        record_surface_selection_drag_transcript(
            m_private->transcript_recorder,
            QStringLiteral("cancel"),
            m_private->selection_anchor,
            logical_position,
            std::nullopt,
            m_private->selection_drag_moved);
        m_private->clear_selection_with_sync(*this);
        event->accept();
        sync_from_session();
        trace_decision(QStringLiteral("source-mismatch"));
        return;
    }

    viewport_position = position.has_value()
        ? position
        : clamped_grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            event->position());
    if (!viewport_position.has_value()) {
        trace_decision(QStringLiteral("out-of-grid"));
        return;
    }

    logical_position = logical_grid_position_for_viewport_cell(
        m_private->render_snapshot,
        *viewport_position);
    if (!logical_position.has_value()) {
        trace_decision(QStringLiteral("viewport-mapping"));
        return;
    }

    const term::Terminal_selection_drag_resolution proof =
        m_private->session->resolve_selection_drag_provenance(
            *m_private->selection_drag_press_provenance,
            m_private->selection_drag_moved);
    if (proof.status !=
            term::Terminal_selection_attachment_resolution_status::TRANSLATED ||
        !proof.resolved_position.has_value() ||
        !proof.source.has_value() ||
        (m_private->selection_drag_moved &&
            !proof.reconciled_proven_range.has_value()))
    {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-move gesture-proof status=%1 original=%2 current=%3")
                    .arg(QString::fromLatin1(
                        term::terminal_selection_attachment_resolution_status_token(
                            proof.status)))
                    .arg(selection_trace_grid_position(
                        m_private->selection_drag_press_provenance->original_position))
                    .arg(selection_trace_grid_position(logical_position)));
        }
        m_private->selection_drag_cancelled = true;
        record_surface_selection_drag_transcript(
            m_private->transcript_recorder,
            QStringLiteral("cancel"),
            m_private->selection_anchor,
            logical_position,
            std::nullopt,
            m_private->selection_drag_moved);
        if (m_private->selection_drag_moved) {
            m_private->detach_selection_visual_attachment_with_sync(
                *this);
        }
        else {
            m_private->clear_selection_with_sync(*this);
        }
        event->accept();
        sync_from_session();
        trace_decision(QStringLiteral("source-mismatch"));
        return;
    }

    m_private->selection_anchor = *proof.resolved_position;
    const term::Terminal_selection_range range =
        selection_range_for_drag(*proof.resolved_position, *logical_position);
    m_private->selection_drag_moved = true;
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("update"),
        m_private->selection_anchor,
        logical_position,
        range,
        true);
    m_private->session->set_selection_range_from_drained_published_source(
        range,
        *proof.source);
    m_private->session->record_selection_drag_proven_range();
    event->accept();
    sync_from_session();
    trace_decision(QStringLiteral("selection-range-set"));
}

void VNM_TerminalSurface::mouseReleaseEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "mouse",
            "release",
            QStringLiteral("button=%1 buttons=%2 modifiers=%3 keep_grab=%4 tracking=%5 drag=%6")
                .arg(static_cast<int>(event->button()))
                .arg(static_cast<int>(event->buttons()))
                .arg(static_cast<int>(event->modifiers()))
                .arg(keepMouseGrab())
                .arg(snapshot_has_terminal_mouse_tracking(m_private->render_snapshot))
                .arg(m_private->selection_drag_active));
    }
    event->ignore();
    set_hyperlink_hover_position(event->position());
    dismiss_row_timestamp_tooltip();
    drain_backend_callback_events();

    std::optional<term::terminal_grid_position_t> viewport_position;
    std::optional<term::terminal_grid_position_t> logical_position;
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled || term::interaction_trace_enabled(),
            QStringLiteral("mouse-release"),
            reason,
            event->position(),
            viewport_position,
            logical_position,
            event->button(),
            event->buttons(),
            event->modifiers(),
            m_private->selection_drag_active,
            m_private->selection_drag_moved,
            m_private->selection_drag_cancelled,
            event->isAccepted());
    };
    trace_decision(QStringLiteral("entry"));

    if (m_private->hyperlink_activation_gesture_active &&
        event->button() == Qt::LeftButton)
    {
        viewport_position = grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            event->position());
        const std::optional<Surface_hyperlink_target> current_hyperlink =
            explicit_hyperlink_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
        const bool request_activation =
            m_private->hyperlink_activation_target.has_value()          &&
            current_hyperlink.has_value()                               &&
            explicit_hyperlink_activation_modifiers(event->modifiers()) &&
            same_surface_hyperlink_target(
                *m_private->hyperlink_activation_target,
                *current_hyperlink);
        const QByteArray target = request_activation
            ? current_hyperlink->uri
            : QByteArray{};
        m_private->clear_hyperlink_activation_state();
        event->accept();
        trace_decision(
            request_activation
                ? QStringLiteral("explicit-hyperlink")
                : QStringLiteral("explicit-hyperlink-cancelled"));
        if (request_activation) {
            emit explicit_hyperlink_activation_requested(target);
        }
        return;
    }

    if (m_private->selection_drag_active &&
        event->button()    == Qt::LeftButton                   &&
        m_private->selection_anchor.has_value()                &&
        m_private->selection_drag_press_provenance.has_value() &&
        m_private->session != nullptr)
    {
        if (m_private->selection_drag_cancelled) {
            m_private->clear_selection_drag_state();
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("drag-cancelled"));
            return;
        }

        if (m_private->session->render_publication_blocked()) {
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("cancel"),
                m_private->selection_anchor,
                logical_position,
                std::nullopt,
                m_private->selection_drag_moved);
            if (!m_private->selection_drag_moved) {
                m_private->clear_selection_with_sync(*this);
            }
            else {
                m_private->detach_selection_visual_attachment_with_sync(
                    *this);
            }
            m_private->clear_selection_drag_state();
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("publication-blocked"));
            return;
        }

        const std::optional<term::terminal_selection_source_identity_t> source =
            m_private->session->published_selection_source_identity();
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral("surface mouse-release source anchor=%1 current=%2 %3")
                    .arg(selection_trace_source_identity(
                        m_private->selection_drag_press_provenance->source))
                    .arg(selection_trace_source_identity(source))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
        }
        if (!source.has_value() ||
            m_private->render_snapshot == nullptr ||
            !selection_source_matches_snapshot(*source, *m_private->render_snapshot))
        {
            if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
                write_selection_trace(m_selection_trace_enabled,
                    QStringLiteral(
                        "surface mouse-release source-mismatch snapshot_reason=%1 anchor_reason=%2")
                        .arg(selection_trace_source_snapshot_mismatch_reason(
                            source,
                            m_private->render_snapshot))
                        .arg(QStringLiteral("resolver-owned")));
            }
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("cancel"),
                m_private->selection_anchor,
                logical_position,
                std::nullopt,
                m_private->selection_drag_moved);
            m_private->clear_selection_drag_state();
            m_private->clear_selection_with_sync(*this);
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("source-mismatch"));
            return;
        }

        viewport_position = grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            event->position());
        viewport_position = viewport_position.has_value()
            ? viewport_position
            : clamped_grid_position_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
        if (viewport_position.has_value()) {
            logical_position = logical_grid_position_for_viewport_cell(
                m_private->render_snapshot,
                *viewport_position);
        }
        QString decision_reason = QStringLiteral("out-of-grid");
        if (logical_position.has_value()) {
            const term::Terminal_selection_drag_resolution proof =
                m_private->session->resolve_selection_drag_provenance(
                    *m_private->selection_drag_press_provenance,
                    m_private->selection_drag_moved);
            if (proof.status !=
                    term::Terminal_selection_attachment_resolution_status::TRANSLATED ||
                !proof.resolved_position.has_value() ||
                !proof.source.has_value() ||
                (m_private->selection_drag_moved &&
                    !proof.reconciled_proven_range.has_value()))
            {
                if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
                    write_selection_trace(m_selection_trace_enabled,
                        QStringLiteral(
                            "surface mouse-release gesture-proof status=%1 original=%2 current=%3")
                            .arg(QString::fromLatin1(
                                term::terminal_selection_attachment_resolution_status_token(
                                    proof.status)))
                            .arg(selection_trace_grid_position(
                                m_private->selection_drag_press_provenance->original_position))
                            .arg(selection_trace_grid_position(logical_position)));
                }
                record_surface_selection_drag_transcript(
                    m_private->transcript_recorder,
                    QStringLiteral("cancel"),
                    m_private->selection_anchor,
                    logical_position,
                    std::nullopt,
                    m_private->selection_drag_moved);
                if (m_private->selection_drag_moved) {
                    m_private->detach_selection_visual_attachment_with_sync(
                        *this);
                }
                else {
                    m_private->clear_selection_with_sync(*this);
                }
                m_private->clear_selection_drag_state();
                event->accept();
                sync_from_session();
                trace_decision(QStringLiteral("source-mismatch"));
                return;
            }

            m_private->selection_anchor = *proof.resolved_position;
            const term::Terminal_selection_range range =
                selection_range_for_drag(*proof.resolved_position, *logical_position);
            const bool selection_range_needed =
                m_private->selection_drag_moved ||
                *logical_position != *proof.resolved_position;
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("finish"),
                m_private->selection_anchor,
                logical_position,
                range,
                selection_range_needed);
            const Selection_drag_session_mutation mutation =
                selection_drag_session_mutation_for_drag(
                    *m_private->selection_anchor,
                    *logical_position,
                    m_private->selection_drag_moved);
            if (mutation.mutation == Selection_session_mutation::SET_SELECTION_RANGE) {
                Q_ASSERT(mutation.range.has_value());
                m_private->session->set_selection_range_from_drained_published_source(
                    *mutation.range,
                    *proof.source);
                m_private->session->record_selection_drag_proven_range();
                if (m_copy_on_select) {
                    (void)copy_selected_text_to_clipboard(
                        Empty_selection_copy_policy::SKIP_EMPTY_SELECTION);
                }
            }
            else {
                m_private->clear_selection_with_sync(*this);
            }
            decision_reason = QStringLiteral("selection-range-set");
        }
        else
        if (!m_private->selection_drag_moved) {
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("clear"),
                m_private->selection_anchor,
                logical_position,
                std::nullopt,
                false);
            m_private->clear_selection_with_sync(*this);
            decision_reason = QStringLiteral("clear-on-click");
        }

        m_private->clear_selection_drag_state();
        event->accept();
        sync_from_session();
        trace_decision(decision_reason);
        return;
    }

    term::Terminal_mouse_button button = terminal_mouse_button(event->button());
    if (button == term::Terminal_mouse_button::NONE) {
        button = m_private->mouse_reporting_drag_button;
    }
    const bool final_release =
        (event->buttons() &
            (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)) == Qt::NoButton;
    const bool force_local_selection = local_selection_override(event->modifiers());
    const bool terminal_mouse_grab_active =
        m_private->mouse_reporting_pressed_buttons != Qt::NoButton;

    if (m_mouse_reporting_policy == Mouse_reporting_policy::DISABLED ||
        m_private->session       == nullptr                          ||
        (force_local_selection && !terminal_mouse_grab_active))
    {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        const QString reason =
            m_mouse_reporting_policy == Mouse_reporting_policy::DISABLED
                ? QStringLiteral("mouse-reporting-disabled")
                : m_private->session == nullptr
                    ? QStringLiteral("no-session")
                    : QStringLiteral("local-selection");
        trace_decision(reason);
        return;
    }

    if (!terminal_mouse_grab_active) {
        if (final_release) {
            m_private->mouse_reporting_last_position.reset();
        }
        trace_decision(QStringLiteral("mouse-reporting-no-grab"));
        return;
    }

    const Qt::MouseButton released_qt_button = event->button();
    if (released_qt_button                                                == Qt::NoButton ||
        (m_private->mouse_reporting_pressed_buttons & released_qt_button) == Qt::NoButton ||
        terminal_mouse_button(released_qt_button)                         == term::Terminal_mouse_button::NONE)
    {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        trace_decision(QStringLiteral("mouse-reporting-invalid-button"));
        return;
    }

    button = terminal_mouse_button(released_qt_button);

    std::shared_ptr<const term::Terminal_render_snapshot> published_snapshot =
        m_private->render_snapshot;
    std::optional<term::terminal_grid_position_t> report_position;
    const auto refresh_report_position = [&] {
        published_snapshot = m_private->render_snapshot;
        viewport_position = grid_position_for_local_point(
            published_snapshot,
            m_private->cell_metrics,
            event->position());
        report_position = viewport_position;
        if (!report_position.has_value() && terminal_mouse_grab_active) {
            report_position = m_private->mouse_reporting_last_position.has_value()
                ? m_private->mouse_reporting_last_position
                : clamped_grid_position_for_local_point(
                    published_snapshot,
                    m_private->cell_metrics,
                    event->position());
        }
        viewport_position = report_position;
    };
    refresh_report_position();
    if (!report_position.has_value() || button == term::Terminal_mouse_button::NONE) {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        trace_decision(QStringLiteral("out-of-grid"));
        return;
    }

    m_private->mouse_reporting_last_position = *report_position;
    if (final_release) {
        m_private->clear_mouse_reporting_state();
    }
    else {
        m_private->mouse_reporting_pressed_buttons &= ~Qt::MouseButtons(released_qt_button);
        m_private->mouse_reporting_drag_button =
            terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
    }

    const Published_mouse_report_write_result encoded =
        m_private->encode_published_mouse_report(
            term::Terminal_mouse_event_kind::RELEASE,
            button,
            *report_position,
            event->modifiers(),
            published_snapshot);
    std::optional<VNM_TerminalSurface::Private::Pending_published_mouse_report> report;
    if (encoded.encoded) {
        report = m_private->pending_published_mouse_report(
            term::Terminal_mouse_event_kind::RELEASE,
            encoded);
    }

    Published_mouse_report_attempt mouse_write{
        Published_mouse_report_attempt_status::NOT_ENCODED,
        std::nullopt,
        encoded.position,
    };
    if (report.has_value()) {
        if (m_private->force_pending_published_mouse_report_block_for_testing()) {
            mouse_write = {
                Published_mouse_report_attempt_status::CALLBACKS_PENDING,
                std::nullopt,
                report->position,
            };
        }
        else
        if (m_private->pending_published_mouse_reports.empty()) {
            mouse_write = m_private->try_write_pending_published_mouse_report(*report);
        }
        else {
            mouse_write = {
                Published_mouse_report_attempt_status::CALLBACKS_PENDING,
                std::nullopt,
                report->position,
            };
        }
    }
    if (mouse_write.status != Published_mouse_report_attempt_status::WRITTEN &&
        mouse_write.status != Published_mouse_report_attempt_status::CALLBACKS_PENDING)
    {
        const bool non_sgr_mouse_tracking =
            m_private->session != nullptr &&
            m_private->session->process_state() == term::Terminal_process_state::RUNNING &&
            snapshot_has_terminal_mouse_tracking(published_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(published_snapshot);
        if (non_sgr_mouse_tracking)
        {
            event->accept();
        }
        trace_decision(
            non_sgr_mouse_tracking
                ? QStringLiteral("mouse-reporting-non-sgr")
                : QStringLiteral("mouse-reporting-unhandled"));
        return;
    }

    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("clear"),
        std::nullopt,
        logical_position,
        std::nullopt,
        false);
    if (m_private->session != nullptr) {
        m_private->clear_selection_with_sync(*this);
    }
    event->accept();
    if (mouse_write.status == Published_mouse_report_attempt_status::CALLBACKS_PENDING) {
        Q_ASSERT(report.has_value());
        m_private->pending_published_mouse_reports.push_back(*report);
        (void)m_private->retry_pending_published_mouse_reports(*this, true);
        sync_from_session();
    }
    else {
        sync_from_session();
        if (mouse_write.result.has_value()) {
            if (!is_accepted(mouse_write.result->code)) {
                report_result_failure(*mouse_write.result);
                return;
            }
        }
    }
    trace_decision(QStringLiteral("mouse-reporting"));
}

void VNM_TerminalSurface::hoverMoveEvent(QHoverEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    set_hyperlink_hover_position(event->position());
    // Actual pointer motion hides a shown tooltip; a fresh idle period over
    // the new position re-requests it. Same-position hover events are scene
    // redeliveries, not motion, and pass through without touching the tooltip
    // state. This runs before the mouse-reporting early returns below so the
    // tooltip works regardless of reporting policy.
    if (row_timestamp_tooltip_pointer_moved(event->position())) {
        dismiss_row_timestamp_tooltip();
        if (m_row_timestamp_tooltip_enabled) {
            m_private->row_timestamp_tooltip_timer.start();
        }
    }
    if (m_mouse_reporting_policy == Mouse_reporting_policy::DISABLED ||
        m_private->session       == nullptr                          ||
        local_selection_override(event->modifiers()))
    {
        return;
    }

    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    if (!position.has_value()) {
        return;
    }

    if (!snapshot_has_terminal_mouse_tracking(m_private->render_snapshot)) {
        return;
    }

    if (m_private->block_terminal_input_behind_pending_published_mouse_report(*this)) {
        return;
    }
    if (m_private->session == nullptr) {
        return;
    }

    const std::optional<term::Terminal_mouse_event_result> mouse_result =
        m_private->session->try_write_mouse_event_without_backend_drain_if_callbacks_empty({
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            position->row,
            position->column,
            event->modifiers(),
        });
    if (!mouse_result.has_value()) {
        event->accept();
        return;
    }
    if (!mouse_result->handled) {
        if (m_private->session->process_state() == term::Terminal_process_state::RUNNING &&
            snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
        {
            event->accept();
        }
        return;
    }

    event->accept();
    sync_from_session();
    if (!is_accepted(mouse_result->result.code)) {
        report_result_failure(mouse_result->result);
        return;
    }
}

void VNM_TerminalSurface::hoverLeaveEvent(QHoverEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    set_hyperlink_hover_position(std::nullopt);
    dismiss_row_timestamp_tooltip();
    QQuickItem::hoverLeaveEvent(event);
}

void VNM_TerminalSurface::wheelEvent(QWheelEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    dismiss_row_timestamp_tooltip();

    if (m_wheel_trace_enabled) {
        record_surface_wheel_ingress_transcript(
            m_private->transcript_recorder,
            QStringLiteral("surface.text_area.wheel"),
            *event);
    }

    int    backend_drain_calls          = 0;
    qint64 backend_drain_elapsed_ns     = 0;
    bool   trace_written                = false;
    bool   local_scroll_attempted       = false;
    bool   local_scroll_possible        = false;
    bool   local_scroll_intent_recorded = false;
    bool   local_scroll_applied         = false;
    bool   deferred_intent_recorded     = false;
    bool   application_route_attempted  = false;
    bool   alternate_screen             = false;
    bool   live_sgr_mouse_reporting     = false;
    bool   published_sgr_mouse_reporting = false;
    bool   published_mouse_tracking      = false;
    bool   has_scroll_result             = false;
    bool   has_viewport_before           = false;
    bool   has_viewport_after            = false;
    int    wheel_steps                   = 0;
    int    effective_line_delta          = 0;
    int    alternate_key_count           = 0;
    int    mouse_report_count            = 0;
    qreal  trace_angle_remainder         = 0.0;
    qreal  trace_pixel_remainder         = 0.0;
    QString local_scroll_block_reason;
    term::Terminal_viewport_scroll_result trace_scroll_result;
    term::Terminal_viewport_state         trace_viewport_before;
    term::Terminal_viewport_state         trace_viewport_after;
    std::optional<bool>                   trace_visible_scroll_applied;

    const auto finish_trace =
        [&](const QString& route, const QString& outcome, bool accepted) {
            if (!m_wheel_trace_enabled || trace_written) {
                return;
            }
            trace_written = true;

            QJsonObject object =
                wheel_trace_event_object(QStringLiteral("surface.text_area.wheel"), *event);
            object.insert(QStringLiteral("route"), route);
            object.insert(QStringLiteral("outcome"), outcome);
            object.insert(QStringLiteral("accepted"), accepted);
            object.insert(QStringLiteral("wheel_steps"), wheel_steps);
            object.insert(QStringLiteral("effective_line_delta"), effective_line_delta);
            object.insert(QStringLiteral("angle_remainder"), trace_angle_remainder);
            object.insert(QStringLiteral("pixel_remainder"), trace_pixel_remainder);
            object.insert(QStringLiteral("wheel_event_policy"), wheel_event_policy_name(m_wheel_event_policy));
            object.insert(
                QStringLiteral("alternate_screen_wheel_policy"),
                alternate_screen_wheel_policy_name(m_alternate_screen_wheel_policy));
            object.insert(
                QStringLiteral("mouse_reporting_policy"),
                mouse_reporting_policy_name(m_mouse_reporting_policy));

            const bool session_present = m_private->session != nullptr;
            object.insert(QStringLiteral("session_present"), session_present);
            const bool render_publication_blocked =
                session_present && m_private->session->render_publication_blocked();
            object.insert(
                QStringLiteral("render_publication_blocked"),
                render_publication_blocked);
            object.insert(
                QStringLiteral("published_synchronized_output"),
                m_private->render_snapshot != nullptr &&
                    m_private->render_snapshot->modes.synchronized_output);
            object.insert(QStringLiteral("backend_drain_calls"), backend_drain_calls);
            object.insert(QStringLiteral("backend_drain_elapsed_ns"), backend_drain_elapsed_ns);
            object.insert(
                QStringLiteral("protocol_state_stale"),
                m_private->session != nullptr &&
                    m_private->session->has_pending_backend_callback_events());

            bool trace_alternate_screen = alternate_screen;

            published_sgr_mouse_reporting =
                published_sgr_mouse_reporting ||
                snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
            published_mouse_tracking =
                published_mouse_tracking ||
                snapshot_has_terminal_mouse_tracking(m_private->render_snapshot);
            if (m_private->render_snapshot != nullptr) {
                const term::Terminal_viewport_state& published_viewport =
                    m_private->render_snapshot->viewport;
                trace_alternate_screen =
                    trace_alternate_screen ||
                    published_viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE;
                insert_wheel_trace_viewport(
                    object,
                    QStringLiteral("published_viewport"),
                    published_viewport);
            }

            object.insert(QStringLiteral("alternate_screen"), trace_alternate_screen);
            object.insert(QStringLiteral("local_scroll_attempted"), local_scroll_attempted);
            object.insert(QStringLiteral("local_scroll_possible"), local_scroll_possible);
            object.insert(
                QStringLiteral("local_scroll_intent_recorded"),
                local_scroll_intent_recorded);
            term::insert_wheel_trace_scroll_publication_fields(
                object,
                local_scroll_applied,
                render_publication_blocked,
                trace_visible_scroll_applied,
                deferred_intent_recorded);
            object.insert(
                QStringLiteral("application_route_attempted"),
                application_route_attempted);
            object.insert(QStringLiteral("live_sgr_mouse_reporting"), live_sgr_mouse_reporting);
            object.insert(
                QStringLiteral("published_sgr_mouse_reporting"),
                published_sgr_mouse_reporting);
            object.insert(QStringLiteral("published_mouse_tracking"), published_mouse_tracking);
            object.insert(QStringLiteral("alternate_key_count"), alternate_key_count);
            object.insert(QStringLiteral("mouse_report_count"), mouse_report_count);
            if (!local_scroll_block_reason.isEmpty()) {
                object.insert(
                    QStringLiteral("local_scroll_block_reason"),
                    local_scroll_block_reason);
            }
            if (has_scroll_result) {
                object.insert(
                    QStringLiteral("scroll_action"),
                    VNM_TerminalSurface::scroll_action_name(
                        public_scroll_action(trace_scroll_result.action)));
                object.insert(
                    QStringLiteral("applied_line_delta"),
                    trace_scroll_result.applied_line_delta);
            }
            if (has_viewport_before) {
                insert_wheel_trace_viewport(
                    object,
                    QStringLiteral("viewport_before"),
                    trace_viewport_before);
            }
            if (has_viewport_after) {
                insert_wheel_trace_viewport(
                    object,
                    QStringLiteral("viewport_after"),
                    trace_viewport_after);
            }

            record_surface_wheel_trace_transcript(
                m_private->transcript_recorder,
                std::move(object));
        };

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        const int steps = term::vertical_wheel_steps(
            *event,
            term::k_angle_delta_per_wheel_step,
            m_private->wheel_zoom_angle_remainder,
            m_private->wheel_zoom_pixel_remainder);
        wheel_steps           = steps;
        trace_angle_remainder = m_private->wheel_zoom_angle_remainder;
        trace_pixel_remainder = m_private->wheel_zoom_pixel_remainder;
        if (steps == 0) {
            if (term::has_vertical_wheel_delta(*event)) {
                event->accept();
                finish_trace(
                    QStringLiteral("control_zoom"),
                    QStringLiteral("sub_step_accumulated"),
                    true);
                return;
            }

            event->ignore();
            finish_trace(
                QStringLiteral("control_zoom"),
                QStringLiteral("zero_vertical_delta"),
                false);
            return;
        }

        const qreal requested_font_size = term::font_size_after_wheel_zoom(m_font_size, steps);
        const qreal previous_font_size  = m_font_size;
        set_font_size(requested_font_size);
        if (term::has_vertical_wheel_delta(*event) ||
            !same_property_value(previous_font_size, m_font_size))
        {
            event->accept();
        }
        else {
            event->ignore();
        }
        finish_trace(
            QStringLiteral("control_zoom"),
            same_property_value(previous_font_size, m_font_size)
                ? QStringLiteral("zoom_clamped_noop")
                : QStringLiteral("zoom_applied"),
            event->isAccepted());
        return;
    }

    if (m_private->session == nullptr) {
        QQuickItem::wheelEvent(event);
        finish_trace(
            QStringLiteral("qt_fallback"),
            QStringLiteral("no_session"),
            event->isAccepted());
        return;
    }

    m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
    if (m_private->session == nullptr) {
        QQuickItem::wheelEvent(event);
        finish_trace(
            QStringLiteral("qt_fallback"),
            QStringLiteral("no_session"),
            event->isAccepted());
        return;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
        m_private->render_snapshot;
    const bool protocol_state_stale =
        m_private->session != nullptr &&
        m_private->session->has_pending_backend_callback_events();
    const term::Terminal_input_mode_state published_input_modes =
        wheel_snapshot != nullptr
            ? input_modes_from_render_snapshot(*wheel_snapshot)
            : term::Terminal_input_mode_state{};

    const auto route_wheel_to_application = [&]() -> bool {
        application_route_attempted = true;
        if (!term::has_vertical_wheel_delta(*event)) {
            return false;
        }

        if (m_private->session == nullptr) {
            event->ignore();
            finish_trace(
                QStringLiteral("application"),
                QStringLiteral("no_session"),
                false);
            return true;
        }

        const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
            wheel_snapshot,
            m_private->cell_metrics,
            event->position());
        alternate_screen =
            wheel_snapshot != nullptr &&
            wheel_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE;
        const auto route_alternate_wheel_to_keys = [&]() -> bool {
            if (!alternate_screen) {
                return false;
            }

            if (protocol_state_stale) {
                event->accept();
                finish_trace(
                    QStringLiteral("alternate_screen"),
                    QStringLiteral("protocol_state_stale"),
                    true);
                return true;
            }

            const int steps = term::vertical_wheel_steps(
                *event,
                m_private->cell_metrics.height,
                m_private->wheel_mouse_angle_remainder,
                m_private->wheel_mouse_pixel_remainder);
            wheel_steps           = steps;
            trace_angle_remainder = m_private->wheel_mouse_angle_remainder;
            trace_pixel_remainder = m_private->wheel_mouse_pixel_remainder;
            if (steps == 0) {
                event->accept();
                finish_trace(
                    QStringLiteral("alternate_screen"),
                    QStringLiteral("sub_step_accumulated"),
                    true);
                return true;
            }

            const bool page_keys =
                m_alternate_screen_wheel_policy ==
                Alternate_screen_wheel_policy::PAGE_KEYS;
            const int key_count = page_keys
                ? std::abs(steps)
                : std::abs(event->angleDelta().y() != 0
                    ? steps * term::k_plain_scroll_lines_per_angle_step
                    : steps);
            const int key = steps > 0
                ? (page_keys ? Qt::Key_PageUp : Qt::Key_Up)
                : (page_keys ? Qt::Key_PageDown : Qt::Key_Down);

            bool handled = false;
            bool callbacks_pending = false;
            term::Terminal_session_result last_result;
            QKeyEvent key_event(QEvent::KeyPress, key, Qt::NoModifier, {});
            const QByteArray key_bytes =
                term::encode_terminal_key_event(key_event, published_input_modes);
            if (key_bytes.isEmpty()) {
                return false;
            }

            for (int i = 0; i < key_count; ++i) {
                handled = true;
                const std::optional<term::Terminal_session_result> write_result =
                    m_private->session
                        ->try_write_user_bytes_without_backend_drain_if_callbacks_empty(
                            key_bytes);
                if (!write_result.has_value()) {
                    callbacks_pending = true;
                    break;
                }

                last_result = *write_result;
                ++alternate_key_count;
                if (!is_accepted(last_result.code)) {
                    break;
                }
            }

            if (!handled) {
                return false;
            }

            event->accept();
            sync_from_session();
            if (!callbacks_pending && !is_accepted(last_result.code)) {
                report_result_failure(last_result);
                return true;
            }
            finish_trace(
                QStringLiteral("alternate_screen"),
                callbacks_pending
                    ? QStringLiteral("protocol_callbacks_pending")
                    : QStringLiteral("alternate_screen_keys"),
                true);
            return true;
        };

        if (m_alternate_screen_wheel_policy !=
            Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST &&
            route_alternate_wheel_to_keys())
        {
            return true;
        }

        published_sgr_mouse_reporting =
            snapshot_has_sgr_mouse_reporting(wheel_snapshot);
        published_mouse_tracking =
            snapshot_has_terminal_mouse_tracking(wheel_snapshot);
        if (m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
            position.has_value() &&
            published_sgr_mouse_reporting)
        {
            if (protocol_state_stale) {
                event->accept();
                finish_trace(
                    QStringLiteral("mouse_tracking"),
                    QStringLiteral("protocol_state_stale"),
                    true);
                return true;
            }

            const int steps = term::vertical_wheel_steps(
                *event,
                m_private->cell_metrics.height,
                m_private->wheel_mouse_angle_remainder,
                m_private->wheel_mouse_pixel_remainder);
            wheel_steps           = steps;
            trace_angle_remainder = m_private->wheel_mouse_angle_remainder;
            trace_pixel_remainder = m_private->wheel_mouse_pixel_remainder;
            if (steps == 0) {
                event->accept();
                finish_trace(
                    QStringLiteral("mouse_tracking"),
                    QStringLiteral("sub_step_accumulated"),
                    true);
                return true;
            }

            const term::Terminal_mouse_button button = steps > 0
                ? term::Terminal_mouse_button::WHEEL_UP
                : term::Terminal_mouse_button::WHEEL_DOWN;
            bool handled = false;
            bool callbacks_pending = false;
            term::Terminal_session_result last_result;
            for (int i = 0; i < std::abs(steps); ++i) {
                const QByteArray mouse_bytes =
                    term::encode_terminal_mouse_event(
                        {
                        term::Terminal_mouse_event_kind::WHEEL,
                        button,
                        position->row,
                        position->column,
                        event->modifiers(),
                        },
                        published_input_modes);
                if (mouse_bytes.isEmpty()) {
                    break;
                }

                handled = true;
                const std::optional<term::Terminal_session_result> write_result =
                    m_private->session
                        ->try_write_user_bytes_without_backend_drain_if_callbacks_empty(
                            mouse_bytes);
                if (!write_result.has_value()) {
                    callbacks_pending = true;
                    break;
                }

                ++mouse_report_count;
                last_result = *write_result;
                if (!is_accepted(last_result.code)) {
                    break;
                }
            }
            if (handled) {
                event->accept();
                sync_from_session();
                if (!callbacks_pending && !is_accepted(last_result.code)) {
                    report_result_failure(last_result);
                    return true;
                }
                finish_trace(
                    QStringLiteral("mouse_tracking"),
                    callbacks_pending
                        ? QStringLiteral("protocol_callbacks_pending")
                        : QStringLiteral("mouse_tracking_route"),
                    true);
                return true;
            }

            return false;
        }

        if (m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
            position.has_value() &&
            published_mouse_tracking)
        {
            event->accept();
            finish_trace(
                QStringLiteral("mouse_tracking"),
                QStringLiteral("mouse_tracking_swallow"),
                true);
            return true;
        }

        return route_alternate_wheel_to_keys();
    };

    const auto scroll_viewport_locally = [&](bool trace_on_failure) -> bool {
        local_scroll_attempted = true;
        if (!term::has_vertical_wheel_delta(*event)) {
            event->ignore();
            local_scroll_block_reason = QStringLiteral("zero_vertical_delta");
            if (trace_on_failure) {
                finish_trace(
                    QStringLiteral("local_scroll"),
                    QStringLiteral("zero_vertical_delta"),
                    false);
            }
            return true;
        }

        if (m_private->session == nullptr) {
            event->ignore();
            if (trace_on_failure) {
                finish_trace(
                    QStringLiteral("local_scroll"),
                    QStringLiteral("no_session"),
                    false);
            }
            return false;
        }

        if (wheel_snapshot == nullptr) {
            event->ignore();
            local_scroll_block_reason = QStringLiteral("no_publication");
            if (trace_on_failure) {
                finish_trace(
                    QStringLiteral("local_scroll"),
                    QStringLiteral("no_publication"),
                    false);
            }
            return false;
        }

        const term::Terminal_viewport_state viewport = wheel_snapshot->viewport;
        const int scroll_direction = vertical_wheel_direction(*event);
        const bool immediate_projection_hold =
            viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
            m_private->session->effective_synchronized_output_scroll_policy() ==
                term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION &&
            m_private->session->render_publication_blocked();
        local_scroll_possible =
            viewport_state_can_scroll_locally(viewport, scroll_direction) ||
            immediate_projection_hold;
        if (!local_scroll_possible) {
            event->ignore();
            local_scroll_block_reason =
                wheel_trace_local_scroll_block_reason(viewport, scroll_direction);
            if (trace_on_failure) {
                finish_trace(
                    QStringLiteral("local_scroll"),
                    wheel_trace_local_scroll_block_outcome(local_scroll_block_reason),
                    false);
            }
            return false;
        }

        const int line_delta = term::vertical_wheel_steps(
            *event,
            m_private->cell_metrics.height,
            m_private->wheel_scroll_angle_remainder,
            m_private->wheel_scroll_pixel_remainder);
        wheel_steps = line_delta;
        trace_angle_remainder = m_private->wheel_scroll_angle_remainder;
        trace_pixel_remainder = m_private->wheel_scroll_pixel_remainder;
        effective_line_delta =
            event->angleDelta().y() != 0
                ? line_delta * term::k_plain_scroll_lines_per_angle_step
                : line_delta;
        if (effective_line_delta == 0) {
            event->accept();
            finish_trace(
                QStringLiteral("local_scroll"),
                QStringLiteral("sub_step_accumulated"),
                true);
            return true;
        }

        const term::Terminal_viewport_state viewport_before = viewport;
        trace_viewport_before = viewport_before;
        has_viewport_before   = true;
        record_surface_scroll_intent_transcript(
            m_private->transcript_recorder,
            QStringLiteral("surface.text_area.wheel"),
            effective_line_delta,
            std::nullopt,
            viewport_before);
        local_scroll_intent_recorded = true;
        const term::Terminal_viewport_scroll_result scroll_result =
            m_private->session->scroll_viewport_lines_from_published_state(
                effective_line_delta,
                viewport_before);
        trace_scroll_result = scroll_result;
        has_scroll_result   = true;
        deferred_intent_recorded =
            scroll_result.action ==
                term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED;
        if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED ||
            deferred_intent_recorded)
        {
            sync_from_session();
            trace_viewport_after =
                m_private->render_snapshot != nullptr
                    ? m_private->render_snapshot->viewport
                    : m_private->session->viewport_state();
            has_viewport_after   = true;
            local_scroll_applied =
                scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
            record_surface_scroll_transcript(
                m_private->transcript_recorder,
                QStringLiteral("surface.text_area.wheel"),
                effective_line_delta,
                std::nullopt,
                scroll_result,
                viewport_before,
                trace_viewport_after);
            event->accept();
            const bool render_publication_blocked_after_scroll =
                m_private->session->render_publication_blocked();
            const bool public_projection_scroll_visible =
                render_publication_blocked_after_scroll &&
                m_private->render_snapshot != nullptr &&
                m_private->render_snapshot->basis ==
                    term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
                m_private->render_snapshot->purpose ==
                    term::Terminal_render_snapshot_purpose::SCROLL &&
                m_private->render_snapshot->public_scroll_diagnostics.visible_scroll_applied;
            const bool visible_scroll_applied =
                local_scroll_applied &&
                (!render_publication_blocked_after_scroll || public_projection_scroll_visible);
            trace_visible_scroll_applied = visible_scroll_applied;
            const QString outcome =
                deferred_intent_recorded
                    ? QStringLiteral("public_projection_deferred_intent_recorded")
                    : public_projection_scroll_visible
                    ? QStringLiteral("public_projection_scroll_visible")
                    : visible_scroll_applied
                    ? QStringLiteral("local_scroll_applied")
                    : QStringLiteral("local_scroll_publication_deferred");
            finish_trace(
                QStringLiteral("local_scroll"),
                outcome,
                true);
            return true;
        }

        event->ignore();
        if (trace_on_failure) {
            const QString noop_outcome = wheel_trace_local_scroll_noop_outcome(
                viewport_before,
                scroll_result.action,
                m_private->session->render_publication_blocked(),
                wheel_snapshot->modes.synchronized_output);
            local_scroll_block_reason = noop_outcome;
            finish_trace(
                QStringLiteral("local_scroll"),
                noop_outcome,
                false);
        }
        return false;
    };

    if (m_wheel_event_policy == Wheel_event_policy::LOCAL_SCROLLBACK_FIRST) {
        if (m_private->session == nullptr) {
            event->ignore();
            finish_trace(
                QStringLiteral("local_scroll"),
                QStringLiteral("no_session"),
                false);
            return;
        }

        const bool try_local_scroll_first =
            wheel_snapshot != nullptr &&
            viewport_state_can_scroll_locally(
                wheel_snapshot->viewport,
                vertical_wheel_direction(*event));
        if (try_local_scroll_first && scroll_viewport_locally(false)) {
            return;
        }
        if (route_wheel_to_application()) {
            return;
        }
        if (!try_local_scroll_first && scroll_viewport_locally(true)) {
            return;
        }
        event->ignore();
        QString final_outcome = QStringLiteral("application_unhandled");
        if (has_scroll_result) {
            final_outcome = wheel_trace_local_scroll_noop_outcome(
                trace_viewport_before,
                trace_scroll_result.action,
                m_private->session != nullptr &&
                    m_private->session->render_publication_blocked(),
                wheel_snapshot != nullptr &&
                    wheel_snapshot->modes.synchronized_output);
            if (local_scroll_block_reason.isEmpty()) {
                local_scroll_block_reason = final_outcome;
            }
        }
        finish_trace(
            QStringLiteral("local_scroll"),
            final_outcome,
            false);
        return;
    }

    if (m_wheel_event_policy == Wheel_event_policy::LOCAL_SCROLLBACK_ONLY) {
        (void)scroll_viewport_locally(true);
        return;
    }

    if (route_wheel_to_application()) {
        return;
    }

    (void)scroll_viewport_locally(true);
}

void VNM_TerminalSurface::inputMethodEvent(QInputMethodEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const QString commit_text             = terminal_commit_text_from_event(*event);
    const QString preedit_text            = event->preeditString();
    const int     preedit_cursor_position = preedit_cursor_position_from_event(*event);
    const std::uint64_t trace_id = term::interaction_trace_enabled()
        ? term::next_interaction_trace_correlation_id()
        : 0U;
    if (term::interaction_trace_enabled()) {
        term::record_interaction_trace(
            "ime",
            "input-method-event",
            QStringLiteral(
                "commit_units=%1 preedit_units=%2 preedit_active_before=%3 cursor=%4 "
                "attributes=%5 replacement_start=%6 replacement_length=%7 accepted_before=%8")
                .arg(commit_text.size())
                .arg(preedit_text.size())
                .arg(m_private->ime_preedit.active)
                .arg(preedit_cursor_position)
                .arg(event->attributes().size())
                .arg(event->replacementStart())
                .arg(event->replacementLength())
                .arg(event->isAccepted()),
            trace_id);
    }

    std::optional<term::Terminal_ime_commit_result> commit_result;
    if (m_private->session != nullptr) {
        if (!commit_text.isEmpty()) {
            m_private->resolve_pending_published_mouse_reports_before_terminal_input(*this);
            if (m_private->session == nullptr) {
                event->ignore();
                term::record_interaction_trace(
                    "ime", "input-method-result", QStringLiteral("route=session-lost accepted=false"), trace_id);
                return;
            }
            commit_result =
                m_private->session->write_ime_commit(commit_text, trace_id);
        }

        const bool commit_blocks_empty_preedit_cancel =
            commit_result.has_value()                &&
            commit_result->handled                   &&
            !is_accepted(commit_result->result.code);
        if (preedit_text.isEmpty()) {
            if (!commit_blocks_empty_preedit_cancel) {
                m_private->session->cancel_ime_preedit();
            }
        }
        else {
            m_private->session->set_ime_preedit(
                preedit_text,
                preedit_cursor_position);
        }

        sync_from_session();

        if (commit_result.has_value() &&
            commit_result->handled &&
            !is_accepted(commit_result->result.code))
        {
            report_result_failure(commit_result->result);
            if (term::interaction_trace_enabled()) {
                term::record_interaction_trace(
                    "ime",
                    "input-method-result",
                    QStringLiteral(
                        "handled=true sequence=%1 result=%2 preedit_active_after=%3 accepted=%4")
                        .arg(commit_result->result.sequence)
                        .arg(static_cast<int>(commit_result->result.code))
                        .arg(m_private->ime_preedit.active)
                        .arg(event->isAccepted()),
                    trace_id);
            }
            return;
        }
    }
    else {
        term::Ime_preedit_state state;
        if (!preedit_text.isEmpty()) {
            state.text            = preedit_text;
            state.cursor_position = preedit_cursor_position;
            state.active          = true;
        }
        m_private->set_ime_preedit_state(*this, std::move(state));
    }

    event->accept();
    if (term::interaction_trace_enabled()) {
        QString result_details = QStringLiteral("handled=%1 preedit_active_after=%2 accepted=%3")
            .arg(commit_result.has_value() && commit_result->handled)
            .arg(m_private->ime_preedit.active)
            .arg(event->isAccepted());
        if (commit_result.has_value()) {
            result_details += QStringLiteral(" sequence=%1 result=%2")
                .arg(commit_result->result.sequence)
                .arg(static_cast<int>(commit_result->result.code));
        }
        term::record_interaction_trace(
            "ime", "input-method-result", result_details, trace_id);
    }
}

void VNM_TerminalSurface::refresh_grid_metrics()
{
    m_private->render_font               = term::vnm_terminal_font(m_font_family, m_font_size);
    m_private->render_device_pixel_ratio = current_device_pixel_ratio(window());
    const QString atlas_font_epoch_key = QStringLiteral("%1|%2")
        .arg(m_private->render_font.toString())
        .arg(m_private->render_device_pixel_ratio, 0, 'g', 17);
    if (m_private->qsg_atlas_font_epoch_key != atlas_font_epoch_key) {
        m_private->qsg_atlas_font_epoch_key = atlas_font_epoch_key;
        ++m_private->qsg_atlas_font_epoch;
        if (m_private->qsg_atlas_font_epoch == 0U) {
            m_private->qsg_atlas_font_epoch = 1U;
        }
    }
    m_private->grid_metrics_provider.set_font(m_private->render_font);
    m_private->grid_metrics_provider.set_device_pixel_ratio(
        m_private->render_device_pixel_ratio);

    if (!std::isfinite(m_font_size) || m_font_size <= 0.0) {
        m_private->cell_metrics = {};
        refresh_hyperlink_hover_feedback();
        if (m_private->session != nullptr) {
            refresh_active_session_geometry();
        }
        else {
            set_grid_size(0, 0);
        }
        m_private->request_render_update(*this);
        return;
    }

    m_private->warm_prompt_text_layouts_for_render_font();
    m_private->cell_metrics = m_private->grid_metrics_provider.cell_metrics();
    refresh_hyperlink_hover_feedback();
    const term::Terminal_metrics_result grid_result =
        m_private->grid_metrics_provider.grid_size_for_item_geometry(boundingRect().size());
    if (m_private->session != nullptr) {
        refresh_active_session_geometry();
    }
    else {
        if (grid_result.status == term::Terminal_metrics_status::OK) {
            set_grid_size(grid_result.grid_size.rows, grid_result.grid_size.columns);
        }
        else {
            set_grid_size(0, 0);
        }
    }
    m_private->request_render_update(*this);
    updateInputMethod(Qt::ImCursorRectangle);
}

void VNM_TerminalSurface::refresh_grid_metrics_if_device_pixel_ratio_changed()
{
    const qreal device_pixel_ratio = current_device_pixel_ratio(window());
    if (same_property_value(device_pixel_ratio, m_private->render_device_pixel_ratio)) {
        return;
    }

    refresh_grid_metrics();
}

void VNM_TerminalSurface::refresh_grid_from_item_geometry()
{
    Q_ASSERT(thread() == QThread::currentThread());

    refresh_grid_metrics();
}

void VNM_TerminalSurface::set_grid_size(int rows, int columns)
{
    if (m_rows == rows && m_columns == columns) {
        return;
    }

    m_rows    = rows;
    m_columns = columns;
    emit grid_geometry_changed();
}

void VNM_TerminalSurface::set_viewport_state(
    const term::Terminal_viewport_state& state)
{
    const int scrollback_rows = std::max(0, state.scrollback_rows);
    const int visible_rows    = std::max(0, state.visible_rows);
    const int offset_from_tail =
        std::clamp(state.offset_from_tail, 0, scrollback_rows);
    const bool at_tail = offset_from_tail == 0;

    if (m_scrollback_rows           == scrollback_rows  &&
        m_viewport_visible_rows     == visible_rows     &&
        m_viewport_offset_from_tail == offset_from_tail &&
        m_viewport_at_tail          == at_tail)
    {
        return;
    }

    // A viewport scroll moves different rows under the resting pointer, so a
    // shown tooltip no longer describes the hovered row. Snapshot publication
    // also lands here when only the scrollback row count grew (streaming
    // output at the tail); that is not user activity and must not dismiss.
    const bool scroll_position_changed =
        m_viewport_visible_rows     != visible_rows ||
        m_viewport_offset_from_tail != offset_from_tail;

    m_scrollback_rows           = scrollback_rows;
    m_viewport_visible_rows     = visible_rows;
    m_viewport_offset_from_tail = offset_from_tail;
    m_viewport_at_tail          = at_tail;
    if (scroll_position_changed) {
        dismiss_row_timestamp_tooltip();
    }
    if (!m_private->shutting_down.load()) {
        emit viewport_changed();
    }
}

void VNM_TerminalSurface::set_process_state(Process_state state)
{
    if (m_process_state == state) {
        return;
    }

    m_process_state = state;
    emit process_state_changed();
}

void VNM_TerminalSurface::set_backend_ready(bool ready)
{
    if (m_backend_ready == ready) {
        return;
    }

    m_backend_ready = ready;
    emit backend_ready_changed();
}

void VNM_TerminalSurface::set_backend_geometry_in_sync(bool in_sync)
{
    if (m_backend_geometry_in_sync == in_sync) {
        return;
    }

    m_backend_geometry_in_sync = in_sync;
    emit geometry_sync_changed();
}

void VNM_TerminalSurface::set_selection_state(Selection_state state)
{
    if (m_selection_state == state) {
        return;
    }

    m_selection_state = state;
    if (!m_private->shutting_down.load()) {
        emit selection_changed();
    }
}

void VNM_TerminalSurface::set_search_state(
    QString             query,
    Search_result_state state,
    int                 match_count,
    int                 current_match)
{
    if (m_search_query         == query         &&
        m_search_result_state  == state         &&
        m_search_match_count   == match_count   &&
        m_current_search_match == current_match)
    {
        return;
    }

    m_search_query         = std::move(query);
    m_search_result_state  = state;
    m_search_match_count   = match_count;
    m_current_search_match = current_match;
    if (!m_private->shutting_down.load()) {
        emit search_changed();
    }
}

void VNM_TerminalSurface::bind_window_signals(QQuickWindow* window)
{
    QObject::disconnect(m_private->window_screen_changed_connection);
    QObject::disconnect(m_private->window_scene_graph_invalidated_connection);
    m_private->bound_window = window;
    ++m_private->window_binding_generation;
    const std::uint64_t window_binding_generation =
        m_private->window_binding_generation;
    bind_screen_signals(window != nullptr ? window->screen() : nullptr);

    if (window == nullptr) {
        return;
    }

    m_private->window_screen_changed_connection = QObject::connect(
        window,
        &QWindow::screenChanged,
        this,
        [this](QScreen* screen) {
            bind_screen_signals(screen);
            refresh_grid_metrics();
            m_private->request_render_update(*this);
        });
    m_private->window_scene_graph_invalidated_connection = QObject::connect(
        window,
        &QQuickWindow::sceneGraphInvalidated,
        this,
        [this, window_binding_generation]() {
            handle_scene_graph_invalidated(window_binding_generation);
        },
        Qt::QueuedConnection);
}

void VNM_TerminalSurface::handle_scene_graph_invalidated(
    std::uint64_t window_binding_generation)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_scene_graph_invalidated();
    }
    if (window_binding_generation == m_private->window_binding_generation) {
        m_private->reset_render_update_schedule();
        m_private->cancel_backend_callback_frame_update_or_queue_posted_drain(
            *this);
    }
}

void VNM_TerminalSurface::bind_screen_signals(QScreen* screen)
{
    QObject::disconnect(m_private->screen_dpi_changed_connection);
    QObject::disconnect(m_private->screen_physical_dpi_changed_connection);

    if (screen == nullptr) {
        return;
    }

    m_private->screen_dpi_changed_connection = QObject::connect(
        screen,
        &QScreen::logicalDotsPerInchChanged,
        this,
        [this](qreal) {
            refresh_grid_metrics();
        });
    m_private->screen_physical_dpi_changed_connection = QObject::connect(
        screen,
        &QScreen::physicalDotsPerInchChanged,
        this,
        [this](qreal) {
            refresh_grid_metrics();
        });
}

bool VNM_TerminalSurface::start_process_with_backend(
    std::unique_ptr<term::Terminal_backend>    backend,
    term::Terminal_launch_config               launch_config)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session != nullptr) {
        drain_backend_callback_events();
        if (is_live_process_state(m_private->session->process_state())) {
            report_backend_error({
                term::Terminal_backend_error_code::START_FAILED,
                QStringLiteral("start requires no active terminal process"),
            });
            return false;
        }

        reset_session();
    }

    if (backend == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::START_FAILED,
            QStringLiteral("terminal backend is not available"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    if (!m_terminal_title.isEmpty()) {
        m_terminal_title.clear();
        emit terminal_title_changed();
    }
    if (!m_terminal_icon_name.isEmpty()) {
        m_terminal_icon_name.clear();
        emit terminal_icon_name_changed();
    }
    set_process_state(Process_state::NOT_STARTED);
    set_backend_ready(false);
    set_backend_geometry_in_sync(false);
    m_private->clear_hyperlink_activation_state();
    m_private->render_snapshot.reset();
    refresh_hyperlink_hover_feedback();
    m_private->set_ime_preedit_state(*this, {});
    m_private->last_installed_render_publication_generation = 0U;
    m_private->last_ime_preedit_generation        = 0U;
    m_private->last_backend_error_signal_sequence = 0U;
    m_private->request_render_update(*this);

    m_private->transcript_recorder.reset();
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (!m_transcript_capture_path.isEmpty()) {
        QString transcript_error;
        m_private->transcript_recorder = term::Terminal_transcript_recorder::create(
            m_transcript_capture_path,
            m_transcript_snapshot_diagnostics,
            m_transcript_timing_diagnostics,
            &transcript_error);
        if (m_private->transcript_recorder == nullptr) {
            report_backend_error({
                term::Terminal_backend_error_code::START_FAILED,
                transcript_error,
            });
            set_process_state(Process_state::FAILED);
            return false;
        }
    }
#else
    if (!m_transcript_capture_path.isEmpty()) {
        report_backend_error({
            term::Terminal_backend_error_code::START_FAILED,
            QStringLiteral("transcript capture/replay is disabled in this build"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }
#endif

    term::Terminal_session_config session_config;
    session_config.output_queue_limits.high_water_bytes =
        k_surface_output_queue_high_water_bytes;
    session_config.output_queue_limits.hard_limit_bytes =
        k_surface_output_queue_hard_limit_bytes;
    session_config.trace_notification_limit              = k_surface_notification_trace_limit;
    session_config.scrollback_limit                      = m_scrollback_limit;
    session_config.retained_history_capacity_bytes       = m_retained_history_capacity_bytes;
    session_config.backend_output_capture_config         = m_backend_output_capture_config;
    session_config.capture_dirty_row_stats               = m_private->dirty_row_stats_enabled;
    session_config.selection_trace_enabled               = m_selection_trace_enabled;
    session_config.transcript_recorder                   = m_private->transcript_recorder;
    session_config.selection_viewport_projection_enabled = true;
    session_config.synchronized_output_scroll_policy =
        terminal_synchronized_output_scroll_policy(m_synchronized_output_scroll_policy);
    session_config.text_area_resize_policy =
        terminal_text_area_resize_policy(m_text_area_resize_policy);
    if (m_text_area_resize_arbitration_enabled) {
        session_config.text_area_resize_arbitration.emplace();
    }
    session_config.recover_scrollback_from_primary_repaints =
        m_primary_repaint_recovery_enabled;
    session_config.bell_policy =
        terminal_bell_policy_for_surface(m_audible_bell_policy, m_visual_bell_policy);
    session_config.backend_event_epoch_notifier = [this](
        std::uint64_t,
        bool          callback_output_pressure) {
        if (callback_output_pressure) {
            queue_backend_callback_pressure_drain();
        }
        else {
            m_private->schedule_backend_callback_frame_or_posted_drain(*this);
        }
    };

    m_private->session =
        std::make_unique<term::Terminal_session>(std::move(backend), session_config);
    ++m_private->session_generation;
    m_private->session->set_color_state(
        term::make_terminal_color_state(resolve_surface_color_scheme(*this)));
    m_private->resize_controller = std::make_unique<term::Terminal_resize_controller>(
        *m_private->session,
        m_private->grid_metrics_provider);

    const term::Terminal_session_result result =
        m_private->resize_controller->start_from_geometry(
            std::move(launch_config),
            boundingRect().size());
    if (is_accepted(result.code) && !m_search_query.isEmpty()) {
        m_private->session->set_search_query(m_search_query);
    }
    sync_from_session();
    if (!is_accepted(result.code)) {
        if (m_process_state == Process_state::NOT_STARTED) {
            set_process_state(Process_state::FAILED);
        }
        report_result_failure(result);
        reset_session();
        return false;
    }

    return true;
}

void VNM_TerminalSurface::queue_backend_callback_pressure_drain()
{
    if (m_private->shutting_down.load()) {
        return;
    }

    // Callback ingress may run on a backend thread. Record pressure with an
    // atomic only; the coalesced GUI wake owns all Qt frame-state cancellation.
    m_private->backend_callback_pressure_drain_requested.store(true);
    queue_backend_callback_drain();
}

void VNM_TerminalSurface::queue_backend_callback_drain()
{
    if (m_private->shutting_down.load()) {
        return;
    }

    // One queued GUI wakeup is enough: Terminal_session owns the pending
    // callback commands until the lambda reaches the budgeted drain.
    bool expected = false;
    if (!m_private->session_drain_queued.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool queued = vnm::qt::post(
        this,
        [this] {
            m_private->session_drain_queued.store(false);
            const bool pressure_owner =
                m_private->backend_callback_pressure_drain_requested.exchange(false);
            if (!m_private->shutting_down.load()) {
                if (pressure_owner) {
                    (void)m_private->cancel_backend_callback_frame_update_wait();
                }
                drain_backend_callback_events_for_posted_work();
            }
        }) == vnm::qt::Post_result::QUEUED;
    if (!queued) {
        // If the wakeup could not be posted, clear the latch so a later
        // backend callback can retry instead of losing the drain request.
        m_private->session_drain_queued.store(false);
    }
}

void VNM_TerminalSurface::drain_backend_callback_events()
{
    drain_backend_callback_events_with_budget(std::nullopt);
}

void VNM_TerminalSurface::drain_backend_callback_events_for_posted_work()
{
    auto& drain_stats = m_private->backend_drain_stats;
    ++drain_stats.posted_drain_calls;
    if (m_private->frame_work_pending(*this)) {
        ++drain_stats.posted_frame_pending_small_budget_calls;
        drain_backend_callback_events_for(k_backend_callback_frame_pending_posted_drain_budget);
        return;
    }

    ++drain_stats.posted_full_budget_calls;
    drain_backend_callback_events_for(k_backend_callback_posted_drain_budget);
}

void VNM_TerminalSurface::drain_backend_callback_events(bool budgeted)
{
    if (budgeted) {
        drain_backend_callback_events_for(k_backend_callback_drain_budget);
    }
    else {
        drain_backend_callback_events_with_budget(std::nullopt);
    }
}

void VNM_TerminalSurface::drain_backend_callback_events_for(
    std::chrono::steady_clock::duration budget)
{
    drain_backend_callback_events_with_budget(
        std::optional<std::chrono::steady_clock::duration>{budget});
}

VNM_TerminalSurface::backend_callback_drain_result_t
VNM_TerminalSurface::drain_backend_callback_events_until_epoch(
    std::uint64_t                                     target_epoch,
    std::optional<std::chrono::steady_clock::duration> budget)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const Backend_callback_incomplete_follow_up incomplete_follow_up =
        budget.has_value()
            ? Backend_callback_incomplete_follow_up::FRAME_UPDATE
            : Backend_callback_incomplete_follow_up::POSTED_DRAIN;

    if (m_private->session == nullptr) {
        return process_backend_callback_events_recorded(
            nullptr,
            budget,
            true,
            target_epoch,
            incomplete_follow_up);
    }

    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    const backend_callback_drain_result_t drain_result =
        process_backend_callback_events_recorded(
            session,
            budget,
            true,
            target_epoch,
            incomplete_follow_up);
    if (budget.has_value()) {
        request_backend_callback_follow_up_after_incomplete_recorded_drain(
            session,
            session_generation,
            drain_result.stop,
            incomplete_follow_up);
    }
    return drain_result;
}

VNM_TerminalSurface::backend_callback_drain_result_t
VNM_TerminalSurface::process_backend_callback_events_recorded(
    term::Terminal_session*                                  session,
    std::optional<std::chrono::steady_clock::duration>       budget,
    bool                                                     use_budget_notification_boundary,
    std::optional<std::uint64_t>                             target_backend_callback_epoch,
    Backend_callback_incomplete_follow_up                    pending_mouse_report_follow_up)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const auto drain_started = std::chrono::steady_clock::now();
    m_private->reconcile_atlas_completion(*this);
    const bool render_update_pending_before_drain = m_private->render_update_pending;
    const bool atlas_completion_pending_before_drain = m_private->atlas_completion_pending;
    const bool frame_work_pending_before_drain =
        render_update_pending_before_drain || atlas_completion_pending_before_drain;

    auto& drain_stats = m_private->backend_drain_stats;
    ++drain_stats.total_drain_calls;
    if (budget.has_value()) {
        ++drain_stats.budgeted_drain_calls;
    }
    else {
        ++drain_stats.unbudgeted_drain_calls;
    }
    if (frame_work_pending_before_drain) {
        ++drain_stats.frame_work_pending_drain_calls;
        if (render_update_pending_before_drain) {
            ++drain_stats.render_update_pending_drain_calls;
        }
        if (atlas_completion_pending_before_drain) {
            ++drain_stats.atlas_completion_pending_drain_calls;
        }
    }

    const auto record_total_elapsed = [&] {
        const std::uint64_t elapsed_count =
            elapsed_ns_count(std::chrono::steady_clock::now() - drain_started);
        drain_stats.total_elapsed_ns += elapsed_count;
        drain_stats.max_elapsed_ns =
            std::max(drain_stats.max_elapsed_ns, elapsed_count);
        if (frame_work_pending_before_drain) {
            drain_stats.frame_work_pending_elapsed_ns += elapsed_count;
        }
    };
    const auto record_stage_elapsed = [](
        std::uint64_t&                                      calls,
        std::uint64_t&                                      elapsed_total_ns,
        std::uint64_t&                                      max_elapsed_ns,
        std::chrono::steady_clock::time_point               started) {
        const std::uint64_t elapsed_count =
            elapsed_ns_count(std::chrono::steady_clock::now() - started);
        ++calls;
        elapsed_total_ns += elapsed_count;
        max_elapsed_ns = std::max(max_elapsed_ns, elapsed_count);
    };

    backend_callback_drain_result_t result;
    if (session == nullptr) {
        record_total_elapsed();
        return result;
    }

    const std::uint64_t session_generation = m_private->session_generation;
    const std::uint64_t callback_processed_epoch_before =
        session->backend_callback_processed_epoch();
    const auto session_processing_started = std::chrono::steady_clock::now();
    if (target_backend_callback_epoch.has_value()) {
        result.stop =
            session->process_backend_callback_events_until_epoch(
                *target_backend_callback_epoch,
                budget);
    }
    else
    if (budget.has_value()) {
        result.stop = session->process_backend_callback_events_for(*budget);
    }
    else {
        session->process_backend_callback_events();
    }
    record_stage_elapsed(
        drain_stats.session_processing_calls,
        drain_stats.session_processing_elapsed_ns,
        drain_stats.session_processing_max_elapsed_ns,
        session_processing_started);

    result.deliver_notifications =
        !use_budget_notification_boundary ||
        !budget.has_value() ||
        backend_drain_reached_notification_boundary(result.stop, *session);
    const auto sync_started = std::chrono::steady_clock::now();
    sync_from_session(result.deliver_notifications);
    record_stage_elapsed(
        drain_stats.sync_from_session_calls,
        drain_stats.sync_from_session_elapsed_ns,
        drain_stats.sync_from_session_max_elapsed_ns,
        sync_started);

    const bool session_still_active =
        m_private->active_session_matches(session, session_generation);
    result.callbacks_pending_after_drain =
        session_still_active && session->has_pending_backend_callback_events();
    if (session_still_active) {
        if (!result.callbacks_pending_after_drain) {
            const bool releases_after_frame_owner =
                m_private->backend_callback_after_frame_wait_active;
            if (releases_after_frame_owner &&
                m_private->
                    before_backend_callback_frame_owner_release_handler_for_testing)
            {
                std::function<void()> handler = std::move(
                    m_private->
                        before_backend_callback_frame_owner_release_handler_for_testing);
                handler();
            }
            m_private->stop_backend_callback_frame_progress_watchdog();
            if (releases_after_frame_owner) {
                // Clear the active after-frame owner, then close its lost-wake
                // window. An enqueue that coalesced before the clear needs a new
                // dispatch; an enqueue after the clear schedules itself.
                result.callbacks_pending_after_drain =
                    session->has_pending_backend_callback_events();
                if (result.callbacks_pending_after_drain) {
                    m_private->schedule_backend_callback_frame_or_posted_drain(
                        *this);
                }
            }
        }
        else
        if (m_private->backend_callback_frame_progress_deadline.has_value() &&
            session->backend_callback_processed_epoch() >
                callback_processed_epoch_before)
        {
            m_private->restart_backend_callback_frame_progress_watchdog();
        }
    }
    if (budget.has_value()) {
        if (result.stop ==
            term::Backend_callback_drain_stop::SYNCHRONIZED_OUTPUT_RELEASED)
        {
            ++drain_stats.synchronized_output_release_incomplete;
        }
        else if (result.stop == term::Backend_callback_drain_stop::UNSETTLED) {
            ++drain_stats.budget_exhausted_incomplete;
        }
    }
    if (result.callbacks_pending_after_drain) {
        ++drain_stats.pending_callback_after_drain;
    }
    if (session_still_active && session->output_backpressure_active()) {
        ++drain_stats.output_backpressure_after_drain;
    }
    record_total_elapsed();
    if (session_still_active) {
        (void)m_private->retry_pending_published_mouse_reports(
            *this,
            false,
            pending_mouse_report_follow_up);
    }
    return result;
}

void VNM_TerminalSurface::
request_backend_callback_follow_up_after_incomplete_recorded_drain(
    term::Terminal_session*                         session,
    std::uint64_t                                   session_generation,
    term::Backend_callback_drain_stop               stop,
    Backend_callback_incomplete_follow_up           follow_up)
{
    if (stop != term::Backend_callback_drain_stop::COMPLETE &&
        session != nullptr &&
        m_private->active_session_matches(session, session_generation) &&
        session->has_pending_backend_callback_events())
    {
        if (follow_up == Backend_callback_incomplete_follow_up::FRAME_UPDATE) {
            m_private->request_backend_callback_frame_update_or_queue_posted_drain(
                *this);
        }
        else {
            ++m_private->backend_drain_stats.requeue_count;
            queue_backend_callback_drain();
        }
    }
}

void VNM_TerminalSurface::drain_backend_callback_events_with_budget(
    std::optional<std::chrono::steady_clock::duration> budget)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled || term::interaction_trace_enabled()) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface backend-drain reason=no-session"));
        }
        (void)process_backend_callback_events_recorded(nullptr, budget, true);
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    const bool trace_drain =
        (m_selection_trace_enabled || term::interaction_trace_enabled()) &&
        (m_private->selection_drag_active || session->has_selection());
    if (trace_drain) {
        const std::optional<term::terminal_selection_source_identity_t> before_source =
            session->published_selection_source_identity();
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface backend-drain begin %1 source=%2")
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot))
                .arg(selection_trace_source_identity(before_source)));
    }
    // Time-sliced drains may publish intermediate render snapshots, but public
    // notifications stay queued until the slice reaches a boundary so the
    // session's coalescing contract is not split by pump cadence.
    const backend_callback_drain_result_t drain_result =
        process_backend_callback_events_recorded(session, budget, true);
    if (trace_drain) {
        const std::optional<term::terminal_selection_source_identity_t> after_source =
            m_private->session != nullptr
                ? m_private->session->published_selection_source_identity()
                : std::optional<term::terminal_selection_source_identity_t>{};
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface backend-drain end %1 source=%2")
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot))
                .arg(selection_trace_source_identity(after_source)));
    }
    if (budget.has_value()) {
        request_backend_callback_follow_up_after_incomplete_recorded_drain(
            session,
            session_generation,
            drain_result.stop,
            Backend_callback_incomplete_follow_up::POSTED_DRAIN);
    }
}

void VNM_TerminalSurface::refresh_active_session_geometry()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr || m_private->resize_controller == nullptr) {
        return;
    }

    if (!is_live_process_state(m_private->session->process_state())) {
        sync_from_session();
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    (void)process_backend_callback_events_recorded(session, std::nullopt, false);
    if (!m_private->active_session_matches(session, session_generation) ||
        !is_live_process_state(session->process_state()))
    {
        return;
    }

    const term::Terminal_session_result result =
        m_private->resize_controller->refresh_from_geometry(boundingRect().size());
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
    }
}

void VNM_TerminalSurface::sync_from_session(bool deliver_notifications)
{
    Q_ASSERT(thread() == QThread::currentThread());
    VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session");

    if (m_private->session == nullptr) {
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    const auto active_session_still_matches = [&] {
        return m_private->active_session_matches(session, session_generation);
    };

    bool render_snapshot_installed = false;
    std::shared_ptr<const term::Terminal_render_snapshot> installed_render_snapshot;
    const auto installed_render_snapshot_still_current = [&] {
        return m_private->render_snapshot == installed_render_snapshot;
    };

    {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::render_snapshot");

        const std::uint64_t snapshot_generation =
            session->render_snapshot_generation();
        if (snapshot_generation !=
            m_private->last_installed_render_publication_generation)
        {
            installed_render_snapshot = session->latest_render_snapshot_handle();
            term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
                *this,
                installed_render_snapshot);
            if (!active_session_still_matches()) {
                return;
            }
            session->mark_render_snapshot_installed(snapshot_generation);
            m_private->last_installed_render_publication_generation =
                snapshot_generation;
            render_snapshot_installed = true;
        }
    }

    if (!active_session_still_matches()) {
        return;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::session_state");

        if (!active_session_still_matches()) {
            return;
        }
        set_process_state(surface_process_state(session->process_state()));
        if (!active_session_still_matches()) {
            return;
        }
        set_backend_ready(session->backend_ready());
        if (!active_session_still_matches()) {
            return;
        }
        set_backend_geometry_in_sync(session->backend_geometry_in_sync());
        if (!active_session_still_matches()) {
            return;
        }
        const Selection_state selection_state =
            session->has_selection()
                ? Selection_state::ACTIVE
                : Selection_state::NONE;
        set_selection_state(selection_state);
        if (!active_session_still_matches()) {
            return;
        }
        const term::terminal_search_result_state_t search_state =
            session->search_result_state();
        set_search_state(
            session->search_query(),
            surface_search_result_state(search_state.status),
            search_state.match_count,
            search_state.current_match);
        if (!active_session_still_matches()) {
            return;
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::grid_and_modes");

        if (!active_session_still_matches()) {
            return;
        }
        const term::terminal_grid_size_t grid_size = session->grid_size();
        set_grid_size(grid_size.rows, grid_size.columns);
        if (!active_session_still_matches()) {
            return;
        }

        const bool live_mouse_reporting_active = session->mouse_reporting_active();
        if (live_mouse_reporting_active != m_private->last_sgr_mouse_reporting_active) {
            m_private->clear_mouse_wheel_remainders();
        }
        m_private->last_sgr_mouse_reporting_active = live_mouse_reporting_active;

        if (!active_session_still_matches()) {
            return;
        }
        const std::uint64_t alternate_scroll_mode_generation =
            session->alternate_scroll_mode_generation();
        if (alternate_scroll_mode_generation !=
            m_private->last_alternate_scroll_mode_generation)
        {
            m_private->clear_mouse_wheel_remainders();
        }
        m_private->last_alternate_scroll_mode_generation = alternate_scroll_mode_generation;

        if (!active_session_still_matches()) {
            return;
        }
        const bool live_alternate_scroll_active = session->alternate_scroll_active();
        if (live_alternate_scroll_active != m_private->last_alternate_scroll_active) {
            m_private->clear_mouse_wheel_remainders();
        }
        m_private->last_alternate_scroll_active = live_alternate_scroll_active;
    }

    if (render_snapshot_installed) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "VNM_TerminalSurface::sync_from_session::render_snapshot_bookkeeping");

        if (!active_session_still_matches()) {
            return;
        }
        if (installed_render_snapshot_still_current() &&
            installed_render_snapshot != nullptr)
        {
            set_viewport_state(installed_render_snapshot->viewport);
            if (!active_session_still_matches()) {
                return;
            }
        }

        if (!active_session_still_matches()) {
            return;
        }
        if (installed_render_snapshot_still_current()) {
            const bool live_mouse_reporting_active = session->mouse_reporting_active();
            const bool sgr_mouse_reporting_active =
                snapshot_has_sgr_mouse_reporting(installed_render_snapshot);
            const bool mouse_reporting_mode_changed =
                installed_render_snapshot != nullptr &&
                installed_render_snapshot->metadata.mouse_reporting_mode_changed;
            if (mouse_reporting_mode_changed ||
                sgr_mouse_reporting_active != live_mouse_reporting_active)
            {
                m_private->clear_mouse_wheel_remainders();
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::ime_preedit");

        const std::uint64_t ime_preedit_generation =
            session->ime_preedit_generation();
        if (ime_preedit_generation != m_private->last_ime_preedit_generation) {
            m_private->set_ime_preedit_state(*this, session->ime_preedit_state());
            if (!active_session_still_matches()) {
                return;
            }
            m_private->last_ime_preedit_generation = ime_preedit_generation;
        }
    }

    if (deliver_notifications && !m_private->delivering_session_notifications) {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::notifications");

        // Answering from inside a handler re-enters here, and the answer can
        // have queued the next notification before it returns: settling one
        // arbitrated text-area resize releases its held tail, which arms the
        // request the tail carries. Delivering that newer batch from the nested
        // call would put it ahead of the notifications this loop still holds and
        // would cost one stack frame per queued request, which a burst of held
        // CSI 8 t sequences answered inline turned into a stack overflow after a
        // few hundred of them. The latch makes the nested delivery a no-op and
        // this loop drains what it queued, in order and without recursing.
        Session_notification_delivery_scope delivering(
            m_private->delivering_session_notifications);
        for (;;) {
            const std::vector<term::Terminal_session_notification> notifications =
                session->take_pending_notifications();
            if (notifications.empty()) {
                break;
            }

            for (const term::Terminal_session_notification& notification : notifications) {
                replay_session_notification(notification);
                if (!active_session_still_matches()) {
                    return;
                }
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session::recovery_timer");

        sync_synchronized_output_recovery_timer();
    }
}

void VNM_TerminalSurface::sync_synchronized_output_recovery_timer()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr ||
        !m_private->session->render_publication_blocked())
    {
        m_private->synchronized_output_recovery_timer.stop();
        return;
    }

    if (!m_private->synchronized_output_recovery_timer.isActive() ||
        m_private->synchronized_output_recovery_timer.interval() !=
            m_synchronized_output_stale_timeout_ms)
    {
        m_private->synchronized_output_recovery_timer.start(
            m_synchronized_output_stale_timeout_ms);
    }
}

void VNM_TerminalSurface::handle_synchronized_output_recovery_timeout()
{
    handle_synchronized_output_recovery_timeout(k_backend_callback_drain_budget);
}

void VNM_TerminalSurface::handle_synchronized_output_recovery_timeout(
    std::chrono::steady_clock::duration budget)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    const backend_callback_drain_result_t drain_result =
        process_backend_callback_events_recorded(session, budget, true);
    if (!m_private->active_session_matches(session, session_generation)) {
        return;
    }

    const auto queue_remaining_callbacks = [&] {
        request_backend_callback_follow_up_after_incomplete_recorded_drain(
            session,
            session_generation,
            drain_result.stop,
            Backend_callback_incomplete_follow_up::POSTED_DRAIN);
    };

    if (!session->render_publication_blocked()) {
        queue_remaining_callbacks();
        return;
    }

    const term::Terminal_session_result result =
        session->force_release_synchronized_output_without_backend_drain();
    sync_from_session(drain_result.deliver_notifications);
    if (!m_private->active_session_matches(session, session_generation)) {
        return;
    }

    queue_remaining_callbacks();

    if (!is_accepted(result.code)) {
        report_result_failure(result);
    }
}

void VNM_TerminalSurface::replay_session_notification(
    const term::Terminal_session_notification& notification)
{
    switch (notification.kind) {
        case term::Terminal_session_notification_kind::PROCESS_STARTED:
            emit process_started();
            break;
        case term::Terminal_session_notification_kind::PROCESS_EXITED:
            m_private->clear_pending_published_mouse_reports();
            m_private->clear_mouse_reporting_state();
            m_private->pending_clipboard_write.reset();
            if (notification.exit.has_value()) {
                emit process_exited(
                    surface_exit_reason(notification.exit->reason),
                    notification.exit->exit_code);
            }
            break;
        case term::Terminal_session_notification_kind::SNAPSHOT_READY:
            break;
        case term::Terminal_session_notification_kind::OUTPUT_ACTIVITY:
            emit output_activity();
            break;
        case term::Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED:
            emit output_backpressure_changed(notification.backpressure_active);
            break;
        case term::Terminal_session_notification_kind::BACKEND_ERROR:
            if (notification.backend_error.has_value()) {
                report_backend_error(*notification.backend_error, notification.sequence);
            }
            break;
        case term::Terminal_session_notification_kind::BELL_REQUESTED:
            if (notification.bell_audible) {
                m_private->play_audible_bell();
            }
            if (notification.bell_audible || notification.bell_visual) {
                emit bell_requested();
            }
            break;
        case term::Terminal_session_notification_kind::TITLE_CHANGED:
            if (m_terminal_title != notification.message) {
                m_terminal_title = notification.message;
                emit terminal_title_changed();
            }
            break;
        case term::Terminal_session_notification_kind::ICON_NAME_CHANGED:
            if (m_terminal_icon_name != notification.message) {
                m_terminal_icon_name = notification.message;
                emit terminal_icon_name_changed();
            }
            break;
        case term::Terminal_session_notification_kind::RESIZE_TRANSACTION:
            break;
        case term::Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
            if (notification.text_area_resize_request.has_value()) {
                emit text_area_resize_requested(
                    notification.text_area_resize_request->rows,
                    notification.text_area_resize_request->columns);
            }
            break;
        case term::Terminal_session_notification_kind::TEXT_AREA_RESIZE_ARBITRATION_REQUESTED:
            if (notification.text_area_resize_arbitration_request.has_value()) {
                const term::terminal_text_area_resize_arbitration_request_t& request =
                    *notification.text_area_resize_arbitration_request;
                // The session id passes through unchanged: there is exactly one
                // request in flight and it has to travel back to the session.
                m_private->pending_text_area_resize_arbitration = request.request_id;
                if (m_text_area_resize_arbitration_timeout_ms > 0) {
                    m_private->text_area_resize_arbitration_timer.start(
                        m_text_area_resize_arbitration_timeout_ms);
                }
                emit text_area_resize_arbitration_requested(
                    request.request_id,
                    request.requested_grid_size.rows,
                    request.requested_grid_size.columns);
            }
            break;
        case term::Terminal_session_notification_kind::TEXT_AREA_RESIZE_ARBITRATION_SETTLED:
            if (notification.text_area_resize_arbitration_settlement.has_value()) {
                const term::terminal_text_area_resize_arbitration_settlement_t& settlement =
                    *notification.text_area_resize_arbitration_settlement;
                m_private->text_area_resize_arbitration_timer.stop();
                m_private->pending_text_area_resize_arbitration.reset();
                emit text_area_resize_arbitration_settled(
                    settlement.request_id,
                    surface_text_area_resize_arbitration_outcome(settlement.outcome),
                    settlement.effective_grid_size.rows,
                    settlement.effective_grid_size.columns);
            }
            break;
        case term::Terminal_session_notification_kind::HOST_REQUEST:
            if (notification.clipboard_write_request.has_value()) {
                m_private->pending_clipboard_write = *notification.clipboard_write_request;
                m_private->pending_clipboard_write->request_id =
                    m_private->next_clipboard_write_request_id++;
                if (m_private->next_clipboard_write_request_id == 0U) {
                    m_private->next_clipboard_write_request_id = 1U;
                }
                emit clipboard_write_requested(
                    m_private->pending_clipboard_write->request_id,
                    m_private->pending_clipboard_write->target_selection,
                    m_private->pending_clipboard_write->decoded_payload);
            }
            break;
    }
}

void VNM_TerminalSurface::report_backend_error(
    term::Terminal_backend_error   error,
    quint64                        sequence)
{
    if (sequence != 0U) {
        m_private->last_backend_error_signal_sequence = sequence;
    }

    emit backend_error(surface_backend_error_code(error.code), std::move(error.message));
}

void VNM_TerminalSurface::report_result_failure(
    const term::Terminal_session_result& result)
{
    if (result.error.has_value()) {
        if (result.sequence != 0U &&
            result.sequence == m_private->last_backend_error_signal_sequence)
        {
            return;
        }

        report_backend_error(*result.error, result.sequence);
        return;
    }

    report_backend_error({
        term::Terminal_backend_error_code::START_FAILED,
        QStringLiteral("terminal session command failed"),
    });
}

void VNM_TerminalSurface::reset_session()
{
    m_private->synchronized_output_recovery_timer.stop();
    ++m_private->session_generation;
    m_private->clear_pending_published_mouse_reports();
    m_private->resize_controller.reset();
    m_private->session.reset();
    m_private->transcript_recorder.reset();
    m_private->session_drain_queued.store(false);
    m_private->backend_callback_pressure_drain_requested.store(false);
    (void)m_private->cancel_backend_callback_frame_update_wait();
    m_private->
        before_backend_callback_frame_owner_release_handler_for_testing = {};
    m_private->reset_atlas_completion();
    m_private->clear_mouse_reporting_state();
    m_private->clear_hyperlink_activation_state();
    m_private->clear_selection_drag_state();
    m_private->pending_clipboard_write.reset();
    m_private->pending_text_area_resize_arbitration.reset();
    m_private->text_area_resize_arbitration_timer.stop();
    m_private->clear_wheel_remainders();
    m_private->last_sgr_mouse_reporting_active       = false;
    m_private->last_alternate_scroll_active          = false;
    m_private->last_alternate_scroll_mode_generation = 0U;
    set_selection_state(Selection_state::NONE);
    set_search_state(
        m_search_query,
        m_search_query.isEmpty()
            ? Search_result_state::INACTIVE
            : Search_result_state::SOURCE_UNAVAILABLE,
        0,
        0);
    term::Terminal_viewport_state empty_viewport;
    empty_viewport.visible_rows = std::max(0, m_rows);
    set_viewport_state(empty_viewport);
    if (m_private->shutting_down.load()) {
        m_private->ime_preedit = {};
    }
    else {
        m_private->set_ime_preedit_state(*this, {});
    }
    m_private->last_ime_preedit_generation        = 0U;
    m_private->last_backend_error_signal_sequence = 0U;
}

void VNM_TerminalSurface::updatePolish()
{
    Q_ASSERT(thread() == QThread::currentThread());

    refresh_grid_metrics_if_device_pixel_ratio_changed();

    // Polish runs before the scene graph syncs this item into the next frame.
    // Drain already-arrived backend callbacks here so a pending render update
    // does not capture a snapshot that is older than queued echo/output.
    if (m_private->session == nullptr || m_private->shutting_down.load()) {
        return;
    }
    term::Terminal_session* const session = m_private->session.get();
    const std::uint64_t session_generation = m_private->session_generation;
    if (!session->has_pending_backend_callback_events()) {
        if (session->render_snapshot_generation() !=
            m_private->last_installed_render_publication_generation)
        {
            sync_from_session();
            if (!m_private->active_session_matches(session, session_generation)) {
                return;
            }
        }
        return;
    }

    const std::uint64_t target_epoch = session->backend_callback_enqueue_epoch();
    if (target_epoch > session->backend_callback_processed_epoch()) {
        (void)drain_backend_callback_events_until_epoch(
            target_epoch,
            m_private->backend_callback_frame_catchup_budget());
    }
}

QSGNode* VNM_TerminalSurface::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    const std::shared_ptr<term::Hierarchical_profiler> render_profiler =
        m_private->render_profiler_handle();
#endif

    const auto update_node = [&]() -> QSGNode* {
        const QSizeF logical_size       = boundingRect().size();
        const qreal  device_pixel_ratio = m_private->render_device_pixel_ratio;

        if (window()              == nullptr   ||
            !std::isfinite(device_pixel_ratio) ||
            device_pixel_ratio    <= 0.0       ||
            !logical_size.isValid()            ||
            logical_size.width()  <= 0.0       ||
            logical_size.height() <= 0.0)
        {
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
            m_private->reset_atlas_completion();
            m_private->publish_renderer_frame(0U, 0U);
            if (old_node != nullptr) {
                if (m_private->qsg_atlas_render_node_live) {
                    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                    lifecycle_recorder != nullptr)
                    {
                        lifecycle_recorder->record_root_node_destroyed();
                        lifecycle_recorder->record_render_node_deleted();
                    }
                    m_private->qsg_atlas_render_node_live = false;
                }
            }
            delete old_node;
            return nullptr;
        }

        if (m_private->render_node_release_pending) {
            const bool requeue_render_update = m_private->render_node_release_requeue_update;
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
            m_private->reset_atlas_completion();
            m_private->publish_renderer_frame(0U, 0U);
            if (old_node != nullptr) {
                if (m_private->qsg_atlas_render_node_live) {
                    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                    lifecycle_recorder != nullptr)
                    {
                        lifecycle_recorder->record_root_node_destroyed();
                        lifecycle_recorder->record_render_node_deleted();
                    }
                    m_private->qsg_atlas_render_node_live = false;
                }
                delete old_node;
            }
            if (requeue_render_update) {
                m_private->request_render_update(*this);
            }
            return nullptr;
        }

        if (m_private->qsg_atlas_recorder == nullptr) {
            m_private->qsg_atlas_recorder =
                std::make_shared<term::Qsg_atlas_recorder>();
        }

        term::Terminal_render_options options = render_options_for_surface(*this);
        term::Captured_atlas_frame captured_frame =
            term::capture_qsg_atlas_frame(
                m_private->render_snapshot,
                m_private->ime_preedit,
                options,
                m_private->cell_metrics,
                logical_size,
                m_private->render_font,
#if VNM_TERMINAL_PROFILING_ENABLED
                render_profiler,
#else
                {},
#endif
                device_pixel_ratio,
                m_private->qsg_atlas_font_epoch,
                ++m_private->qsg_atlas_capture_sequence,
                m_private->cursor_blink_visible);
        const std::uint64_t captured_snapshot_sequence =
            atlas_frame_snapshot_sequence(captured_frame);
        const std::uint64_t captured_publication_generation =
            atlas_frame_publication_generation(captured_frame);
        const std::uint64_t captured_capture_sequence =
            captured_frame.capture_sequence;
        const bool created_render_node = old_node == nullptr;
        QSGNode* updated_node = term::update_qsg_atlas_node(
            old_node,
            std::move(captured_frame),
            m_private->qsg_atlas_recorder);
        if (created_render_node && updated_node != nullptr) {
            if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                lifecycle_recorder != nullptr)
            {
                lifecycle_recorder->record_root_node_created();
            }
            m_private->qsg_atlas_render_node_live = true;
        }
        m_private->publish_renderer_frame(
            captured_snapshot_sequence,
            captured_capture_sequence);
        if (updated_node != nullptr) {
            m_private->wait_for_atlas_completion(
                *this,
                window(),
                captured_snapshot_sequence,
                captured_publication_generation,
                captured_capture_sequence);
        }
        else {
            m_private->reset_render_update_schedule();
            m_private->reset_atlas_completion();
        }
        return updated_node;
    };

#if VNM_TERMINAL_PROFILING_ENABLED
    QSGNode* result_node = nullptr;
    {
        term::Active_profiler_binding render_profiler_binding(render_profiler.get());
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::updatePaintNode");
        result_node = update_node();
    }
    return result_node;
#else
    return update_node();
#endif
}

void term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
    VNM_TerminalSurface&                               surface,
    std::shared_ptr<const Terminal_render_snapshot>    snapshot)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->render_snapshot = std::move(snapshot);
    surface.refresh_hyperlink_hover_feedback();
    surface.updateInputMethod(Qt::ImCursorRectangle);
    surface.m_private->request_render_update(surface);
}

void term::VNM_TerminalSurface_render_bridge::set_render_profiler(
    VNM_TerminalSurface&                               surface,
    std::shared_ptr<Hierarchical_profiler>             profiler)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->set_render_profiler(std::move(profiler));
}

term::Render_profile_snapshot_t
term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->current_render_profiler_snapshot();
}

void term::VNM_TerminalSurface_render_bridge::set_dirty_row_stats_enabled(
    VNM_TerminalSurface&   surface,
    bool                   enabled)
{
    surface.set_dirty_row_stats_enabled(enabled);
}

void term::VNM_TerminalSurface_render_bridge::set_selection_trace_enabled(
    VNM_TerminalSurface&   surface,
    bool                   enabled)
{
    surface.set_selection_trace_enabled(enabled);
}

term::Qsg_atlas_frame_report
term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->qsg_atlas_recorder != nullptr
        ? surface.m_private->qsg_atlas_recorder->snapshot()
        : term::Qsg_atlas_frame_report{};
}

term::Terminal_screen_model_dirty_row_stats
term::VNM_TerminalSurface_render_bridge::dirty_row_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->dirty_row_stats()
        : term::Terminal_screen_model_dirty_row_stats{};
}

term::Terminal_screen_model_dirty_row_timeline
term::VNM_TerminalSurface_render_bridge::dirty_row_timeline(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->dirty_row_timeline()
        : term::Terminal_screen_model_dirty_row_timeline{};
}

term::Terminal_screen_model_profile_stats
term::VNM_TerminalSurface_render_bridge::model_profile_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->model_profile_stats()
        : term::Terminal_screen_model_profile_stats{};
}

term::terminal_retained_history_diagnostics_t
term::VNM_TerminalSurface_render_bridge::retained_history_diagnostics(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->retained_history_diagnostics()
        : term::terminal_retained_history_diagnostics_t{};
}

term::Terminal_session_profile_stats
term::VNM_TerminalSurface_render_bridge::session_profile_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->profile_stats()
        : term::Terminal_session_profile_stats{};
}

void term::VNM_TerminalSurface_render_bridge::set_session_profile_stats_enabled_for_benchmark(
    VNM_TerminalSurface&   surface,
    bool                   enabled)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->session != nullptr) {
        surface.m_private->session->set_profile_stats_enabled(enabled);
    }
}

void term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
    VNM_TerminalSurface&               surface,
    bool                               visible)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->cursor_blink_visible = visible;
    surface.m_private->request_render_update(surface);
}

void term::VNM_TerminalSurface_render_bridge::set_ime_preedit_state(
    VNM_TerminalSurface&               surface,
    Ime_preedit_state                  state)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->set_ime_preedit_state(surface, std::move(state));
}

bool term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
    VNM_TerminalSurface&               surface,
    std::unique_ptr<Terminal_backend>  backend,
    QStringList                        argv,
    QString                            working_directory)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());

    Terminal_launch_config launch_config;
    launch_config.argv              = std::move(argv);
    launch_config.working_directory = std::move(working_directory);
    return surface.start_process_with_backend(
        std::move(backend),
        std::move(launch_config));
}

bool term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session_drain_queued.load();
}

bool term::VNM_TerminalSurface_render_bridge::
backend_callback_frame_update_queued_for_testing(
    const VNM_TerminalSurface& surface)
{
    return surface.m_private->backend_callback_frame_update_queued.load();
}

bool term::VNM_TerminalSurface_render_bridge::
backend_callback_after_frame_wait_active_for_testing(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->backend_callback_after_frame_wait_active;
}

std::uint64_t term::VNM_TerminalSurface_render_bridge::
stale_after_frame_callback_ignored_count_for_testing(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->
        stale_after_frame_callback_ignored_count_for_testing;
}

std::size_t term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->pending_backend_callback_event_count()
        : 0U;
}

std::uint64_t term::VNM_TerminalSurface_render_bridge::backend_callback_enqueue_epoch(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->backend_callback_enqueue_epoch()
        : 0U;
}

std::uint64_t term::VNM_TerminalSurface_render_bridge::backend_callback_processed_epoch(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->backend_callback_processed_epoch()
        : 0U;
}

void term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.drain_backend_callback_events();
}

void term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events_for_posted_work(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.drain_backend_callback_events_for_posted_work();
}

void term::VNM_TerminalSurface_render_bridge::
process_backend_callbacks_without_notification_delivery_for_testing(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->session == nullptr) {
        return;
    }

    surface.m_private->session->process_backend_callback_events();
    surface.sync_from_session(false);
}

void term::VNM_TerminalSurface_render_bridge::set_backend_callback_frame_catchup_budget_for_benchmark(
    VNM_TerminalSurface&                    surface,
    std::chrono::steady_clock::duration     budget)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    const std::chrono::steady_clock::duration zero =
        std::chrono::steady_clock::duration::zero();
    surface.m_private->backend_callback_frame_catchup_budget_override =
        std::max(budget, zero);
}

std::chrono::steady_clock::duration
term::VNM_TerminalSurface_render_bridge::backend_callback_frame_catchup_budget_for_testing(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->backend_callback_frame_catchup_budget();
}

std::chrono::milliseconds
term::VNM_TerminalSurface_render_bridge::
backend_callback_frame_progress_watchdog_interval_for_testing()
{
    return std::chrono::milliseconds{
        k_backend_callback_frame_progress_watchdog_ms};
}

std::optional<std::chrono::steady_clock::time_point>
term::VNM_TerminalSurface_render_bridge::
backend_callback_frame_progress_deadline_for_testing(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->backend_callback_frame_progress_deadline;
}

void term::VNM_TerminalSurface_render_bridge::
set_before_backend_callback_frame_owner_release_handler_for_testing(
    VNM_TerminalSurface&       surface,
    std::function<void()>      handler)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->
        before_backend_callback_frame_owner_release_handler_for_testing =
            std::move(handler);
}

void term::VNM_TerminalSurface_render_bridge::
set_pending_published_mouse_report_block_count_for_testing(
    VNM_TerminalSurface&   surface,
    int                    count)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->pending_published_mouse_report_block_count_for_testing =
        std::max(0, count);
}

void term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
    VNM_TerminalSurface&   surface,
    std::function<void()>  handler)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->audible_bell_handler = std::move(handler);
}

void term::VNM_TerminalSurface_render_bridge::simulate_update_polish(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.updatePolish();
}

void term::VNM_TerminalSurface_render_bridge::handle_synchronized_output_recovery_timeout(
    VNM_TerminalSurface&                    surface,
    std::chrono::steady_clock::duration     budget)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.handle_synchronized_output_recovery_timeout(budget);
}

void term::VNM_TerminalSurface_render_bridge::
    handle_row_timestamp_tooltip_timeout_for_testing(
        VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->row_timestamp_tooltip_timer.stop();
    surface.handle_row_timestamp_tooltip_timeout();
}

std::shared_ptr<const term::Terminal_render_snapshot>
term::VNM_TerminalSurface_render_bridge::render_snapshot(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->render_snapshot;
}

term::terminal_cell_metrics_t
term::VNM_TerminalSurface_render_bridge::cell_metrics(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->cell_metrics;
}

term::Ime_preedit_state term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->ime_preedit;
}

term::Terminal_surface_render_invalidation_stats_t
term::VNM_TerminalSurface_render_bridge::invalidation_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->current_invalidation_stats(
        const_cast<VNM_TerminalSurface&>(surface));
}

std::uint64_t
term::VNM_TerminalSurface_render_bridge::session_rendered_render_snapshot_generation(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->rendered_render_snapshot_generation()
        : 0U;
}

term::Terminal_surface_backend_drain_stats_t
term::VNM_TerminalSurface_render_bridge::backend_drain_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->backend_drain_stats;
}

bool term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->atlas_completion_pending;
}

bool term::VNM_TerminalSurface_render_bridge::mark_completed_atlas_completion_pending_for_testing(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->render_snapshot == nullptr) {
        return false;
    }

    return mark_reported_atlas_completion_pending_for_testing(
        surface,
        surface.m_private->render_snapshot->metadata.publication_generation,
        true);
}

bool term::VNM_TerminalSurface_render_bridge::
mark_reported_atlas_completion_pending_for_testing(
    VNM_TerminalSurface&   surface,
    std::uint64_t          reported_publication_generation,
    bool                   drew)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->render_snapshot == nullptr) {
        return false;
    }

    surface.m_private->reset_render_update_schedule();
    surface.m_private->atlas_completion_pending              = true;
    surface.m_private->atlas_completion_complete_for_testing = true;
    surface.m_private->atlas_completion_snapshot_sequence =
        surface.m_private->render_snapshot->metadata.sequence;
    surface.m_private->atlas_completion_publication_generation =
        surface.m_private->render_snapshot->metadata.publication_generation;
    surface.m_private->atlas_completion_capture_sequence  =
        std::max<std::uint64_t>(surface.m_private->qsg_atlas_capture_sequence, 1U);
    surface.m_private->atlas_completion_report_generation_for_testing =
        reported_publication_generation;
    surface.m_private->atlas_completion_report_drew_for_testing = drew;
    return true;
}

void term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->reconcile_atlas_completion(surface);
}

term::terminal_renderer_lifecycle_stats_t
term::VNM_TerminalSurface_render_bridge::lifecycle_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    const auto lifecycle_recorder = surface.m_private->lifecycle_recorder();
    return lifecycle_recorder != nullptr
        ? lifecycle_recorder->snapshot()
        : term::terminal_renderer_lifecycle_stats_t{};
}

void term::VNM_TerminalSurface_render_bridge::set_qsg_atlas_render_node_live_for_testing(
    VNM_TerminalSurface&   surface,
    bool                   live)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->qsg_atlas_render_node_live = live;
}

std::shared_ptr<term::Terminal_renderer_lifecycle_recorder>
term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->ensure_lifecycle_recorder();
}

void term::VNM_TerminalSurface_render_bridge::release_resources(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.releaseResources();
}

void term::VNM_TerminalSurface_render_bridge::simulate_scene_graph_invalidated(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.handle_scene_graph_invalidated(surface.m_private->window_binding_generation);
}

void term::VNM_TerminalSurface_render_bridge::simulate_stale_scene_graph_invalidated(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    const std::uint64_t generation = surface.m_private->window_binding_generation;
    surface.handle_scene_graph_invalidated(
        generation == 0U
            ? ~std::uint64_t{0U}
            : generation - 1U);
}

void term::VNM_TerminalSurface_render_bridge::invalidate_public_projection_for_testing(
    VNM_TerminalSurface&                       surface,
    Terminal_public_projection_disable_reason  reason)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->session == nullptr) {
        return;
    }

    surface.m_private->session->invalidate_public_projection_for_testing(reason);
    surface.sync_from_session();
}
