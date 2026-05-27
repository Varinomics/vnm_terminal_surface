#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/linux_pty_backend.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_resize_controller.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"
#include <QColor>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethodEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QThread>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace term = vnm_terminal::internal;

namespace {

// The trace remains diagnostic; public signal delivery drains the session's
// durable notification channel during GUI-thread sync.
constexpr std::size_t k_surface_notification_trace_limit         = 0U;
constexpr qreal       k_font_zoom_min_pixel_size                 = 6.0;
constexpr qreal       k_font_zoom_max_pixel_size                 = 72.0;
constexpr qreal       k_font_zoom_wheel_step                     = 1.0;
constexpr qreal       k_angle_delta_per_wheel_step               = 120.0;
constexpr int         k_plain_scroll_lines_per_angle_step        = 3;
constexpr int         k_min_synchronized_output_stale_timeout_ms = 1;
constexpr std::chrono::milliseconds k_backend_callback_drain_budget{4};

bool same_grid_size(term::terminal_grid_size_t left, term::terminal_grid_size_t right)
{
    return left.rows == right.rows && left.columns == right.columns;
}

bool same_viewport_mapping(
    const term::Terminal_viewport_state&    left,
    const term::Terminal_viewport_state&    right)
{
    return
        left.active_buffer    == right.active_buffer    &&
        left.visible_rows     == right.visible_rows     &&
        left.scrollback_rows  == right.scrollback_rows  &&
        left.offset_from_tail == right.offset_from_tail;
}

int first_visible_logical_row_for_viewport(
    const term::Terminal_viewport_state& viewport)
{
    return viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
        ? 0
        : viewport.scrollback_rows - viewport.offset_from_tail;
}

bool same_visible_row_identity(
    const term::Terminal_viewport_state& left,
    const term::Terminal_viewport_state& right)
{
    return
        left.active_buffer == right.active_buffer &&
        left.visible_rows  == right.visible_rows  &&
        first_visible_logical_row_for_viewport(left) ==
            first_visible_logical_row_for_viewport(right);
}

bool same_viewport_row_identity_space(
    const term::Terminal_render_snapshot&  left,
    const term::Terminal_render_snapshot&  right)
{
    return
        same_grid_size(left.grid_size, right.grid_size)                  &&
        same_viewport_mapping(left.viewport, right.viewport);
}

void append_dirty_range_rows(
    std::vector<int>&      rows,
    const std::vector<term::Terminal_render_dirty_row_range>&
                           ranges)
{
    for (const term::Terminal_render_dirty_row_range& range : ranges) {
        for (int row = range.first_row; row < range.first_row + range.row_count; ++row) {
            rows.push_back(row);
        }
    }
}

std::shared_ptr<const term::Terminal_render_snapshot> snapshot_with_coalesced_dirty_rows(
    const term::Terminal_render_snapshot&  pending_snapshot,
    const term::Terminal_render_snapshot&  current_snapshot)
{
    term::Terminal_render_snapshot snapshot = current_snapshot;

    if (!same_viewport_row_identity_space(pending_snapshot, current_snapshot)) {
        snapshot.dirty_row_ranges =
            term::compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
    }

    std::vector<int> dirty_rows;
    append_dirty_range_rows(dirty_rows, pending_snapshot.dirty_row_ranges);
    append_dirty_range_rows(dirty_rows, current_snapshot.dirty_row_ranges);
    snapshot.dirty_row_ranges =
        term::compact_dirty_row_ranges(std::move(dirty_rows), snapshot.grid_size.rows, false);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
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

    return static_cast<qreal>(std::max(1, static_cast<int>(std::round(font_size))));
}

qreal finite_positive_font_size_or_default(qreal font_size)
{
    if (!std::isfinite(font_size) || font_size <= 0.0) {
        return term::k_vnm_terminal_default_font_pixel_size;
    }

    return font_size;
}

int wheel_steps_from_delta(int delta, qreal step_size, qreal& remainder)
{
    if (delta == 0 || !std::isfinite(step_size) || step_size <= 0.0) {
        return 0;
    }

    remainder += static_cast<qreal>(delta);
    const int steps = static_cast<int>(std::trunc(remainder / step_size));
    remainder -= static_cast<qreal>(steps) * step_size;
    return steps;
}

int vertical_wheel_steps(
    const QWheelEvent& event,
    qreal              pixel_step_size,
    qreal&             angle_remainder,
    qreal&             pixel_remainder)
{
    const int angle_delta = event.angleDelta().y();
    if (angle_delta != 0) {
        pixel_remainder = 0.0;
        return wheel_steps_from_delta(
            angle_delta,
            k_angle_delta_per_wheel_step,
            angle_remainder);
    }

    const int pixel_delta = event.pixelDelta().y();
    if (pixel_delta == 0) {
        return 0;
    }

    angle_remainder = 0.0;
    return wheel_steps_from_delta(pixel_delta, pixel_step_size, pixel_remainder);
}

bool has_vertical_wheel_delta(const QWheelEvent& event)
{
    return event.angleDelta().y() != 0 || event.pixelDelta().y() != 0;
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

bool is_light_theme(const QString& color_theme)
{
    return color_theme.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
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
    return term::make_linux_pty_backend();
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

    const int column = static_cast<int>(std::floor(point.x() / metrics.width));
    const int row    = static_cast<int>(std::floor(point.y() / metrics.height));
    if (row <  0 || column < 0 || row >= snapshot->grid_size.rows ||
        column >= snapshot->grid_size.columns)
    {
        return std::nullopt;
    }

    return term::terminal_grid_position_t{row, column};
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

    const int column = std::clamp(
        static_cast<int>(std::floor(point.x() / metrics.width)),
        0,
        snapshot->grid_size.columns - 1);
    const int row = std::clamp(
        static_cast<int>(std::floor(point.y() / metrics.height)),
        0,
        snapshot->grid_size.rows - 1);
    return term::terminal_grid_position_t{row, column};
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

bool selection_drag_sources_have_compatible_coordinates(
    const term::terminal_selection_source_identity_t& left,
    const term::terminal_selection_source_identity_t& right)
{
    return
        left.source_content_basis.grid_reflow_generation ==
            right.source_content_basis.grid_reflow_generation    &&
        left.session_epoch     == right.session_epoch            &&
        left.buffer_id         == right.buffer_id                &&
        left.grid_reflow_basis == right.grid_reflow_basis        &&
        same_grid_size(left.grid_size, right.grid_size);
}

bool selection_drag_sources_have_same_content_generation(
    const term::terminal_selection_source_identity_t& left,
    const term::terminal_selection_source_identity_t& right)
{
    return left.source_content_basis.content_generation ==
        right.source_content_basis.content_generation;
}

bool selection_source_matches_snapshot(
    const term::terminal_selection_source_identity_t& source,
    const term::Terminal_render_snapshot&             snapshot)
{
    return
        source.buffer_id == snapshot.viewport.active_buffer     &&
        source.row_origin_generation == snapshot.metadata.row_origin_generation &&
        same_grid_size(source.grid_size, snapshot.grid_size)    &&
        same_viewport_mapping(source.viewport_mapping, snapshot.viewport);
}

void write_selection_trace(bool enabled, const QString& message)
{
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
    if (!same_grid_size(left.grid_size, right.grid_size)) {
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
    if (!same_grid_size(source->grid_size, snapshot->grid_size)) {
        append_selection_trace_reason(reasons, QStringLiteral("grid-size"));
    }
    if (!same_viewport_mapping(source->viewport_mapping, snapshot->viewport)) {
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

enum class Selection_drag_content_validation_status
{
    ACCEPTED,
    MISSING_SNAPSHOT,
    ROW_ORIGIN_CHANGED,
    ROW_ORIGIN_AMBIGUOUS,
    RANGE_NOT_VISIBLE,
    SELECTED_ROW_CONTENT_CHANGED,
};

QString selection_trace_drag_content_validation_status(
    Selection_drag_content_validation_status status)
{
    switch (status) {
        case Selection_drag_content_validation_status::ACCEPTED:
            return QStringLiteral("accepted");
        case Selection_drag_content_validation_status::MISSING_SNAPSHOT:
            return QStringLiteral("missing-snapshot");
        case Selection_drag_content_validation_status::ROW_ORIGIN_CHANGED:
            return QStringLiteral("row-origin-changed");
        case Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS:
            return QStringLiteral("row-origin-ambiguous");
        case Selection_drag_content_validation_status::RANGE_NOT_VISIBLE:
            return QStringLiteral("range-not-visible");
        case Selection_drag_content_validation_status::SELECTED_ROW_CONTENT_CHANGED:
            return QStringLiteral("selected-row-content");
    }

    return QStringLiteral("unknown");
}

Selection_drag_content_validation_status validate_selection_drag_content_drift(
    const term::terminal_selection_source_identity_t&             anchor_source,
    const term::terminal_selection_source_identity_t&             current_source,
    const std::shared_ptr<const term::Terminal_render_snapshot>&  anchor_snapshot,
    const std::shared_ptr<const term::Terminal_render_snapshot>&  current_snapshot,
    const term::Terminal_selection_range&                         range,
    int                                                           scrollback_limit)
{
    if (anchor_snapshot == nullptr || current_snapshot == nullptr) {
        return Selection_drag_content_validation_status::MISSING_SNAPSHOT;
    }

    if (anchor_source.row_origin_generation != current_source.row_origin_generation) {
        return Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS;
    }

    if (selection_drag_sources_have_same_content_generation(anchor_source, current_source)) {
        return Selection_drag_content_validation_status::ACCEPTED;
    }

    if (!same_grid_size(anchor_snapshot->grid_size, current_snapshot->grid_size)) {
        return Selection_drag_content_validation_status::ROW_ORIGIN_CHANGED;
    }

    const term::terminal_grid_position_t start = term::normalized_selection_start(range);
    const term::terminal_grid_position_t end   = term::normalized_selection_end(range);
    const int anchor_first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(*anchor_snapshot);
    const int current_first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(*current_snapshot);
    if (anchor_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY) {
        const int anchor_scrollback_rows  = anchor_snapshot->viewport.scrollback_rows;
        const int current_scrollback_rows = current_snapshot->viewport.scrollback_rows;
        if (scrollback_limit <= 0 &&
            (start.row < anchor_first_visible_logical_row ||
                start.row < current_first_visible_logical_row))
        {
            return Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS;
        }
        if (current_scrollback_rows < anchor_scrollback_rows) {
            return Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS;
        }
        if (scrollback_limit > 0 &&
            current_scrollback_rows >= scrollback_limit &&
            (current_scrollback_rows != anchor_scrollback_rows ||
                anchor_scrollback_rows >= scrollback_limit))
        {
            return Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS;
        }
    }

    if (start.row < anchor_first_visible_logical_row ||
        end.row   >= anchor_first_visible_logical_row + anchor_snapshot->grid_size.rows ||
        start.row < current_first_visible_logical_row ||
        end.row   >= current_first_visible_logical_row + current_snapshot->grid_size.rows)
    {
        return Selection_drag_content_validation_status::RANGE_NOT_VISIBLE;
    }

    const std::vector<const term::Terminal_render_cell*> anchor_cells =
        term::render_snapshot_cells_by_position(*anchor_snapshot);
    const std::vector<const term::Terminal_render_cell*> current_cells =
        term::render_snapshot_cells_by_position(*current_snapshot);
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int anchor_viewport_row  = logical_row - anchor_first_visible_logical_row;
        const int current_viewport_row = logical_row - current_first_visible_logical_row;
        const QString anchor_row =
            term::selected_text_from_render_snapshot_row(
                *anchor_snapshot,
                anchor_cells,
                anchor_viewport_row,
                0,
                anchor_snapshot->grid_size.columns,
                false);
        const QString current_row =
            term::selected_text_from_render_snapshot_row(
                *current_snapshot,
                current_cells,
                current_viewport_row,
                0,
                current_snapshot->grid_size.columns,
                false);
        if (anchor_row != current_row) {
            return Selection_drag_content_validation_status::SELECTED_ROW_CONTENT_CHANGED;
        }
    }

    return Selection_drag_content_validation_status::ACCEPTED;
}

bool selection_drag_content_validation_accepted(
    Selection_drag_content_validation_status status)
{
    return status == Selection_drag_content_validation_status::ACCEPTED;
}

bool selection_drag_content_validation_allows_payload_detach(
    Selection_drag_content_validation_status status)
{
    return
        status == Selection_drag_content_validation_status::ROW_ORIGIN_AMBIGUOUS ||
        status == Selection_drag_content_validation_status::RANGE_NOT_VISIBLE ||
        status == Selection_drag_content_validation_status::SELECTED_ROW_CONTENT_CHANGED;
}

void cancel_selection_drag_after_content_validation_failure(
    term::Terminal_session&                 session,
    Selection_drag_content_validation_status status,
    bool                                    drag_moved)
{
    if (drag_moved && selection_drag_content_validation_allows_payload_detach(status)) {
        session.detach_selection_visual_attachment();
        return;
    }

    session.clear_selection();
}

void set_selection_range_for_drag(
    term::Terminal_session&        session,
    term::terminal_grid_position_t anchor,
    term::terminal_grid_position_t current,
    bool                           drag_moved,
    const term::terminal_selection_source_identity_t& source)
{
    if (!drag_moved && current == anchor) {
        session.clear_selection();
        return;
    }

    session.set_selection_range_from_drained_published_source(
        selection_range_for_drag(anchor, current),
        source);
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

term::Terminal_render_options render_options_for_surface(const VNM_TerminalSurface& surface)
{
    term::Terminal_render_options options;
    const bool light_theme = is_light_theme(surface.color_theme());
    if (light_theme) {
        options.default_background   = QColor(246, 247, 242);
        options.default_foreground   = QColor(31,  44,  38);
        options.selection_background = QColor(95, 145, 190, 150);
        options.cursor_color         = QColor(30, 46, 38);
        options.preedit_background   = QColor(220, 220, 205, 170);
        options.visual_bell_color    = QColor(255, 255, 255, 96);
    }

    options.cursor_shape_override         = terminal_cursor_shape(surface.cursor_style());
    options.cursor_blink_enabled_override = surface.cursor_blink_enabled();
    options.visual_bell_enabled =
        surface.visual_bell_policy() == VNM_TerminalSurface::Bell_policy::ENABLED;
    return options;
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

QString wheel_trace_scroll_action_name(term::Terminal_viewport_scroll_action action)
{
    switch (action) {
        case term::Terminal_viewport_scroll_action::VIEWPORT_MOVED:
            return QStringLiteral("viewport_moved");
        case term::Terminal_viewport_scroll_action::AT_BOUNDARY:
            return QStringLiteral("at_boundary");
        case term::Terminal_viewport_scroll_action::TERMINAL_INPUT:
            return QStringLiteral("terminal_input");
    }

    return QStringLiteral("unknown");
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
    if (recorder == nullptr ||
        result.action != term::Terminal_viewport_scroll_action::VIEWPORT_MOVED)
    {
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

    void publish_render_profiler_snapshot(
        const std::shared_ptr<term::Hierarchical_profiler>&
                               profiler,
        std::uint64_t          sequence)
    {
        if (profiler == nullptr) {
            return;
        }

        term::Profile_node_snapshot root_snapshot = profiler->root_snapshot();
        term::Profile_timeline_snapshot timeline_snapshot = profiler->timeline_snapshot();
        term::terminal_text_layout_slow_diagnostics_t slow_text_layouts;
        if (const std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
            recorder = slow_text_layout_recorder_handle();
            recorder != nullptr)
        {
            slow_text_layouts = recorder->snapshot();
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        if (render_profiler != profiler) {
            return;
        }

        render_profiler_snapshot.sequence          = sequence;
        render_profiler_snapshot.root              = std::move(root_snapshot);
        render_profiler_snapshot.timeline          = std::move(timeline_snapshot);
        render_profiler_snapshot.slow_text_layouts = std::move(slow_text_layouts);
    }

    term::Render_profile_snapshot_t current_render_profiler_snapshot() const
    {
        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
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
        surface.update();
    }

    void reset_render_update_schedule()
    {
        render_update_pending = false;
        render_update_window = nullptr;
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
        ++render_invalidation_stats.consumed_updates;
    }

    term::Terminal_surface_render_invalidation_stats_t current_invalidation_stats() const
    {
        term::Terminal_surface_render_invalidation_stats_t stats = render_invalidation_stats;
        stats.pending_update = render_update_pending;
        return stats;
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

    void clear_selection_drag_state()
    {
        selection_anchor.reset();
        selection_anchor_buffer_id.reset();
        selection_anchor_source.reset();
        selection_anchor_snapshot.reset();
        selection_drag_active    = false;
        selection_drag_moved     = false;
        selection_drag_cancelled = false;
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

    term::Qt_grid_metrics_provider                         grid_metrics_provider;
    term::terminal_cell_metrics_t                          cell_metrics;
    QFont                                                  render_font;
    qreal                                                  render_device_pixel_ratio             = 1.0;
    bool                                                   render_light_theme                    = false;
    std::shared_ptr<const term::Terminal_render_snapshot>  render_snapshot;
    term::Ime_preedit_state                                ime_preedit;
    bool                                                   cursor_blink_visible                  = true;
    term::Qsg_terminal_renderer                            renderer;
    term::Terminal_renderer_stats_publisher                renderer_stats_publisher;
#if VNM_TERMINAL_PROFILING_ENABLED
    mutable std::mutex                                     render_profiler_mutex;
    std::shared_ptr<term::Hierarchical_profiler>           render_profiler;
    term::Render_profile_snapshot_t                        render_profiler_snapshot;
    std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
                                                           slow_text_layout_recorder;
    std::atomic_bool                                       render_profiler_enabled               = false;
#endif
    mutable std::mutex                                     renderer_lifecycle_recorder_mutex;
    std::shared_ptr<Renderer_lifecycle_recorder>           renderer_lifecycle_recorder;
    std::atomic_bool                                  renderer_lifecycle_recorder_enabled =
                                                           false;

    term::Terminal_surface_render_invalidation_stats_t     render_invalidation_stats;
    bool                                                   render_update_pending              = false;
    bool                                                   render_node_release_pending        = false;
    bool                                                   render_node_release_requeue_update = false;
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
    std::optional<term::terminal_grid_position_t>          selection_anchor;
    std::optional<term::Terminal_buffer_id>                selection_anchor_buffer_id;
    std::optional<term::terminal_selection_source_identity_t>
                                                           selection_anchor_source;
    std::shared_ptr<const term::Terminal_render_snapshot>  selection_anchor_snapshot;
    std::optional<term::Terminal_osc52_write_request>      pending_clipboard_write;
    QString                                                warmed_prompt_text_layout_font_key;
    QTimer                                                 synchronized_output_recovery_timer;
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
    std::unique_ptr<term::Terminal_resize_controller>      resize_controller;
    std::shared_ptr<term::Terminal_transcript_recorder>    transcript_recorder;
    std::uint64_t                                          last_render_snapshot_generation       = 0U;
    std::uint64_t                                          last_ime_preedit_generation           = 0U;
    std::uint64_t                                          last_backend_error_signal_sequence    = 0U;
    std::uint64_t                                          next_clipboard_write_request_id       = 1U;
    bool                                                   dirty_row_stats_enabled               = false;
    // These atomics are read by backend callback threads through the notifier.
    // reset_session() must close the session before Private storage is destroyed.
    std::atomic_bool                                       session_drain_queued                  = false;
    std::atomic_bool                                       shutting_down                         = false;
};

VNM_TerminalSurface::VNM_TerminalSurface(QQuickItem* parent)
:
    QQuickItem(parent),
    m_private(std::make_unique<Private>())
{
    m_font_family = term::vnm_terminal_default_monospace_font_family();
    m_font_size = term::k_vnm_terminal_default_font_pixel_size;
    setFlag(ItemHasContents,        true);
    setFlag(ItemAcceptsInputMethod, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFocus(true);
    m_private->synchronized_output_recovery_timer.setSingleShot(true);
    QObject::connect(
        &m_private->synchronized_output_recovery_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_synchronized_output_recovery_timeout();
        });
    refresh_grid_metrics();
}

VNM_TerminalSurface::~VNM_TerminalSurface()
{
    Q_ASSERT(thread() == QThread::currentThread());

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

    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_release_resources();
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

QString VNM_TerminalSurface::color_theme() const
{
    return m_color_theme;
}

void VNM_TerminalSurface::set_color_theme(const QString& color_theme)
{
    if (m_color_theme == color_theme) {
        return;
    }

    m_color_theme = color_theme;
    m_private->render_light_theme = is_light_theme(m_color_theme);
    emit color_theme_changed();
    m_private->request_render_update(*this);
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

QString VNM_TerminalSurface::backend_output_capture_path() const
{
    return m_backend_output_capture_path;
}

void VNM_TerminalSurface::set_backend_output_capture_path(const QString& path)
{
    m_backend_output_capture_path = path;
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
    int                applied_line_delta)
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
    term::insert_wheel_trace_scroll_publication_fields(
        object,
        local_scroll_applied,
        render_publication_blocked);
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
    emit visual_bell_policy_changed();
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

    QGuiApplication::clipboard()->setText(
        QString::fromUtf8(request.decoded_payload),
        QClipboard::Clipboard);
    return true;
}

QString VNM_TerminalSurface::selected_text()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface selected-text reason=no-session"));
        }
        return {};
    }

    drain_backend_callback_events();
    const term::Terminal_selection_result result = m_private->session->selected_text();
    if (m_selection_trace_enabled) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface selected-text result=%1 size=%2")
                .arg(static_cast<int>(result.code))
                .arg(result.text.size()));
    }
    return result.code == term::Terminal_selection_result_code::OK
        ? result.text
        : QString();
}

bool VNM_TerminalSurface::copy_selected_text_to_clipboard()
{
    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface copy-selected-text reason=no-session"));
        }
        return false;
    }

    const term::Terminal_selection_result result = m_private->session->selected_text();
    if (result.code != term::Terminal_selection_result_code::OK) {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral("surface copy-selected-text result=%1 size=%2")
                    .arg(static_cast<int>(result.code))
                    .arg(result.text.size()));
        }
        return false;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard != nullptr) {
        clipboard->setText(result.text, QClipboard::Clipboard);
    }
    if (m_selection_trace_enabled) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface copy-selected-text result=%1 size=%2 clipboard=%3")
                .arg(static_cast<int>(result.code))
                .arg(result.text.size())
                .arg(selection_trace_bool(clipboard != nullptr)));
    }
    return true;
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
    m_private->session->clear_selection();
    sync_from_session();
}

bool VNM_TerminalSurface::paste_text(QString text)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return false;
    }

    const term::Terminal_paste_text_result paste_result =
        m_private->session->write_paste_text(
            std::move(text),
            paste_framing_policy(m_bracketed_paste_policy));
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

