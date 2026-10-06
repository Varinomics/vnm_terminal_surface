#include "vnm_terminal/internal/terminal_touch_controller.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QStyleHints>
#include <QTouchEvent>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <utility>

namespace vnm_terminal::internal {

QRectF terminal_touch_handle_rect(
    terminal_canvas_selection_endpoint_t endpoint,
    terminal_cell_metrics_t metrics,
    QSizeF content_size)
{
    const qreal diameter = std::clamp(metrics.height * 0.7, 14.0, 22.0);
    const qreal x = std::clamp(endpoint.column * metrics.width - diameter / 2.0,
        0.0, std::max(0.0, content_size.width() - diameter));
    const qreal y = std::clamp((endpoint.row + 1) * metrics.height,
        0.0, std::max(0.0, content_size.height() - diameter));
    return {x, y, diameter, diameter};
}

Terminal_touch_controller::Terminal_touch_controller(
    QQuickItem& item, Callbacks callbacks, bool enabled)
:
    QObject(&item),
    m_item(item),
    m_callbacks(std::move(callbacks)),
    m_enabled(enabled)
{
    m_hold_timer.setSingleShot(true);
    m_hold_timer.setInterval(QGuiApplication::styleHints()->mousePressAndHoldInterval());
    m_edge_timer.setInterval(100);
    m_request_timer.setSingleShot(true);
    m_request_timer.setInterval(5000);
    connect(&m_hold_timer, &QTimer::timeout, this, &Terminal_touch_controller::prolonged_press);
    connect(&m_edge_timer, &QTimer::timeout, this, &Terminal_touch_controller::extend_handle);
    connect(&m_request_timer, &QTimer::timeout, this, [this] {
        const bool word_request = m_word_target.has_value() || m_selecting_inspected_word;
        cancel();
        if (word_request) {
            report_notice(QStringLiteral("The terminal did not confirm the pressed text."));
        }
    });
}

void Terminal_touch_controller::set_enabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    cancel();
    m_enabled = enabled;
}

QRectF Terminal_touch_controller::viewport() const
{
    return m_viewport.isEmpty() ? m_item.boundingRect() : m_viewport;
}

void Terminal_touch_controller::set_viewport(QRectF viewport)
{
    if (m_viewport == viewport) {
        return;
    }
    m_viewport = viewport;
}

QPointF Terminal_touch_controller::position() const
{
    return m_item.mapFromScene(m_scene_position);
}

void Terminal_touch_controller::dismiss_menu()
{
    m_menu_pending = false;
    m_word_target.reset();
    m_paste_menu = false;
    if (m_menu_visible) {
        m_menu_visible = false;
        m_callbacks.menu_dismissed();
    }
}

void Terminal_touch_controller::cancel()
{
    const bool owner_gesture = m_gesture != Gesture::DISMISSING && !m_gesture_id.isEmpty() && m_source &&
        (m_gesture == Gesture::SELECTING || m_gesture == Gesture::HANDLE);
    Terminal_touch_selection_request cancellation;
    if (owner_gesture) {
        cancellation.request_id = m_next_request_id++;
        cancellation.gesture_id = m_gesture_id;
        cancellation.action = Terminal_touch_selection_action::CANCEL_GESTURE;
        cancellation.source = *m_source;
        cancellation.expected_selection_generation = m_generation;
    }
    m_hold_timer.stop();
    m_edge_timer.stop();
    m_request_timer.stop();
    m_inflight_request = 0U;
    m_point_id = -1;
    m_released = false;
    m_move_pending = false;
    m_gesture = Gesture::NONE;
    m_selecting_inspected_word = false;
    m_source.reset();
    m_gesture_id.clear();
    dismiss_menu();
    m_item.setKeepTouchGrab(false);
    if (owner_gesture) {
        // Cancellation is ordered after the admitted gesture, but its reply
        // must never prevent a new touch after a transport or grab loss.
        m_callbacks.dispatch(cancellation);
    }
}

