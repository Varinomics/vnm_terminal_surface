#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/terminal_touch_selection.h"
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <functional>
#include <optional>

class QQuickItem;
class QTouchEvent;

namespace vnm_terminal::internal {

struct Terminal_touch_presentation
{
    Terminal_canvas_selection selection;
    terminal_cell_metrics_t cell_metrics;
    QRectF block_cursor;
};

QRectF terminal_touch_handle_rect(
    terminal_canvas_selection_endpoint_t endpoint,
    terminal_cell_metrics_t metrics,
    QSizeF content_size);

class Terminal_touch_controller : public QObject
{
public:
    struct Callbacks
    {
        std::function<std::optional<Terminal_touch_presentation>()> presentation;
        std::function<void(const Terminal_touch_selection_request&)> dispatch;
        std::function<void()> tapped;
        std::function<void(Terminal_touch_menu, QRectF, std::uint64_t)> menu_requested;
        std::function<void()> menu_dismissed;
        std::function<void(QString)> notice;
        std::function<bool()> paste_available = [] { return true; };
    };

    Terminal_touch_controller(QQuickItem& item, Callbacks callbacks, bool enabled);
    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled);
    QRectF viewport() const;
    void set_viewport(QRectF viewport);
    void touch_event(QTouchEvent& event);
    void touch_ungrabbed();
    void cancel();
    void presentation_changed();
    bool selection_active() const;
    void activate_tap();
    bool dismiss_selection(std::uint64_t generation = 0U);
    std::uint64_t request_copy(std::uint64_t generation);
    void request_padding_menu(QPointF position);
    void complete(const Terminal_touch_selection_result& result);

private:
    enum class Gesture { NONE, PENDING, SELECTING, PASTE, HANDLE, DISMISSING, INSPECTING, CONTEXT };
    void prolonged_press();
    void extend_handle();
    void send(Terminal_touch_selection_action action, int scroll_lines = 0,
        const terminal_selection_point_t* pinned_target = nullptr);
    void finish_release();
    void show_menu_if_presented();
    void dismiss_menu();
    void show_paste_menu();
    bool press_near_block_cursor(const Terminal_touch_presentation& presentation) const;
    bool word_target_is_current() const;
    void report_notice(QString reason);
    QPointF position() const;
    terminal_selection_point_t point_for_position(const Terminal_touch_presentation& presentation) const;

    QQuickItem& m_item;
    Callbacks m_callbacks;
    QTimer m_hold_timer;
    QTimer m_edge_timer;
    QTimer m_request_timer;
    bool m_enabled = false;
    QRectF m_viewport;
    Gesture m_gesture = Gesture::NONE;
    int m_point_id = -1;
    QPointF m_press_scene_position;
    QPointF m_scene_position;
    QPointF m_handle_offset;
    QString m_gesture_id;
    Terminal_selection_handle m_handle = Terminal_selection_handle::END;
    std::uint64_t m_next_request_id = 1U;
    std::uint64_t m_inflight_request = 0U;
    std::uint64_t m_generation = 0U;
    std::optional<terminal_selection_source_t> m_source;
    std::optional<Terminal_touch_selection_request> m_word_target;
    bool m_paste_menu = false;
    bool m_selecting_inspected_word = false;
    bool m_released = false;
    bool m_release_move_sent = false;
    bool m_move_pending = false;
    bool m_menu_pending = false;
    bool m_menu_visible = false;
    bool m_press_dismisses_selection = false;
};

} // namespace vnm_terminal::internal