bool VNM_TerminalSurface::scroll_viewport_lines(int line_delta)
{
    return scroll_viewport_lines_with_diagnostics(line_delta).local_scroll_applied;
}

VNM_TerminalSurface::wheel_scroll_diagnostic_result_t
VNM_TerminalSurface::scroll_viewport_lines_with_diagnostics(int line_delta)
{
    Q_ASSERT(thread() == QThread::currentThread());

    wheel_scroll_diagnostic_result_t diagnostic;
    if (line_delta == 0) {
        diagnostic.no_op_cause = QStringLiteral("zero_line_delta");
        return diagnostic;
    }

    diagnostic.session_present = m_private->session != nullptr;
    if (!diagnostic.session_present) {
        diagnostic.no_op_cause = QStringLiteral("no_session");
        return diagnostic;
    }

    diagnostic.render_publication_blocked =
        m_private->session->render_publication_blocked();
    diagnostic.published_synchronized_output =
        m_private->render_snapshot != nullptr &&
        m_private->render_snapshot->modes.synchronized_output;
    if (m_private->render_snapshot != nullptr) {
        diagnostic.alternate_screen =
            m_private->render_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::ALTERNATE;
    }

    const term::Terminal_viewport_state viewport_before =
        m_private->session->viewport_state();
    diagnostic.alternate_screen =
        diagnostic.alternate_screen ||
        viewport_before.active_buffer == term::Terminal_buffer_id::ALTERNATE;
    record_surface_scroll_intent_transcript(
        m_private->transcript_recorder,
        QStringLiteral("api.lines"),
        line_delta,
        std::nullopt,
        viewport_before);
    diagnostic.local_scroll_intent_recorded = true;
    const term::Terminal_viewport_scroll_result scroll_result =
        m_private->session->scroll_published_viewport_lines(line_delta);
    diagnostic.scroll_action =
        wheel_trace_scroll_action_name(scroll_result.action);
    diagnostic.applied_line_delta = scroll_result.applied_line_delta;
    sync_from_session();
    if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        record_surface_scroll_transcript(
            m_private->transcript_recorder,
            QStringLiteral("api.lines"),
            line_delta,
            std::nullopt,
            scroll_result,
            viewport_before,
            m_private->session->viewport_state());
        diagnostic.local_scroll_applied = true;
        return diagnostic;
    }

    if (diagnostic.render_publication_blocked) {
        diagnostic.no_op_cause = QStringLiteral("synchronized_output_deferred");
    }
    else
    if (diagnostic.published_synchronized_output) {
        diagnostic.no_op_cause = QStringLiteral("synchronized_output_published");
    }
    else
    if (diagnostic.alternate_screen) {
        diagnostic.no_op_cause = QStringLiteral("alternate_screen");
    }
    else
    if (scroll_result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY) {
        diagnostic.no_op_cause = QStringLiteral("boundary_or_clamp");
    }
    else {
        diagnostic.no_op_cause = QStringLiteral("no_publication");
    }

    return diagnostic;
}