bool Terminal_touch_controller::selection_active() const
{
    const auto presentation = m_callbacks.presentation();
    return m_gesture == Gesture::DISMISSING || m_gesture == Gesture::SELECTING ||
        m_gesture == Gesture::HANDLE || m_gesture == Gesture::INSPECTING ||
        m_gesture == Gesture::CONTEXT || m_menu_visible || m_menu_pending ||
        (presentation && presentation->selection.has_selection);
}

void Terminal_touch_controller::activate_tap()
{
    if (!dismiss_selection()) {
        m_callbacks.tapped();
    }
}

bool Terminal_touch_controller::dismiss_selection(std::uint64_t generation)
{
    const auto presentation = m_callbacks.presentation();
    if (generation != 0U &&
        (!presentation || presentation->selection.selection_generation != generation ||
            m_inflight_request != 0U || m_point_id >= 0))
    {
        return false;
    }
    if (m_gesture == Gesture::DISMISSING) {
        return true;
    }
    const bool selecting = m_gesture == Gesture::SELECTING || m_gesture == Gesture::HANDLE;
    if (!selecting && (!presentation || !presentation->selection.has_selection)) {
        const bool menu_active = m_menu_visible || m_menu_pending ||
            m_gesture == Gesture::INSPECTING || m_gesture == Gesture::CONTEXT;
        cancel();
        return menu_active;
    }
    m_hold_timer.stop();
    m_edge_timer.stop();
    dismiss_menu();
    m_point_id = -1;
    m_item.setKeepTouchGrab(false);
    m_gesture = Gesture::DISMISSING;
    m_move_pending = false;
    if (m_inflight_request == 0U) {
        if (presentation) {
            m_source = presentation->selection.source;
            m_generation = presentation->selection.selection_generation;
        }
        m_gesture_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        send(Terminal_touch_selection_action::CLEAR);
    }
    // A pending selection reply supplies the generation to clear. Retaining
    // its correlation prevents a late END from reopening the editing menu.
    return true;
}

void Terminal_touch_controller::touch_ungrabbed()
{
    // Qt releases the exclusive grab after delivering TouchEnd as well as
    // when another gesture takes it. Only loss of an unreleased point cancels.
    if (m_point_id >= 0) {
        cancel();
    }
}

void Terminal_touch_controller::touch_event(QTouchEvent& event)
{
    if (!m_enabled) {
        event.ignore();
        return;
    }
    if (event.type() == QEvent::TouchCancel) {
        cancel();
        m_point_id = -1;
        event.accept();
        return;
    }
    if (event.points().size() != 1) {
        cancel();
        if (event.type() == QEvent::TouchEnd) {
            m_point_id = -1;
            m_gesture = Gesture::NONE;
        }
        event.ignore();
        return;
    }
    const QEventPoint& point = event.points().front();
    if (m_gesture == Gesture::DISMISSING) {
        event.accept();
        return;
    }
    if (point.state() == QEventPoint::State::Pressed) {
        if (m_point_id >= 0) {
            cancel();
            event.accept();
            return;
        }
        if (m_inflight_request != 0U) {
            dismiss_selection();
            event.accept();
            return;
        }
        const auto presentation = m_callbacks.presentation();
        m_press_dismisses_selection = selection_active();
        dismiss_menu();
        m_point_id             = point.id();
        m_press_scene_position = point.scenePosition();
        m_scene_position       = point.scenePosition();
        m_handle_offset        = {};
        m_gesture_id           = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_released             = false;
        m_release_move_sent = false;
        m_gesture       = Gesture::PENDING;
        m_source.reset();
        if (presentation) {
            m_source     = presentation->selection.source;
            m_generation = presentation->selection.selection_generation;
            const auto& selection = presentation->selection;
            if (selection.touch_handles_visible) {
                for (const auto handle : {Terminal_selection_handle::START, Terminal_selection_handle::END}) {
                    const auto endpoint = handle == Terminal_selection_handle::START
                        ? selection.start : selection.end;
                    const QRectF rect = terminal_touch_handle_rect(
                        endpoint, presentation->cell_metrics,
                        QSizeF(selection.source.columns * presentation->cell_metrics.width,
                            selection.source.rows * presentation->cell_metrics.height));
                    if (endpoint.visible && rect.adjusted(-10.0, -10.0, 10.0, 10.0).contains(position())) {
                        m_handle = handle;
                        m_handle_offset = position() - QPointF(
                            endpoint.column * presentation->cell_metrics.width,
                            (endpoint.row + 1) * presentation->cell_metrics.height);
                        m_gesture = Gesture::HANDLE;
                        m_item.grabTouchPoints({m_point_id});
                        m_item.setKeepTouchGrab(true);
                        send(Terminal_touch_selection_action::BEGIN_HANDLE);
                        break;
                    }
                }
            }
        }
        if (m_gesture == Gesture::PENDING) {
            m_hold_timer.setInterval(QGuiApplication::styleHints()->mousePressAndHoldInterval());
            m_hold_timer.start();
        }
    }
    else
    if (point.id() == m_point_id) {
        m_scene_position = point.scenePosition();
        if (point.state() == QEventPoint::State::Released) {
            m_hold_timer.stop();
            m_edge_timer.stop();
            m_released = true;
            m_move_pending = false;
            m_point_id = -1;
            m_item.setKeepTouchGrab(false);
            if (m_gesture == Gesture::PENDING) {
                m_gesture = Gesture::NONE;
                if (m_press_dismisses_selection) {
                    dismiss_selection();
                }
                else {
                    activate_tap();
                }
            }
            else
            if (m_gesture == Gesture::PASTE) {
                show_paste_menu();
            }
            else
            if (m_gesture == Gesture::INSPECTING) {
                // The owner reply resolves this already-qualified hold.
            }
            else
            if (m_gesture == Gesture::CONTEXT) {
                const auto presentation = m_callbacks.presentation();
                if (presentation && presentation->selection.has_selection) {
                    m_generation = presentation->selection.selection_generation;
                    m_gesture = Gesture::NONE;
                    m_menu_pending = true;
                    show_menu_if_presented();
                }
                else {
                    cancel();
                }
            }
            else
            if (m_gesture == Gesture::SELECTING || m_gesture == Gesture::HANDLE) {
                finish_release();
            }
            else {
                m_gesture = Gesture::NONE;
            }
        }
        else
        if (m_gesture == Gesture::PENDING &&
            (m_scene_position - m_press_scene_position).manhattanLength() > QGuiApplication::styleHints()->startDragDistance())
        {
            cancel();
        }
        else
        if (m_gesture == Gesture::HANDLE) {
            extend_handle();
        }
    }
    event.accept();
}

void Terminal_touch_controller::prolonged_press()
{
    if (m_gesture != Gesture::PENDING) {
        return;
    }
    m_item.grabTouchPoints({m_point_id});
    m_item.setKeepTouchGrab(true);
    const auto presentation = m_callbacks.presentation();
    if (!presentation) {
        m_gesture = Gesture::PASTE;
        return;
    }
    if (presentation->selection.word_query_version == k_terminal_word_query_version) {
        m_generation = presentation->selection.selection_generation;
        if (presentation->selection.has_selection) {
            m_gesture = Gesture::CONTEXT;
        }
        else if (m_callbacks.paste_available() && press_near_block_cursor(*presentation)) {
            m_gesture = Gesture::PASTE;
        }
        else {
            m_gesture = Gesture::INSPECTING;
            send(Terminal_touch_selection_action::INSPECT_WORD);
        }
        return;
    }
    m_gesture = Gesture::SELECTING;
    send(Terminal_touch_selection_action::SELECT_WORD);
}