bool VNM_TerminalSurface::scroll_to_offset_from_tail(int offset_from_tail)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return false;
    }

    const term::Terminal_viewport_state viewport_before =
        m_private->session->viewport_state();
    record_surface_scroll_intent_transcript(
        m_private->transcript_recorder,
        QStringLiteral("api.offset"),
        offset_from_tail - viewport_before.offset_from_tail,
        offset_from_tail,
        viewport_before);
    const term::Terminal_viewport_scroll_result scroll_result =
        m_private->session->scroll_published_viewport_to_offset_from_tail(offset_from_tail);
    sync_from_session();
    if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        record_surface_scroll_transcript(
            m_private->transcript_recorder,
            QStringLiteral("api.offset"),
            scroll_result.applied_line_delta,
            offset_from_tail,
            scroll_result,
            viewport_before,
            m_private->session->viewport_state());
    }
    return scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
}

bool VNM_TerminalSurface::start_process(QStringList argv, QString working_directory)
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
    }

    if (argv.isEmpty() || argv.front().trimmed().isEmpty()) {
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
        std::move(argv),
        std::move(working_directory));
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

    const term::Terminal_session_result result = m_private->session->interrupt();
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

    const term::Terminal_session_result result = m_private->session->terminate();
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
            const term::Terminal_focus_event_result focus_result =
                m_private->session->write_focus_event(false);
            m_private->session->cancel_ime_preedit();
            sync_from_session();
            if (focus_result.handled && !is_accepted(focus_result.result.code)) {
                report_result_failure(focus_result.result);
            }
            return;
        }

        term::Ime_preedit_state state;
        m_private->set_ime_preedit_state(*this, std::move(state));
    }
    else
    if (change == ItemActiveFocusHasChanged) {
        if (m_private->session != nullptr) {
            const term::Terminal_focus_event_result focus_result =
                m_private->session->write_focus_event(true);
            sync_from_session();
            if (focus_result.handled && !is_accepted(focus_result.result.code)) {
                report_result_failure(focus_result.result);
            }
        }
    }
}

void VNM_TerminalSurface::keyPressEvent(QKeyEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (is_plain_copy_shortcut(*event)) {
        if (m_copy_shortcut_policy ==
            Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT)
        {
            if (copy_selected_text_to_clipboard()) {
                event->accept();
                return;
            }
        }
        else
        if (m_copy_shortcut_policy == Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE) {
            (void)copy_selected_text_to_clipboard();
            event->accept();
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
            const term::Terminal_viewport_state viewport =
                m_private->session->viewport_state();
            if (viewport.active_buffer   == term::Terminal_buffer_id::PRIMARY &&
                viewport.scrollback_rows >  0)
            {
                const int visible_rows =
                    std::max(1, viewport.visible_rows);
                const term::Terminal_viewport_state viewport_before =
                    m_private->session->viewport_state();
                record_surface_scroll_intent_transcript(
                    m_private->transcript_recorder,
                    QStringLiteral("page_key"),
                    direction * visible_rows,
                    std::nullopt,
                    viewport_before);
                const term::Terminal_viewport_scroll_result scroll_result =
                    m_private->session->scroll_viewport_lines(direction * visible_rows);
                if (scroll_result.action ==
                    term::Terminal_viewport_scroll_action::VIEWPORT_MOVED)
                {
                    sync_from_session();
                    record_surface_scroll_transcript(
                        m_private->transcript_recorder,
                        QStringLiteral("page_key"),
                        direction * visible_rows,
                        std::nullopt,
                        scroll_result,
                        viewport_before,
                        m_private->session->viewport_state());
                    event->accept();
                    return;
                }

                event->accept();
                return;
            }
        }

        const term::Terminal_key_event_result key_result =
            m_private->session->write_key_event(*event);
        if (!key_result.handled) {
            event->ignore();
            return;
        }

        event->accept();
        sync_from_session();
        if (!is_accepted(key_result.result.code)) {
            report_result_failure(key_result.result);
        }
        return;
    }

    const QByteArray bytes =
        term::encode_terminal_key_event(*event, term::Terminal_input_mode_state{});
    if (bytes.isEmpty()) {
        event->ignore();
        return;
    }

    event->accept();
}