terminal_selection_point_t Terminal_touch_controller::point_for_position(
    const Terminal_touch_presentation& presentation) const
{
    const QPointF local_position = position() - m_handle_offset;
    const qreal row_position = local_position.y() -
        (m_gesture == Gesture::HANDLE ? presentation.cell_metrics.height * 0.5 : 0.0);
    const int row = std::clamp((int)std::floor(row_position / presentation.cell_metrics.height),
        0, presentation.selection.source.rows - 1);
    const int column = std::clamp((int)std::round(local_position.x() / presentation.cell_metrics.width),
        0, presentation.selection.source.columns);
    return {presentation.selection.visible_rows[(std::size_t)row], column};
}

void Terminal_touch_controller::send(Terminal_touch_selection_action action, int scroll_lines,
    const terminal_selection_point_t* pinned_target)
{
    const auto presentation = m_callbacks.presentation();
    const bool target_required = action == Terminal_touch_selection_action::SELECT_WORD ||
        action == Terminal_touch_selection_action::INSPECT_WORD ||
        action == Terminal_touch_selection_action::BEGIN_HANDLE ||
        action == Terminal_touch_selection_action::MOVE_HANDLE ||
        action == Terminal_touch_selection_action::SCROLL_EXTEND;
    if (target_required && (!presentation || presentation->selection.visible_rows.size() !=
            (std::size_t)presentation->selection.source.rows))
    {
        if (action == Terminal_touch_selection_action::MOVE_HANDLE ||
            action == Terminal_touch_selection_action::SCROLL_EXTEND)
        {
            m_move_pending = true;
            m_release_move_sent = false;
        }
        else {
            cancel();
        }
        return;
    }
    if (!presentation && !m_source) {
        cancel();
        return;
    }
    Terminal_touch_selection_request request;
    request.request_id = m_next_request_id++;
    request.gesture_id = m_gesture_id;
    request.action     = action;
    request.source     = m_source ? *m_source : presentation->selection.source;
    if (target_required) {
        request.target = pinned_target ? *pinned_target : point_for_position(*presentation);
    }
    request.handle     = m_handle;
    request.expected_selection_generation = m_generation;
    request.scroll_lines = scroll_lines;
    if (!pinned_target && (action == Terminal_touch_selection_action::SELECT_WORD ||
        action == Terminal_touch_selection_action::INSPECT_WORD))
    {
        request.target.column = std::clamp(
            (int)std::floor(position().x() / presentation->cell_metrics.width),
            0, request.source.columns - 1);
    }
    if (action == Terminal_touch_selection_action::INSPECT_WORD) {
        m_word_target = request;
    }
    m_inflight_request = request.request_id;
    m_request_timer.start();
    m_callbacks.dispatch(request);
}

void Terminal_touch_controller::extend_handle()
{
    if (m_gesture != Gesture::HANDLE || m_released) {
        return;
    }
    if (m_inflight_request != 0U) {
        m_move_pending = true;
        return;
    }
    const auto presentation = m_callbacks.presentation();
    if (!presentation) {
        m_move_pending = true;
        return;
    }
    const qreal edge = std::max(20.0, presentation->cell_metrics.height);
    const QRectF visible = viewport();
    const int scroll = position().y() < visible.top() + edge ? 3
        : position().y() > visible.bottom() - edge ? -3 : 0;
    if (scroll != 0) {
        m_edge_timer.start();
        send(Terminal_touch_selection_action::SCROLL_EXTEND, scroll);
    }
    else {
        m_edge_timer.stop();
        send(Terminal_touch_selection_action::MOVE_HANDLE);
    }
}

void Terminal_touch_controller::finish_release()
{
    if (!m_released || (m_gesture != Gesture::SELECTING && m_gesture != Gesture::HANDLE)) {
        return;
    }
    if (m_inflight_request == 0U) {
        if (m_gesture == Gesture::HANDLE && !m_release_move_sent) {
            m_release_move_sent = true;
            send(Terminal_touch_selection_action::MOVE_HANDLE);
        }
        else {
            send(Terminal_touch_selection_action::END_GESTURE);
        }
    }
}