void VNM_TerminalSurface::mousePressEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();

    forceActiveFocus(Qt::MouseFocusReason);
    drain_backend_callback_events();

    const term::Terminal_mouse_button button = terminal_mouse_button(event->button());
    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    const bool force_local_selection = local_selection_override(event->modifiers());
    std::optional<term::terminal_grid_position_t> logical_position;
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled,
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

    if (!force_local_selection &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr &&
        position.has_value()                &&
        button                   != term::Terminal_mouse_button::NONE)
    {
        const term::Terminal_mouse_event_result mouse_result =
            m_private->session->write_mouse_event({
                term::Terminal_mouse_event_kind::PRESS,
                button,
                position->row,
                position->column,
                event->modifiers(),
        });
        if (mouse_result.handled) {
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
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            if (!is_accepted(mouse_result.result.code)) {
                report_result_failure(mouse_result.result);
            }
            trace_decision(QStringLiteral("mouse-reporting"));
            return;
        }
        if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
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
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("mouse-reporting"));
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr && paste_text(clipboard->text())) {
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
        if (m_selection_trace_enabled) {
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

    m_private->selection_anchor           = *logical_position;
    m_private->selection_anchor_buffer_id = m_private->render_snapshot->viewport.active_buffer;
    m_private->selection_anchor_source    = *source;
    m_private->selection_anchor_snapshot  = m_private->render_snapshot;
    m_private->selection_drag_active      = true;
    m_private->selection_drag_moved       = false;
    m_private->selection_drag_cancelled   = false;
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("start"),
        m_private->selection_anchor,
        logical_position,
        std::nullopt,
        false);
    m_private->session->clear_selection();
    event->accept();
    sync_from_session();
    if (m_selection_trace_enabled) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface mouse-press source anchor=%1 current=%2 %3")
                .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                .arg(selection_trace_source_identity(source))
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
    }
    trace_decision(QStringLiteral("clear-on-click"));
}

void VNM_TerminalSurface::mouseMoveEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    std::optional<term::terminal_grid_position_t> viewport_position = position;
    std::optional<term::terminal_grid_position_t> logical_position;
    const bool force_local_selection = local_selection_override(event->modifiers());
    const bool terminal_mouse_grab_active =
        m_private->mouse_reporting_pressed_buttons != Qt::NoButton;
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled,
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

    if (!m_private->selection_drag_active &&
        (!force_local_selection || terminal_mouse_grab_active)       &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr)
    {
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
            const term::Terminal_mouse_event_result mouse_result =
                m_private->session->write_mouse_event({
                    kind,
                    button,
                    report_position->row,
                    report_position->column,
                    event->modifiers(),
                });
            if (mouse_result.handled) {
                m_private->mouse_reporting_last_position = *report_position;
                m_private->clear_selection_drag_state();
                record_surface_selection_drag_transcript(
                    m_private->transcript_recorder,
                    QStringLiteral("clear"),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    false);
                m_private->session->clear_selection();
                event->accept();
                sync_from_session();
                if (!is_accepted(mouse_result.result.code)) {
                    report_result_failure(mouse_result.result);
                }
                viewport_position = report_position;
                trace_decision(QStringLiteral("mouse-reporting"));
                return;
            }
            if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
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

    if (!m_private->selection_drag_active                  ||
        !m_private->selection_anchor.has_value()           ||
        !m_private->selection_anchor_buffer_id.has_value() ||
        !m_private->selection_anchor_source.has_value()    ||
        m_private->session                  == nullptr     ||
        (event->buttons() & Qt::LeftButton) == Qt::NoButton)
    {
        trace_decision(QStringLiteral("drag-inactive"));
        return;
    }

    if (m_private->selection_drag_cancelled) {
        event->accept();
        trace_decision(QStringLiteral("drag-cancelled"));
        return;
    }

    if (m_private->render_snapshot == nullptr ||
        m_private->render_snapshot->viewport.active_buffer !=
            *m_private->selection_anchor_buffer_id)
    {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-move source-mismatch reason=buffer anchor=%1 current=source{none} %2")
                    .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
        }
        record_surface_selection_drag_transcript(
            m_private->transcript_recorder,
            QStringLiteral("cancel"),
            m_private->selection_anchor,
            logical_position,
            std::nullopt,
            m_private->selection_drag_moved);
        m_private->clear_selection_drag_state();
        m_private->session->clear_selection();
        event->accept();
        sync_from_session();
        trace_decision(QStringLiteral("source-mismatch"));
        return;
    }

    if (m_private->session->render_publication_blocked()) {
        event->accept();
        trace_decision(QStringLiteral("publication-blocked"));
        return;
    }

    const std::optional<term::terminal_selection_source_identity_t> source =
        m_private->session->published_selection_source_identity();
    if (m_selection_trace_enabled) {
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface mouse-move source anchor=%1 current=%2 %3")
                .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                .arg(selection_trace_source_identity(source))
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
    }
    if (!source.has_value() ||
        !selection_source_matches_snapshot(*source, *m_private->render_snapshot) ||
        !selection_drag_sources_have_compatible_coordinates(
            *m_private->selection_anchor_source,
            *source))
    {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-move source-mismatch snapshot_reason=%1 anchor_reason=%2")
                    .arg(selection_trace_source_snapshot_mismatch_reason(
                        source,
                        m_private->render_snapshot))
                    .arg(source.has_value()
                        ? selection_trace_drag_coordinate_mismatch_reason(
                            *m_private->selection_anchor_source,
                            *source)
                        : QStringLiteral("source-missing")));
        }
        m_private->selection_drag_cancelled = true;
        record_surface_selection_drag_transcript(
            m_private->transcript_recorder,
            QStringLiteral("cancel"),
            m_private->selection_anchor,
            logical_position,
            std::nullopt,
            m_private->selection_drag_moved);
        m_private->session->clear_selection();
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

    const term::Terminal_selection_range range =
        selection_range_for_drag(*m_private->selection_anchor, *logical_position);
    const Selection_drag_content_validation_status content_validation =
        validate_selection_drag_content_drift(
            *m_private->selection_anchor_source,
            *source,
            m_private->selection_anchor_snapshot,
            m_private->render_snapshot,
            range,
            m_scrollback_limit);
    if (!selection_drag_content_validation_accepted(content_validation))
    {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral(
                    "surface mouse-move source-mismatch reason=%1 "
                    "range=%2 anchor=%3 current=%4 anchor_snapshot=%5 current_snapshot=%6")
                    .arg(selection_trace_drag_content_validation_status(content_validation))
                    .arg(selection_trace_range(range))
                    .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                    .arg(selection_trace_source_identity(*source))
                    .arg(selection_trace_snapshot_identity(m_private->selection_anchor_snapshot))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
        }
        m_private->selection_drag_cancelled = true;
        record_surface_selection_drag_transcript(
            m_private->transcript_recorder,
            QStringLiteral("cancel"),
            m_private->selection_anchor,
            logical_position,
            range,
            m_private->selection_drag_moved);
        cancel_selection_drag_after_content_validation_failure(
            *m_private->session,
            content_validation,
            m_private->selection_drag_moved);
        event->accept();
        sync_from_session();
        trace_decision(QStringLiteral("source-mismatch"));
        return;
    }

    m_private->selection_drag_moved = true;
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("update"),
        m_private->selection_anchor,
        logical_position,
        range,
        true);
    m_private->session->set_selection_range_from_drained_published_source(range, *source);
    event->accept();
    sync_from_session();
    trace_decision(QStringLiteral("selection-range-set"));
}