void Terminal_touch_controller::complete(const Terminal_touch_selection_result& result)
{
    if (result.action == Terminal_touch_selection_action::COPY ||
        result.request_id != m_inflight_request)
    {
        return;
    }
    m_inflight_request = 0U;
    m_request_timer.stop();
    if (result.status != Terminal_touch_selection_status::OK) {
        const bool word_request = m_word_target.has_value() || m_selecting_inspected_word;
        cancel();
        if (word_request) {
            report_notice(result.reason.isEmpty()
                ? QStringLiteral("The pressed terminal text is no longer available.") : result.reason);
        }
        return;
    }
    m_generation = result.selection_generation;
    if (result.action == Terminal_touch_selection_action::INSPECT_WORD) {
        if (!result.word_available || !word_target_is_current()) {
            cancel();
            report_notice(QStringLiteral("The pressed terminal text changed before it could be selected."));
            return;
        }
        if (*result.word_available) {
            const auto target = m_word_target->target;
            // The owner's selected frame may arrive before its reply. It must
            // not invalidate the inspection proof that admitted this request.
            m_word_target.reset();
            m_gesture = Gesture::SELECTING;
            m_selecting_inspected_word = true;
            send(Terminal_touch_selection_action::SELECT_WORD, 0, &target);
        }
        else {
            m_word_target.reset();
            m_gesture = Gesture::PASTE;
            if (m_released) {
                show_paste_menu();
            }
        }
        return;
    }
    if (m_gesture == Gesture::DISMISSING) {
        if (result.action == Terminal_touch_selection_action::CLEAR) {
            m_request_timer.start();
            presentation_changed();
        }
        else {
            send(Terminal_touch_selection_action::CLEAR);
        }
        return;
    }
    if (result.action == Terminal_touch_selection_action::END_GESTURE) {
        m_gesture = Gesture::NONE;
        m_released = false;
        m_move_pending = false;
        m_release_move_sent = false;
        m_gesture_id.clear();
        m_word_target.reset();
        m_selecting_inspected_word = false;
        m_menu_pending = true;
        show_menu_if_presented();
    }
    else
    if (result.action != Terminal_touch_selection_action::CANCEL_GESTURE) {
        if (m_gesture == Gesture::NONE) {
            send(Terminal_touch_selection_action::CANCEL_GESTURE);
        }
        else
        if (m_released) {
            finish_release();
        }
        else
        if (m_move_pending) {
            m_move_pending = false;
            extend_handle();
        }
    }
}

void Terminal_touch_controller::show_menu_if_presented()
{
    if (!m_menu_pending) {
        return;
    }
    const auto presentation = m_callbacks.presentation();
    if (!presentation || presentation->selection.selection_generation != m_generation ||
        !presentation->selection.has_selection)
    {
        return;
    }
    QRectF anchor;
    for (const auto& span : presentation->selection.spans) {
        const QRectF rect(span.first_column * presentation->cell_metrics.width,
            span.row * presentation->cell_metrics.height,
            span.column_count * presentation->cell_metrics.width, presentation->cell_metrics.height);
        anchor = anchor.isEmpty() ? rect : anchor.united(rect);
    }
    anchor = anchor.intersected(viewport());
    if (anchor.isEmpty()) {
        m_menu_pending = false;
        return;
    }
    m_menu_pending = false;
    m_menu_visible = true;
    m_paste_menu = false;
    m_callbacks.menu_requested(
        presentation->selection.word_query_version == k_terminal_word_query_version
            ? Terminal_touch_menu::COPY : Terminal_touch_menu::LEGACY_COPY,
        anchor, m_generation);
}

void Terminal_touch_controller::report_notice(QString reason)
{
    if (m_callbacks.notice) {
        m_callbacks.notice(std::move(reason));
    }
}

bool Terminal_touch_controller::word_target_is_current() const
{
    const auto presentation = m_callbacks.presentation();
    return m_word_target && presentation &&
        presentation->selection.source == m_word_target->source &&
        presentation->selection.selection_generation == m_generation &&
        std::find(presentation->selection.visible_rows.begin(), presentation->selection.visible_rows.end(),
            m_word_target->target.row) != presentation->selection.visible_rows.end();
}