void VNM_TerminalSurface::mouseReleaseEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

    std::optional<term::terminal_grid_position_t> viewport_position;
    std::optional<term::terminal_grid_position_t> logical_position;
    const auto trace_decision = [&](const QString& reason) {
        trace_surface_mouse_decision(
            m_selection_trace_enabled,
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

    if (m_private->selection_drag_active &&
        event->button()    == Qt::LeftButton              &&
        m_private->selection_anchor.has_value()           &&
        m_private->selection_anchor_buffer_id.has_value() &&
        m_private->selection_anchor_source.has_value()    &&
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
                m_private->session->clear_selection();
            } else {
                m_private->session->detach_selection_visual_attachment();
            }
            m_private->clear_selection_drag_state();
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("publication-blocked"));
            return;
        }

        if (m_private->render_snapshot == nullptr ||
            m_private->render_snapshot->viewport.active_buffer !=
                *m_private->selection_anchor_buffer_id)
        {
            if (m_selection_trace_enabled) {
                write_selection_trace(m_selection_trace_enabled,
                    QStringLiteral(
                        "surface mouse-release source-mismatch reason=buffer anchor=%1 current=source{none} %2")
                    .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
            }
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("cancel"),
                m_private->selection_anchor,
                logical_position,
                std::nullopt,
                m_private->selection_drag_moved);
            m_private->clear_selection_drag_state();
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            trace_decision(QStringLiteral("source-mismatch"));
            return;
        }

        const std::optional<term::terminal_selection_source_identity_t> source =
            m_private->session->published_selection_source_identity();
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled,
                QStringLiteral("surface mouse-release source anchor=%1 current=%2 %3")
                    .arg(selection_trace_source_identity(*m_private->selection_anchor_source))
                    .arg(selection_trace_source_identity(source))
                    .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
        }
        if (!source.has_value() ||
            !selection_source_matches_snapshot(*source, *m_private->render_snapshot) ||
            !selection_drag_sources_have_compatible_coordinates(
                *m_private->selection_anchor_source,
                *source))
        {
            if (m_selection_trace_enabled) {
                write_selection_trace(m_selection_trace_enabled,
                    QStringLiteral(
                        "surface mouse-release source-mismatch snapshot_reason=%1 anchor_reason=%2")
                        .arg(selection_trace_source_snapshot_mismatch_reason(
                            source,
                            m_private->render_snapshot))
                        .arg(source.has_value()
                        ? selection_trace_drag_coordinate_mismatch_reason(
                            *m_private->selection_anchor_source,
                            *source)
                        : QStringLiteral("source-missing")));
            }
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("cancel"),
                m_private->selection_anchor,
                logical_position,
                std::nullopt,
                m_private->selection_drag_moved);
            m_private->clear_selection_drag_state();
            m_private->session->clear_selection();
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
            const term::Terminal_selection_range range =
                selection_range_for_drag(*m_private->selection_anchor, *logical_position);
            const bool selection_range_needed =
                m_private->selection_drag_moved ||
                *logical_position != *m_private->selection_anchor;
            const Selection_drag_content_validation_status content_validation =
                selection_range_needed
                    ? validate_selection_drag_content_drift(
                        *m_private->selection_anchor_source,
                        *source,
                        m_private->selection_anchor_snapshot,
                        m_private->render_snapshot,
                        range,
                        m_scrollback_limit)
                    : Selection_drag_content_validation_status::ACCEPTED;
            if (!selection_drag_content_validation_accepted(content_validation))
            {
                if (m_selection_trace_enabled) {
                    write_selection_trace(m_selection_trace_enabled,
                        QStringLiteral(
                            "surface mouse-release source-mismatch reason=%1 "
                            "range=%2 anchor=%3 current=%4 anchor_snapshot=%5 current_snapshot=%6")
                            .arg(selection_trace_drag_content_validation_status(content_validation))
                            .arg(selection_trace_range(range))
                            .arg(selection_trace_source_identity(
                                *m_private->selection_anchor_source))
                            .arg(selection_trace_source_identity(*source))
                            .arg(selection_trace_snapshot_identity(
                                m_private->selection_anchor_snapshot))
                            .arg(selection_trace_snapshot_identity(m_private->render_snapshot)));
                }
                record_surface_selection_drag_transcript(
                    m_private->transcript_recorder,
                    QStringLiteral("cancel"),
                    m_private->selection_anchor,
                    logical_position,
                    range,
                    m_private->selection_drag_moved);
                cancel_selection_drag_after_content_validation_failure(
                    *m_private->session,
                    content_validation,
                    m_private->selection_drag_moved);
                m_private->clear_selection_drag_state();
                event->accept();
                sync_from_session();
                trace_decision(QStringLiteral("source-mismatch"));
                return;
            }
            record_surface_selection_drag_transcript(
                m_private->transcript_recorder,
                QStringLiteral("finish"),
                m_private->selection_anchor,
                logical_position,
                range,
                selection_range_needed);
            set_selection_range_for_drag(
                *m_private->session,
                *m_private->selection_anchor,
                *logical_position,
                m_private->selection_drag_moved,
                *source);
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
            m_private->session->clear_selection();
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

    viewport_position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    std::optional<term::terminal_grid_position_t> report_position = viewport_position;
    if (!report_position.has_value() && terminal_mouse_grab_active) {
        report_position = m_private->mouse_reporting_last_position.has_value()
            ? m_private->mouse_reporting_last_position
            : clamped_grid_position_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
    }
    viewport_position = report_position;
    if (!report_position.has_value() || button == term::Terminal_mouse_button::NONE) {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        trace_decision(QStringLiteral("out-of-grid"));
        return;
    }

    const term::Terminal_mouse_event_result mouse_result =
        m_private->session->write_mouse_event({
            term::Terminal_mouse_event_kind::RELEASE,
            button,
            report_position->row,
            report_position->column,
            event->modifiers(),
        });
    if (!mouse_result.handled) {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        else {
            m_private->mouse_reporting_pressed_buttons &= ~Qt::MouseButtons(released_qt_button);
            m_private->mouse_reporting_drag_button =
                terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
        }
        const bool non_sgr_mouse_tracking =
            snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
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

    m_private->mouse_reporting_last_position = *report_position;
    if (final_release) {
        m_private->clear_mouse_reporting_state();
    }
    else {
        m_private->mouse_reporting_pressed_buttons &= ~Qt::MouseButtons(released_qt_button);
        m_private->mouse_reporting_drag_button =
            terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
    }
    record_surface_selection_drag_transcript(
        m_private->transcript_recorder,
        QStringLiteral("clear"),
        std::nullopt,
        logical_position,
        std::nullopt,
        false);
    m_private->session->clear_selection();
    event->accept();
    sync_from_session();
    if (!is_accepted(mouse_result.result.code)) {
        report_result_failure(mouse_result.result);
    }
    trace_decision(QStringLiteral("mouse-reporting"));
}

void VNM_TerminalSurface::hoverMoveEvent(QHoverEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

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

    const term::Terminal_mouse_event_result mouse_result =
        m_private->session->write_mouse_event({
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            position->row,
            position->column,
            event->modifiers(),
        });
    if (!mouse_result.handled) {
        if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
        {
            event->accept();
        }
        return;
    }

    event->accept();
    sync_from_session();
    if (!is_accepted(mouse_result.result.code)) {
        report_result_failure(mouse_result.result);
    }
}

void VNM_TerminalSurface::wheelEvent(QWheelEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_wheel_trace_enabled) {
        record_surface_wheel_ingress_transcript(
            m_private->transcript_recorder,
            QStringLiteral("surface.text_area"),
            *event);
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

    int    backend_drain_calls          = 0;
    qint64 backend_drain_elapsed_ns     = 0;
    bool   trace_written                = false;
    bool   local_scroll_attempted       = false;
    bool   local_scroll_possible        = false;
    bool   local_scroll_intent_recorded = false;
    bool   local_scroll_applied         = false;
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

    const auto finish_trace =
        [&](const QString& route, const QString& outcome, bool accepted) {
            if (!m_wheel_trace_enabled || trace_written) {
                return;
            }
            trace_written = true;

            QJsonObject object =
                wheel_trace_event_object(QStringLiteral("surface.text_area"), *event);
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
            object.insert(QStringLiteral("protocol_state_stale"), protocol_state_stale);

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
                render_publication_blocked);
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
                    wheel_trace_scroll_action_name(trace_scroll_result.action));
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
        const int steps = vertical_wheel_steps(
            *event,
            k_angle_delta_per_wheel_step,
            m_private->wheel_zoom_angle_remainder,
            m_private->wheel_zoom_pixel_remainder);
        wheel_steps           = steps;
        trace_angle_remainder = m_private->wheel_zoom_angle_remainder;
        trace_pixel_remainder = m_private->wheel_zoom_pixel_remainder;
        if (steps == 0) {
            if (has_vertical_wheel_delta(*event)) {
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

        const qreal base_font_size = finite_positive_font_size_or_default(m_font_size);
        const qreal requested_font_size = std::clamp(
            base_font_size + static_cast<qreal>(steps) * k_font_zoom_wheel_step,
            k_font_zoom_min_pixel_size,
            k_font_zoom_max_pixel_size);
        const qreal previous_font_size = m_font_size;
        set_font_size(requested_font_size);
        if (has_vertical_wheel_delta(*event) ||
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

    const auto route_wheel_to_application = [&]() -> bool {
        application_route_attempted = true;
        if (!has_vertical_wheel_delta(*event)) {
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

            const int steps = vertical_wheel_steps(
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
                    ? steps * k_plain_scroll_lines_per_angle_step
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

            const int steps = vertical_wheel_steps(
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
        if (!has_vertical_wheel_delta(*event)) {
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
        local_scroll_possible = viewport_state_can_scroll_locally(viewport, scroll_direction);
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

        const int line_delta = vertical_wheel_steps(
            *event,
            m_private->cell_metrics.height,
            m_private->wheel_scroll_angle_remainder,
            m_private->wheel_scroll_pixel_remainder);
        wheel_steps = line_delta;
        trace_angle_remainder = m_private->wheel_scroll_angle_remainder;
        trace_pixel_remainder = m_private->wheel_scroll_pixel_remainder;
        effective_line_delta =
            event->angleDelta().y() != 0
                ? line_delta * k_plain_scroll_lines_per_angle_step
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
            QStringLiteral("wheel"),
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
        if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
            sync_from_session();
            trace_viewport_after = m_private->session->viewport_state();
            has_viewport_after   = true;
            local_scroll_applied = true;
            record_surface_scroll_transcript(
                m_private->transcript_recorder,
                QStringLiteral("wheel"),
                effective_line_delta,
                std::nullopt,
                scroll_result,
                viewport_before,
                trace_viewport_after);
            event->accept();
            const bool visible_scroll_applied =
                !m_private->session->render_publication_blocked();
            finish_trace(
                QStringLiteral("local_scroll"),
                visible_scroll_applied
                    ? QStringLiteral("local_scroll_applied")
                    : QStringLiteral("local_scroll_publication_deferred"),
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

    std::optional<term::Terminal_ime_commit_result> commit_result;
    if (m_private->session != nullptr) {
        if (!commit_text.isEmpty()) {
            commit_result = m_private->session->write_ime_commit(commit_text);
        }

        const bool commit_blocks_empty_preedit_cancel =
            commit_result.has_value()               &&
            commit_result->handled                  &&
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
}

void VNM_TerminalSurface::refresh_grid_metrics()
{
    m_private->render_font               = term::vnm_terminal_font(m_font_family, m_font_size);
    m_private->render_device_pixel_ratio = current_device_pixel_ratio(window());
    m_private->render_light_theme        = is_light_theme(m_color_theme);
    m_private->grid_metrics_provider.set_font(m_private->render_font);
    m_private->grid_metrics_provider.set_device_pixel_ratio(
        m_private->render_device_pixel_ratio);

    if (!std::isfinite(m_font_size) || m_font_size <= 0.0) {
        m_private->cell_metrics = {};
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

    m_scrollback_rows           = scrollback_rows;
    m_viewport_visible_rows     = visible_rows;
    m_viewport_offset_from_tail = offset_from_tail;
    m_viewport_at_tail          = at_tail;
    emit viewport_changed();
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
    emit selection_changed();
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
    QStringList                                argv,
    QString                                    working_directory)
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
    m_private->render_snapshot.reset();
    m_private->set_ime_preedit_state(*this, {});
    m_private->last_render_snapshot_generation    = 0U;
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
    session_config.trace_notification_limit    = k_surface_notification_trace_limit;
    session_config.scrollback_limit            = m_scrollback_limit;
    session_config.backend_output_capture_path = m_backend_output_capture_path;
    session_config.capture_dirty_row_stats     = m_private->dirty_row_stats_enabled;
    session_config.selection_trace_enabled     = m_selection_trace_enabled;
    session_config.transcript_recorder         = m_private->transcript_recorder;
#if defined(Q_OS_WIN)
    session_config.recover_scrollback_from_primary_repaints = true;
#endif
    session_config.bell_policy.audible_enabled =
        m_audible_bell_policy == Bell_policy::ENABLED;
    session_config.bell_policy.visual_enabled =
        m_visual_bell_policy == Bell_policy::ENABLED;
    session_config.backend_event_notifier = [this] {
        queue_backend_callback_drain();
    };

    m_private->session =
        std::make_unique<term::Terminal_session>(std::move(backend), session_config);
    m_private->resize_controller = std::make_unique<term::Terminal_resize_controller>(
        *m_private->session,
        m_private->grid_metrics_provider);

    term::Terminal_launch_config launch_config;
    launch_config.argv              = std::move(argv);
    launch_config.working_directory = std::move(working_directory);

    const term::Terminal_session_result result =
        m_private->resize_controller->start_from_geometry(
            std::move(launch_config),
            boundingRect().size());
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

    const bool queued = QMetaObject::invokeMethod(
        this,
        [this] {
            m_private->session_drain_queued.store(false);
            if (!m_private->shutting_down.load()) {
                drain_backend_callback_events_for_posted_work();
            }
        },
        Qt::QueuedConnection);
    if (!queued) {
        // If the wakeup could not be posted, clear the latch so a later
        // backend callback can retry instead of losing the drain request.
        m_private->session_drain_queued.store(false);
    }
}

void VNM_TerminalSurface::drain_backend_callback_events()
{
    drain_backend_callback_events(false);
}

void VNM_TerminalSurface::drain_backend_callback_events_for_posted_work()
{
    drain_backend_callback_events(true);
}

void VNM_TerminalSurface::drain_backend_callback_events(bool budgeted)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        if (m_selection_trace_enabled) {
            write_selection_trace(m_selection_trace_enabled, QStringLiteral("surface backend-drain reason=no-session"));
        }
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    const bool trace_drain =
        m_selection_trace_enabled &&
        (m_private->selection_drag_active || session->has_selection());
    if (trace_drain) {
        const std::optional<term::terminal_selection_source_identity_t> before_source =
            session->published_selection_source_identity();
        write_selection_trace(m_selection_trace_enabled,
            QStringLiteral("surface backend-drain begin %1 source=%2")
                .arg(selection_trace_snapshot_identity(m_private->render_snapshot))
                .arg(selection_trace_source_identity(before_source)));
    }
    bool drain_complete = true;
    if (budgeted) {
        drain_complete = session->process_backend_callback_events_for(
            k_backend_callback_drain_budget);
    }
    else {
        session->process_backend_callback_events();
    }
    sync_from_session();
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
    if (budgeted &&
        !drain_complete &&
        m_private->session.get() == session &&
        session->has_pending_backend_callback_events())
    {
        queue_backend_callback_drain();
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
    session->process_backend_callback_events();
    sync_from_session();
    if (m_private->session.get() != session ||
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

void VNM_TerminalSurface::sync_from_session()
{
    Q_ASSERT(thread() == QThread::currentThread());
    VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session");

    if (m_private->session == nullptr) {
        return;
    }

    set_process_state(surface_process_state(m_private->session->process_state()));
    set_backend_ready(m_private->session->backend_ready());
    set_backend_geometry_in_sync(m_private->session->backend_geometry_in_sync());
    set_selection_state(
        m_private->session->has_selection()
            ? Selection_state::ACTIVE
            : Selection_state::NONE);

    const term::terminal_grid_size_t grid_size = m_private->session->grid_size();
    set_grid_size(grid_size.rows, grid_size.columns);

    const bool live_mouse_reporting_active = m_private->session->mouse_reporting_active();
    if (live_mouse_reporting_active != m_private->last_sgr_mouse_reporting_active) {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_sgr_mouse_reporting_active = live_mouse_reporting_active;

    const bool live_alternate_scroll_active = m_private->session->alternate_scroll_active();
    const std::uint64_t alternate_scroll_mode_generation =
        m_private->session->alternate_scroll_mode_generation();
    if (alternate_scroll_mode_generation !=
        m_private->last_alternate_scroll_mode_generation)
    {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_alternate_scroll_mode_generation = alternate_scroll_mode_generation;

    if (live_alternate_scroll_active != m_private->last_alternate_scroll_active) {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_alternate_scroll_active = live_alternate_scroll_active;

    const std::uint64_t snapshot_generation =
        m_private->session->render_snapshot_generation();
    if (snapshot_generation != m_private->last_render_snapshot_generation) {
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            *this,
            m_private->session->latest_render_snapshot_handle());
        m_private->session->mark_render_snapshot_synced(snapshot_generation);
        m_private->last_render_snapshot_generation = snapshot_generation;
        if (m_private->render_snapshot != nullptr) {
            set_viewport_state(m_private->render_snapshot->viewport);
        }

        const bool sgr_mouse_reporting_active =
            snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
        const bool mouse_reporting_mode_changed =
            m_private->render_snapshot != nullptr &&
            m_private->render_snapshot->metadata.mouse_reporting_mode_changed;
        if (mouse_reporting_mode_changed ||
            sgr_mouse_reporting_active != live_mouse_reporting_active)
        {
            m_private->clear_mouse_wheel_remainders();
        }
    }

    const std::uint64_t ime_preedit_generation =
        m_private->session->ime_preedit_generation();
    if (ime_preedit_generation != m_private->last_ime_preedit_generation) {
        m_private->set_ime_preedit_state(*this, m_private->session->ime_preedit_state());
        m_private->last_ime_preedit_generation = ime_preedit_generation;
    }

    const std::vector<term::Terminal_session_notification> notifications =
        m_private->session->take_pending_notifications();
    for (const term::Terminal_session_notification& notification : notifications) {
        replay_session_notification(notification);
    }

    sync_synchronized_output_recovery_timer();
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
    const bool drain_complete = session->process_backend_callback_events_for(budget);
    sync_from_session();
    if (m_private->session.get() != session ||
        !session->render_publication_blocked())
    {
        return;
    }

    const term::Terminal_session_result result =
        session->force_release_synchronized_output_without_backend_drain();
    sync_from_session();
    if (!drain_complete &&
        m_private->session.get() == session &&
        session->has_pending_backend_callback_events())
    {
        queue_backend_callback_drain();
    }
    if (m_private->session.get() != session) {
        return;
    }

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
            emit bell_requested();
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
    m_private->resize_controller.reset();
    m_private->session.reset();
    m_private->transcript_recorder.reset();
    m_private->session_drain_queued.store(false);
    m_private->clear_mouse_reporting_state();
    m_private->clear_selection_drag_state();
    m_private->pending_clipboard_write.reset();
    m_private->clear_wheel_remainders();
    m_private->last_sgr_mouse_reporting_active       = false;
    m_private->last_alternate_scroll_active          = false;
    m_private->last_alternate_scroll_mode_generation = 0U;
    set_selection_state(Selection_state::NONE);
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

QSGNode* VNM_TerminalSurface::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*)
{
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
            m_private->renderer_stats_publisher.publish({});
            if (old_node != nullptr) {
                if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                lifecycle_recorder != nullptr)
                {
                    lifecycle_recorder->record_render_node_deleted();
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
            m_private->renderer_stats_publisher.publish({});
            if (old_node != nullptr) {
                if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                lifecycle_recorder != nullptr)
                {
                    lifecycle_recorder->record_render_node_deleted();
                }
                delete old_node;
            }
            if (requeue_render_update) {
                m_private->request_render_update(*this);
            }
            return nullptr;
        }

        const term::Terminal_render_options options = render_options_for_surface(*this);
        const term::Terminal_render_frame frame = term::build_terminal_render_frame(
            m_private->render_snapshot.get(),
            logical_size,
            m_private->cell_metrics,
            options,
            m_private->cursor_blink_visible,
            &m_private->ime_preedit);
        term::terminal_renderer_stats_t renderer_stats;
        QSGNode* updated_node = m_private->renderer.update_node(
            old_node,
            window(),
            frame,
            m_private->render_font,
            device_pixel_ratio,
            m_private->lifecycle_recorder(),
            renderer_stats
#if VNM_TERMINAL_PROFILING_ENABLED
            ,
            m_private->slow_text_layout_recorder_handle()
#endif
        );
        m_private->renderer_stats_publisher.publish(renderer_stats);
        if (updated_node != nullptr && renderer_stats.paint_completed) {
            m_private->consume_render_update(window());
            m_private->render_invalidation_stats.last_rendered_snapshot_sequence =
                m_private->render_snapshot != nullptr
                    ? m_private->render_snapshot->metadata.sequence
                    : 0U;
        }
        else {
            m_private->reset_render_update_schedule();
        }
        return updated_node;
    };

#if VNM_TERMINAL_PROFILING_ENABLED
    const std::shared_ptr<term::Hierarchical_profiler> render_profiler =
        m_private->render_profiler_handle();
    QSGNode* result_node = nullptr;
    {
        term::Active_profiler_binding render_profiler_binding(render_profiler.get());
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::updatePaintNode");
        result_node = update_node();
    }

    m_private->publish_render_profiler_snapshot(
        render_profiler,
        m_private->render_invalidation_stats.last_rendered_snapshot_sequence);
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
    if (surface.m_private->render_update_pending &&
        surface.m_private->render_snapshot != nullptr &&
        snapshot                           != nullptr)
    {
        snapshot = snapshot_with_coalesced_dirty_rows(
            *surface.m_private->render_snapshot,
            *snapshot);
    }
    surface.m_private->render_snapshot = std::move(snapshot);
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
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->dirty_row_stats_enabled = enabled;
    if (surface.m_private->session != nullptr) {
        surface.m_private->session->set_dirty_row_stats_enabled(enabled);
    }
}

void term::VNM_TerminalSurface_render_bridge::set_selection_trace_enabled(
    VNM_TerminalSurface&   surface,
    bool                   enabled)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_selection_trace_enabled = enabled;
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

void term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
    VNM_TerminalSurface&               surface,
    bool                               visible)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->cursor_blink_visible = visible;
    surface.m_private->request_render_update(surface);
}

bool term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
    VNM_TerminalSurface&               surface,
    std::unique_ptr<Terminal_backend>  backend,
    QStringList                        argv,
    QString                            working_directory)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.start_process_with_backend(
        std::move(backend),
        std::move(argv),
        std::move(working_directory));
}

bool term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session_drain_queued.load();
}

void term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.drain_backend_callback_events();
}

void term::VNM_TerminalSurface_render_bridge::handle_synchronized_output_recovery_timeout(
    VNM_TerminalSurface&                    surface,
    std::chrono::steady_clock::duration     budget)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.handle_synchronized_output_recovery_timeout(budget);
}

std::shared_ptr<const term::Terminal_render_snapshot>
term::VNM_TerminalSurface_render_bridge::render_snapshot(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->render_snapshot;
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
    return surface.m_private->current_invalidation_stats();
}

term::terminal_renderer_stats_t
term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->renderer_stats_publisher.snapshot();
}

term::terminal_renderer_cumulative_stats_t
term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->renderer_stats_publisher.cumulative_snapshot();
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