bool Terminal_touch_controller::press_near_block_cursor(const Terminal_touch_presentation& presentation) const
{
    if (presentation.block_cursor.isEmpty()) {
        return false;
    }
    const QRectF cursor = m_item.mapRectToScene(presentation.block_cursor);
    const QRectF visible = m_item.mapRectToScene(viewport());
    if (!cursor.intersects(visible)) {
        return false;
    }
    const qreal margin_x = std::min(6.0, cursor.width() * 0.25);
    const qreal margin_y = std::min(6.0, cursor.height() * 0.25);
    return cursor.adjusted(-margin_x, -margin_y, margin_x, margin_y)
        .intersected(visible).contains(m_scene_position);
}

void Terminal_touch_controller::show_paste_menu()
{
    const auto presentation = m_callbacks.presentation();
    if (presentation && presentation->selection.has_selection) {
        cancel();
        return;
    }
    m_gesture = Gesture::NONE;
    m_menu_visible = true;
    m_paste_menu = true;
    m_callbacks.menu_requested(Terminal_touch_menu::PASTE,
        QRectF(position() - QPointF(1.0, 1.0), QSizeF(2.0, 2.0)), m_generation);
}

void Terminal_touch_controller::request_padding_menu(QPointF local_position)
{
    cancel();
    m_scene_position = m_item.mapToScene(local_position);
    const auto presentation = m_callbacks.presentation();
    if (presentation && presentation->selection.has_selection) {
        m_generation = presentation->selection.selection_generation;
        m_menu_pending = true;
        show_menu_if_presented();
        return;
    }
    show_paste_menu();
}

void Terminal_touch_controller::presentation_changed()
{
    const auto presentation = m_callbacks.presentation();
    if (presentation && m_source && presentation->selection.source != *m_source) {
        const bool word_menu = m_word_target.has_value();
        cancel();
        if (word_menu) {
            report_notice(QStringLiteral("The pressed terminal text is no longer available."));
        }
        return;
    }
    if (m_word_target && !word_target_is_current()) {
        cancel();
        report_notice(QStringLiteral("The pressed terminal text is no longer available."));
        return;
    }
    if (m_gesture == Gesture::DISMISSING) {
        if (presentation && m_inflight_request == 0U &&
            presentation->selection.selection_generation >= m_generation)
        {
            cancel();
        }
        return;
    }
    if (presentation && m_menu_visible && !m_paste_menu &&
        (presentation->selection.selection_generation != m_generation || !presentation->selection.has_selection))
    {
        dismiss_menu();
    }
    if (presentation && m_menu_visible && m_paste_menu &&
        presentation->selection.has_selection &&
        presentation->selection.word_query_version == k_terminal_word_query_version)
    {
        dismiss_menu();
    }
    show_menu_if_presented();
    if (presentation && m_move_pending && m_inflight_request == 0U) {
        m_move_pending = false;
        if (m_released) {
            finish_release();
        }
        else {
            extend_handle();
        }
    }
}

std::uint64_t Terminal_touch_controller::request_copy(std::uint64_t generation)
{
    const auto presentation = m_callbacks.presentation();
    if (!presentation || presentation->selection.selection_generation != generation) {
        return 0U;
    }
    Terminal_touch_selection_request request;
    request.request_id = m_next_request_id++;
    request.gesture_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.action     = Terminal_touch_selection_action::COPY;
    request.source     = presentation->selection.source;
    request.expected_selection_generation = generation;
    // Copy callers correlate completion with the returned id. Even a direct
    // owner adapter must not complete before that id reaches its caller.
    QMetaObject::invokeMethod(this, [this, request] { m_callbacks.dispatch(request); }, Qt::QueuedConnection);
    return request.request_id;
}

} // namespace vnm_terminal::internal
