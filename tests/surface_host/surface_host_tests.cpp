#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QTemporaryDir>
#include <QVariant>
#include <QWheelEvent>
#include <QtGui/private/qwindow_p.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

class Scoped_env_var
{
public:
    Scoped_env_var(const char* name, std::optional<QByteArray> value)
    :
        m_name(name),
        m_was_set(qEnvironmentVariableIsSet(name)),
        m_previous_value(qgetenv(name))
    {
        if (value.has_value()) {
            qputenv(m_name.constData(), *value);
        }
        else {
            qunsetenv(m_name.constData());
        }
    }

    ~Scoped_env_var()
    {
        if (m_was_set) {
            qputenv(m_name.constData(), m_previous_value);
        }
        else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    bool       m_was_set = false;
    QByteArray m_previous_value;
};

bool check_bytes_equal(const QByteArray& actual, const QByteArray& expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected.toHex(' ').constData()
            << " actual="   << actual.toHex(' ').constData() << '\n';
        return false;
    }

    return true;
}

bool check_int_equal(int actual, int expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected
            << " actual="   << actual << '\n';
        return false;
    }

    return true;
}

bool check_uint64_equal(std::uint64_t actual, std::uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected
            << " actual="   << actual << '\n';
        return false;
    }

    return true;
}

bool check_write_chunks_equal(
    const std::vector<QByteArray>& writes,
    std::size_t                    first_index,
    const std::vector<QByteArray>& expected,
    const char*                    message)
{
    if (writes.size() - std::min(writes.size(), first_index) != expected.size()) {
        std::cerr << "FAIL: " << message
            << " expected chunk count=" << expected.size()
            << " actual chunk count="   << writes.size() - std::min(writes.size(), first_index)
            << '\n';
        return false;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (writes[first_index + i] != expected[i]) {
            std::cerr << "FAIL: " << message
                << " chunk="    << i
                << " expected=" << expected[i].toHex(' ').constData()
                << " actual="   << writes[first_index + i].toHex(' ').constData()
                << '\n';
            return false;
        }
    }

    return true;
}

std::optional<term::Terminal_transcript_event> first_transcript_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == kind) {
            return event;
        }
    }

    return std::nullopt;
}

std::optional<term::Terminal_transcript_event> last_transcript_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->kind == kind) {
            return *it;
        }
    }

    return std::nullopt;
}

bool transcript_has_event_after(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind,
    const term::Terminal_transcript_event&              marker)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == kind && event.event_index > marker.event_index) {
            return true;
        }
    }

    return false;
}

bool transcript_has_source_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind,
    const QString&                                      source)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == kind &&
            event.object.value(QStringLiteral("source")).toString() == source)
        {
            return true;
        }
    }

    return false;
}

bool check_route(
    bool             condition,
    const QString&   route,
    const QByteArray& suffix)
{
    const QByteArray message = route.toUtf8() + suffix;
    return check(condition, message.constData());
}

struct Frozen_public_viewport_state
{
    int  scrollback_rows       = 0;
    int  visible_rows          = 0;
    int  offset_from_tail      = 0;
    bool at_tail               = true;
    int  viewport_change_count = 0;
};

Frozen_public_viewport_state frozen_public_viewport_state(
    const VNM_TerminalSurface& surface,
    int                        viewport_change_count)
{
    return {
        surface.scrollback_rows(),
        surface.viewport_visible_rows(),
        surface.viewport_offset_from_tail(),
        surface.viewport_at_tail(),
        viewport_change_count,
    };
}

bool check_public_viewport_frozen(
    const VNM_TerminalSurface&          surface,
    const Frozen_public_viewport_state& frozen,
    int                                 viewport_change_count,
    const QString&                      route)
{
    bool ok = true;
    ok &= check_route(
        surface.scrollback_rows() == frozen.scrollback_rows,
        route,
        " keeps public scrollback frozen");
    ok &= check_route(
        surface.viewport_visible_rows() == frozen.visible_rows,
        route,
        " keeps public visible rows frozen");
    ok &= check_route(
        surface.viewport_offset_from_tail() == frozen.offset_from_tail,
        route,
        " keeps public offset frozen");
    ok &= check_route(
        surface.viewport_at_tail() == frozen.at_tail,
        route,
        " keeps public at-tail flag frozen");
    ok &= check_route(
        viewport_change_count == frozen.viewport_change_count,
        route,
        " emits no viewport_changed while public projection is invalidated");
    return ok;
}

bool nearly_equal(qreal lhs, qreal rhs, qreal tolerance = 0.01)
{
    return std::abs(lhs - rhs) <= tolerance;
}

bool check_rect_near(
    const QRectF&  actual,
    const QRectF&  expected,
    const char*    message)
{
    if (!nearly_equal(actual.left(),   expected.left())  ||
        !nearly_equal(actual.top(),    expected.top())   ||
        !nearly_equal(actual.width(),  expected.width()) ||
        !nearly_equal(actual.height(), expected.height()))
    {
        std::cerr << "FAIL: " << message
            << " expected=(" << expected.left() << ',' << expected.top()
            << ',' << expected.width() << ',' << expected.height()
            << ") actual=(" << actual.left() << ',' << actual.top()
            << ',' << actual.width() << ',' << actual.height() << ")\n";
        return false;
    }

    return true;
}

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

QByteArray osc52_write_sequence(const char* target_selection, const QByteArray& payload)
{
    QByteArray bytes = QByteArrayLiteral("\x1b]52;");
    bytes += target_selection;
    bytes += ';';
    bytes += payload.toBase64();
    bytes += '\a';
    return bytes;
}

struct Clipboard_write_observation
{
    quint64    request_id = 0U;
    QString    target_selection;
    QByteArray payload;
};

class Clipboard_text_guard
{
public:
    Clipboard_text_guard()
    :
        m_clipboard(QGuiApplication::clipboard()),
        m_original_text(m_clipboard->text(QClipboard::Clipboard))
    {}

    ~Clipboard_text_guard()
    {
        m_clipboard->setText(m_original_text, QClipboard::Clipboard);
    }

private:
    QClipboard*    m_clipboard = nullptr;
    QString        m_original_text;
};

void observe_backend_errors(VNM_TerminalSurface& surface, int& error_count)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::backend_error,
        &surface,
        [&error_count](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++error_count;
        });
}

void observe_backend_error_codes(
    VNM_TerminalSurface&                                   surface,
    std::vector<VNM_TerminalSurface::Backend_error_code>&  error_codes)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::backend_error,
        &surface,
        [&error_codes](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            error_codes.push_back(code);
        });
}

void observe_clipboard_write_requests(
    VNM_TerminalSurface&                                   surface,
    std::vector<Clipboard_write_observation>&              requests)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::clipboard_write_requested,
        &surface,
        [&requests](
            quint64 request_id,
            const QString& target_selection,
            const QByteArray& payload) {
            requests.push_back({request_id, target_selection, payload});
        });
}

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

template <typename Predicate>
bool pump_until(QGuiApplication& app, Predicate predicate, int rounds = 20)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }

    return predicate();
}

// Keep the event loop alive for a wall-clock duration. Negative checks on the
// hover-idle tooltip timer need real elapsed time, not a round count, because
// processEvents returns immediately when the queue is empty.
void pump_for(QGuiApplication& app, int duration_ms)
{
    const qint64 deadline_ms = QDateTime::currentMSecsSinceEpoch() + duration_ms;
    while (QDateTime::currentMSecsSinceEpoch() < deadline_ms) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

bool window_render_matches(
    QGuiApplication&               app,
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    const std::function<bool()>&   predicate)
{
    for (int i = 0; i < 30; ++i) {
        surface.update();
        window.requestUpdate();
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        if (!image.isNull() && predicate()) {
            return true;
        }
    }

    return false;
}

std::optional<term::Qsg_atlas_frame_report> capture_next_surface_frame(
    QGuiApplication&       app,
    QQuickWindow&          window,
    VNM_TerminalSurface&   surface,
    std::uint64_t          previous_capture_count)
{
    for (int i = 0; i < 30; ++i) {
        surface.update();
        window.requestUpdate();
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        const term::Qsg_atlas_frame_report atlas_report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (!image.isNull() &&
            atlas_report.capture_count > previous_capture_count)
        {
            return atlas_report;
        }
    }

    return std::nullopt;
}

struct Surface_frame_attempt
{
    bool                         valid = false;
    term::Qsg_atlas_frame_report report;
};

Surface_frame_attempt capture_surface_frame_attempt(
    QGuiApplication&       app,
    QQuickWindow&          window,
    VNM_TerminalSurface&   surface)
{
    surface.update();
    window.requestUpdate();
    pump_events(app, 1);

    const QImage image = window.grabWindow();
    Surface_frame_attempt attempt;
    attempt.valid = !image.isNull();
    attempt.report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    return attempt;
}

bool capture_surface_sequence(
    QGuiApplication&               app,
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    std::uint64_t                  sequence)
{
    return window_render_matches(app, window, surface, [&] {
        const term::Qsg_atlas_frame_report atlas_report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        return
            atlas_report.capture_count > 0U                     &&
            atlas_report.captured_snapshot_sequence == sequence;
    });
}

std::uint64_t live_root_node_count(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_root_nodes_created - stats.render_root_nodes_destroyed;
}

std::uint64_t live_text_resource_count(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_text_resources_created - stats.render_text_resources_destroyed;
}

bool has_valid_lifecycle_resource_counts(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        stats.render_root_nodes_created     >= stats.render_root_nodes_destroyed &&
        stats.render_text_resources_created >= stats.render_text_resources_destroyed;
}

bool has_live_render_tree(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats) == 1U;
}

bool has_no_live_render_resources(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats) == 0U;
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text.to_qstring();
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
}

bool snapshot_contains_text(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        if (snapshot_row_text(snapshot, row).contains(text)) {
            return true;
        }
    }

    return false;
}

bool snapshot_dirty_ranges_contain_row(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        if (row >= range.first_row && row < range.first_row + range.row_count) {
            return true;
        }
    }

    return false;
}

bool snapshot_cell_has_rgb_background(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    quint32                                rgba)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row != row || cell.position.column != column) {
            continue;
        }
        if (cell.style_id >= snapshot.styles.size()) {
            return false;
        }

        const term::Terminal_text_style& style =
            snapshot.styles[static_cast<std::size_t>(cell.style_id)];
        return
            style.background.kind == term::Terminal_color_ref_kind::RGB &&
            style.background.rgba == rgba;
    }

    return false;
}

bool snapshot_row_has_rgb_background(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count,
    quint32                                rgba)
{
    bool ok = true;
    for (int column = first_column;
         column < first_column + column_count;
         ++column)
    {
        ok &= snapshot_cell_has_rgb_background(snapshot, row, column, rgba);
    }
    return ok;
}

bool snapshot_has_selection_span(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count)
{
    for (const term::Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row          == row          &&
            span.first_column == first_column &&
            span.column_count == column_count)
        {
            return true;
        }
    }

    return false;
}

int first_visible_logical_row_for_snapshot(const term::Terminal_render_snapshot& snapshot)
{
    return snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
        ? 0
        : snapshot.viewport.scrollback_rows - snapshot.viewport.offset_from_tail;
}

bool surface_scrolls_to_first_visible_row(
    VNM_TerminalSurface&   surface,
    int                    first_visible_row)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (snapshot == nullptr) {
        return false;
    }

    const int target_offset = snapshot->viewport.scrollback_rows - first_visible_row;
    if (target_offset < 0 || target_offset > snapshot->viewport.scrollback_rows) {
        return false;
    }

    if (target_offset == snapshot->viewport.offset_from_tail) {
        return true;
    }

    return surface.scroll_to_offset_from_tail(target_offset);
}

bool surface_public_prefix_selection_is_highlighted(
    const term::Terminal_render_snapshot& snapshot,
    int                                   first_visible_row)
{
    constexpr int         k_first_selected_row = 36;
    constexpr int         k_last_selected_row  = 42;
    constexpr int         k_tail_column_count  = 17;
    constexpr std::size_t k_selected_row_count =
        static_cast<std::size_t>(k_last_selected_row - k_first_selected_row + 1);

    if (first_visible_logical_row_for_snapshot(snapshot) != first_visible_row ||
        snapshot.selection_spans.size()             != k_selected_row_count   ||
        snapshot.grid_size.columns                  <  k_tail_column_count)
    {
        return false;
    }

    for (int public_row = k_first_selected_row;
         public_row <= k_last_selected_row;
         ++public_row)
    {
        const int snapshot_row = public_row - first_visible_row;
        if (snapshot_row < 0 || snapshot_row >= snapshot.grid_size.rows) {
            return false;
        }

        const int expected_column_count =
            public_row == k_last_selected_row
                ? k_tail_column_count
                : snapshot.grid_size.columns;
        if (!snapshot_has_selection_span(snapshot, snapshot_row, 0, expected_column_count)) {
            return false;
        }
    }

    return true;
}

QByteArray numbered_scroll_lines(int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes += QStringLiteral("scroll-line-%1\r\n")
            .arg(i, 3, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

QByteArray repeated_scroll_lines(int count, const QByteArray& line)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes += line;
        bytes += QByteArrayLiteral("\r\n");
    }
    return bytes;
}

constexpr std::chrono::milliseconds k_posted_drain_budget_probe_write_delay{8};
constexpr qsizetype k_backend_output_drain_slice_contract_bytes = 4096;
constexpr qsizetype k_epoch_catchup_budget_probe_payload_bytes = 8192;

QByteArray cursor_show_boundary_title_slice(const QByteArray& title)
{
    QByteArray slice;
    slice += QByteArrayLiteral("\x1b]2;");
    slice += title;
    slice += '\a';
    const QByteArray cursor_show = QByteArrayLiteral("\x1b[?25h");
    slice += QByteArray(
        k_backend_output_drain_slice_contract_bytes - slice.size() - cursor_show.size(),
        'x');
    slice += cursor_show;
    return slice;
}

QByteArray epoch_catchup_budget_probe_payload(const QByteArray& tail_output)
{
    QByteArray bytes(k_epoch_catchup_budget_probe_payload_bytes, 'x');
    bytes += QByteArrayLiteral("\r\n");
    bytes += tail_output;
    return bytes;
}

struct Scripted_backend_lifecycle_state
{
    std::atomic<int> destructed_count{0};
    std::atomic<int> terminate_count{0};
    std::atomic<int> worker_callback_attempts{0};
    std::atomic<int> worker_callback_completions{0};
};

class Scripted_backend final : public term::Terminal_backend
{
public:
    ~Scripted_backend() override
    {
        join_worker();
        if (lifecycle_state != nullptr) {
            ++lifecycle_state->destructed_count;
        }
    }

    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        if (start_attempt_observer != nullptr) {
            ++(*start_attempt_observer);
        }

        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        m_callbacks = std::move(callbacks);
        start_configs.push_back(config);
        if (start_config_observer != nullptr) {
            start_config_observer->push_back(config);
        }
        running = true;
        if (on_start != nullptr) {
            on_start();
        }
        for (const QByteArray& output : outputs_during_start) {
            m_callbacks.output_received(output);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        if (write_delay > std::chrono::steady_clock::duration::zero()) {
            const auto deadline = std::chrono::steady_clock::now() + write_delay;
            // Stay inside the owner drain without depending on platform sleep granularity.
            while (std::chrono::steady_clock::now() < deadline) {
            }
        }
        if (reject_writes) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("scripted write rejection"));
        }
        for (const QByteArray& output : outputs_during_write) {
            m_callbacks.output_received(output);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("scripted resize requires a positive grid"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        output_pause_requests.push_back(paused);
        output_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("scripted interrupt without process"));
        }

        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("scripted terminate without process"));
        }

        if (lifecycle_state != nullptr) {
            ++lifecycle_state->terminate_count;
        }
        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        return term::backend_accept();
    }

    void emit_output(QByteArray bytes)
    {
        m_callbacks.output_received(std::move(bytes));
    }

    void emit_exit(term::Terminal_backend_exit exit)
    {
        running = false;
        m_callbacks.process_exited(exit);
    }

    void emit_error(term::Terminal_backend_error error)
    {
        m_callbacks.error_reported(std::move(error));
    }

    void emit_output_from_worker(QByteArray bytes)
    {
        join_worker();

        term::Terminal_backend_callbacks callbacks = m_callbacks;
        std::shared_ptr<Scripted_backend_lifecycle_state> state = lifecycle_state;
        worker = std::thread([callbacks, state, bytes = std::move(bytes)]() mutable {
            if (state != nullptr) {
                ++state->worker_callback_attempts;
            }
            callbacks.output_received(std::move(bytes));
            if (state != nullptr) {
                ++state->worker_callback_completions;
            }
        });
    }

    void join_worker()
    {
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool                       running                = false;
    bool                       reject_writes          = false;
    bool                       output_paused          = false;
    std::chrono::steady_clock::duration
                               write_delay{};
    std::vector<QByteArray>    outputs_during_start;
    std::vector<QByteArray>    outputs_during_write;
    std::vector<term::Terminal_launch_config>
                               start_configs;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          output_pause_requests;
    std::vector<term::Terminal_launch_config>*
                               start_config_observer  = nullptr;
    std::function<void()>      on_start;
    std::shared_ptr<Scripted_backend_lifecycle_state>
                               lifecycle_state;
    int*                       start_attempt_observer = nullptr;
    std::thread                worker;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(640, 320);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(520.0, 240.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        window.show();
    }
};

class Blocking_backend_event_hook
{
public:
    void block_until_released()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_entered_cv.notify_all();
        m_release_cv.wait(lock, [this] { return m_released; });
        m_exited = true;
        m_exited_cv.notify_all();
    }

    bool wait_until_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered_cv.wait_for(lock, timeout, [this] { return m_entered; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_release_cv.notify_all();
    }

    bool wait_until_exited(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_exited_cv.wait_for(lock, timeout, [this] { return m_exited; });
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_entered_cv;
    std::condition_variable m_release_cv;
    std::condition_variable m_exited_cv;
    bool                    m_entered  = false;
    bool                    m_released = false;
    bool                    m_exited   = false;
};

Scripted_backend* start_surface_with_backend(
    VNM_TerminalSurface&               surface,
    std::unique_ptr<Scripted_backend>  backend,
    QStringList                        argv,
    bool*                              out_started)
{
    Scripted_backend* backend_ptr = backend.get();
    *out_started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        std::move(argv));
    return backend_ptr;
}

void queue_posted_drain_budget_probe(
    Scripted_backend&   backend,
    QByteArray          tail_output)
{
    backend.write_delay          = k_posted_drain_budget_probe_write_delay;
    backend.outputs_during_write = {std::move(tail_output)};
    backend.emit_output(QByteArrayLiteral("\x1b[6n"));
}

bool send_key(
    VNM_TerminalSurface&   surface,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const char*            message)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_window_key_and_expect_write(
    VNM_TerminalSurface&   surface,
    QQuickWindow&          window,
    Scripted_backend&      backend,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const QByteArray&      expected,
    const char*            message)
{
    surface.forceActiveFocus();
    const std::size_t write_count = backend.writes.size();
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(&window, &event);
    bool ok  = check(event.isAccepted(), message);
    ok      &= check(backend.writes.size() == write_count + 1U, message);
    if (backend.writes.size() > write_count) {
        ok &= check_bytes_equal(backend.writes.back(), expected, message);
    }
    return ok;
}

bool send_key_and_expect_write(
    VNM_TerminalSurface&   surface,
    Scripted_backend&      backend,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const QByteArray&      expected,
    const char*            message)
{
    const std::size_t write_count = backend.writes.size();
    bool ok = send_key(surface, key, modifiers, text, message);
    ok &= check(backend.writes.size() == write_count + 1U, message);
    if (backend.writes.size() > write_count) {
        ok &= check_bytes_equal(backend.writes.back(), expected, message);
    }
    return ok;
}

bool send_ime_event(
    VNM_TerminalSurface&   surface,
    const QString&         commit_text,
    const QString&         preedit_text,
    int                    cursor_position,
    int                    replacement_start,
    int                    replacement_length,
    const char*            message)
{
    QList<QInputMethodEvent::Attribute> attributes;
    if (!preedit_text.isEmpty()) {
        attributes.push_back(QInputMethodEvent::Attribute(
            QInputMethodEvent::Cursor,
            cursor_position,
            1,
            QVariant()));
    }

    QInputMethodEvent event(preedit_text, attributes);
    if (!commit_text.isEmpty()) {
        event.setCommitString(commit_text, replacement_start, replacement_length);
    }
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_ime_preedit(
    VNM_TerminalSurface&   surface,
    const QString&         text,
    int                    cursor_position,
    const char*            message)
{
    return send_ime_event(surface, {}, text, cursor_position, 0, 0, message);
}

bool send_ime_commit(
    VNM_TerminalSurface&   surface,
    const QString&         text,
    const char*            message)
{
    QInputMethodEvent event;
    event.setCommitString(text);
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_empty_ime_commit(VNM_TerminalSurface& surface, const char* message)
{
    QInputMethodEvent event;
    event.setCommitString(QString());
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_empty_ime_event(VNM_TerminalSurface& surface, const char* message)
{
    QInputMethodEvent event;
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    QPointF                position,
    int                    pixel_delta_y,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    QWheelEvent event(
        position,
        position,
        QPoint(0, pixel_delta_y),
        QPoint(0, angle_delta_y),
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    int                    pixel_delta_y,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    return
        send_wheel_event(
            surface,
            modifiers,
            QPointF(8.0, 8.0),
            pixel_delta_y,
            angle_delta_y,
            expected_accepted,
            message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    return send_wheel_event(
        surface,
        modifiers,
        0,
        angle_delta_y,
        expected_accepted,
        message);
}

bool send_mouse_event(
    VNM_TerminalSurface&   surface,
    QEvent::Type           type,
    QPointF                position,
    Qt::MouseButton        button,
    Qt::MouseButtons       buttons,
    Qt::KeyboardModifiers  modifiers,
    bool                   expected_accepted,
    const char*            message)
{
    QMouseEvent event(type, position, position, position, button, buttons, modifiers);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_hover_move(
    VNM_TerminalSurface&   surface,
    QPointF                position,
    Qt::KeyboardModifiers  modifiers,
    bool                   expected_accepted,
    const char*            message)
{
    QHoverEvent event(QEvent::HoverMove, position, position, modifiers);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

QByteArray joined_writes_since(
    const std::vector<QByteArray>& writes,
    std::size_t                    first_index)
{
    QByteArray joined;
    for (std::size_t i = first_index; i < writes.size(); ++i) {
        joined.append(writes[i]);
    }
    return joined;
}

QByteArray framed_paste(QByteArray body)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[200~");
    bytes += body;
    bytes += QByteArrayLiteral("\x1b[201~");
    return bytes;
}

term::terminal_cell_metrics_t current_cell_metrics(const VNM_TerminalSurface& surface)
{
    // Mirror the surface exactly: production snaps each font metric up to the
    // device pixel grid (see Qt_grid_metrics_provider) and clamps the cell
    // height to ascent + descent. Recomputing raw QFontMetricsF values here
    // would diverge whenever ceil(ascent) + ceil(descent) exceeds lineSpacing,
    // sending too-short pixel-wheel fragments and mis-placing pointer hit-tests.
    return term::VNM_TerminalSurface_render_bridge::cell_metrics(surface);
}

bool metrics_equal(
    const term::terminal_cell_metrics_t&  actual,
    const term::terminal_cell_metrics_t&  expected,
    qreal                                tolerance)
{
    return
        std::abs(actual.width - expected.width)     <= tolerance &&
        std::abs(actual.height - expected.height)   <= tolerance &&
        std::abs(actual.ascent - expected.ascent)   <= tolerance &&
        std::abs(actual.descent - expected.descent) <= tolerance;
}

bool check_metrics_equal(
    const term::terminal_cell_metrics_t&  actual,
    const term::terminal_cell_metrics_t&  expected,
    const char*                           message)
{
    constexpr qreal tolerance = 0.000001;
    const bool equal = metrics_equal(actual, expected, tolerance);
    if (equal) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected={width=" << expected.width
        << ", height="          << expected.height
        << ", ascent="          << expected.ascent
        << ", descent="         << expected.descent
        << "} actual={width="   << actual.width
        << ", height="          << actual.height
        << ", ascent="          << actual.ascent
        << ", descent="         << actual.descent
        << "}\n";
    return false;
}

void set_window_device_pixel_ratio(QQuickWindow& window, qreal device_pixel_ratio)
{
    QWindowPrivate::get(&window)->devicePixelRatio = device_pixel_ratio;
}

QPointF point_in_grid_cell(
    const VNM_TerminalSurface& surface,
    int                        row,
    int                        column)
{
    const term::terminal_cell_metrics_t metrics = current_cell_metrics(surface);
    return {
        (static_cast<qreal>(column) + 0.5) * metrics.width,
        (static_cast<qreal>(row) + 0.5) * metrics.height,
    };
}

QByteArray sgr_mouse_report(int code, int row, int column, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes += QByteArray::number(code);
    bytes += ';';
    bytes += QByteArray::number(column + 1);
    bytes += ';';
    bytes += QByteArray::number(row + 1);
    bytes += final_byte;
    return bytes;
}

bool test_start_maps_output_to_snapshot(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int started_count  = 0;
    int activity_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_started,
        &fixture.surface,
        [&started_count] {
            ++started_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_activity,
        &fixture.surface,
        [&activity_count] {
            ++activity_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("hello from start")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(started, "surface scripted start succeeds");
    ok &= check(backend_ptr->start_configs.size() == 1U,
        "surface forwards one launch config to backend");
    ok &= check(started_count == 1, "surface emits process_started");
    ok &= check(activity_count == 1, "surface emits output_activity for start output");
    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::RUNNING,
        "surface process state becomes running");
    ok &= check(fixture.surface.backend_ready(), "surface backend ready becomes true");
    ok &= check(snapshot != nullptr && snapshot_contains_text(*snapshot, QStringLiteral("hello")),
        "surface publishes session render snapshot for start output");

    return ok;
}

bool test_surface_polish_refreshes_metrics_after_window_dpr_change(QGuiApplication& app)
{
    (void)app;

    Surface_fixture fixture;
    const term::terminal_cell_metrics_t initial_metrics =
        current_cell_metrics(fixture.surface);
    qreal selected_device_pixel_ratio = 0.0;
    term::terminal_cell_metrics_t expected_metrics{};
    for (const qreal candidate : {1.1, 1.2, 1.25, 1.333333333333, 1.5, 1.75, 2.0, 2.25}) {
        const term::Qt_grid_metrics_provider expected_provider(
            term::vnm_terminal_font(fixture.surface.font_family(), fixture.surface.font_size()),
            candidate);
        const term::terminal_cell_metrics_t candidate_metrics =
            expected_provider.cell_metrics();
        if (!metrics_equal(initial_metrics, candidate_metrics, 0.000001)) {
            selected_device_pixel_ratio = candidate;
            expected_metrics = candidate_metrics;
            break;
        }
    }

    if (selected_device_pixel_ratio <= 0.0) {
        std::cerr
            << "SKIP: no tested DPR candidate changed snapped terminal metrics "
            << "on this host\n";
        return true;
    }

    set_window_device_pixel_ratio(fixture.window, selected_device_pixel_ratio);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::terminal_cell_metrics_t updated_metrics =
        current_cell_metrics(fixture.surface);

    bool ok = true;
    ok &= check_metrics_equal(
        updated_metrics,
        expected_metrics,
        "surface polish refreshes terminal metrics after DPR-only window change");
    return ok;
}

bool test_surface_session_snapshot_burst_coalesces_to_latest_render(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("sync-burst-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface sync burst starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("sync-burst-baseline")),
        "surface sync burst publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface sync burst captures baseline before rapid updates");
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);

    const std::vector<QByteArray> burst_outputs = {
        QByteArrayLiteral("\r\nsync-burst-row-0"),
        QByteArrayLiteral("\r\nsync-burst-row-1"),
        QByteArrayLiteral("\r\nsync-burst-row-2"),
        QByteArrayLiteral("\r\nsync-burst-row-3"),
        QByteArrayLiteral("\r\nsync-burst-row-4"),
    };

    std::uint64_t previous_snapshot_sequence = baseline_snapshot->metadata.sequence;
    std::uint64_t latest_snapshot_sequence   = previous_snapshot_sequence;
    for (std::size_t i = 0; i < burst_outputs.size(); ++i) {
        backend_ptr->emit_output(burst_outputs[i]);
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->metadata.sequence > previous_snapshot_sequence,
            "surface sync burst advances snapshot sequence for each drained output");
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(
                *snapshot,
                QStringLiteral("sync-burst-row-%1").arg(static_cast<int>(i))),
            "surface sync burst latest session snapshot contains drained output");
        if (snapshot != nullptr) {
            previous_snapshot_sequence = snapshot->metadata.sequence;
            latest_snapshot_sequence   = snapshot->metadata.sequence;
        }

        const term::Terminal_surface_render_invalidation_stats_t stats_after_drain =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
        ok &= check(stats_after_drain.pending_update,
            "surface sync burst leaves render update pending while paint is delayed");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const term::Terminal_surface_render_invalidation_stats_t stats_after_noop_drain =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
        ok &= check(stats_after_noop_drain.update_requests ==
            stats_after_drain.update_requests &&
            stats_after_noop_drain.scheduled_updates ==
                stats_after_drain.scheduled_updates &&
            stats_after_noop_drain.coalesced_requests ==
                stats_after_drain.coalesced_requests,
            "surface sync burst generation gate avoids duplicate render requests");
    }

    const term::Terminal_surface_render_invalidation_stats_t burst_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(burst_pending_stats.update_requests ==
        baseline_stats.update_requests + burst_outputs.size(),
        "surface sync burst records one render request per fresh snapshot");
    ok &= check(burst_pending_stats.scheduled_updates ==
        baseline_stats.scheduled_updates + 1U,
        "surface sync burst schedules one render update for rapid session snapshots");
    ok &= check(burst_pending_stats.coalesced_requests ==
        baseline_stats.coalesced_requests + burst_outputs.size() - 1U,
        "surface sync burst coalesces pending session snapshot renders");
    ok &= check(burst_pending_stats.pending_update,
        "surface sync burst has one pending render before the delayed paint");

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        latest_snapshot_sequence),
        "surface sync burst captures the newest coalesced session snapshot");
    const term::Qsg_atlas_frame_report captured_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
    ok &= check(captured_report.captured_snapshot_sequence == latest_snapshot_sequence,
        "surface sync burst reports the latest captured snapshot sequence");

    return ok;
}

bool test_surface_polish_drains_queued_backend_output_before_render_capture(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("polish-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface polish drain starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("polish-baseline")),
        "surface polish drain publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface polish drain captures baseline before delayed output");

    backend_ptr->emit_output(QByteArrayLiteral("\r\npolish-pending-frame"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> pending_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pending_snapshot != nullptr &&
        pending_snapshot->metadata.sequence > baseline_snapshot->metadata.sequence &&
        snapshot_contains_text(*pending_snapshot, QStringLiteral("polish-pending-frame")),
        "surface polish drain creates an older pending render snapshot");
    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(pending_stats.pending_update,
        "surface polish drain leaves the older snapshot pending for capture");
    if (pending_snapshot == nullptr) {
        return ok;
    }

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\r\npolish-worker-echo"));
    backend_ptr->join_worker();
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            fixture.surface),
        "surface polish drain does not queue posted drain work before polish");
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "surface polish drain has pending worker output before polish");
    const std::shared_ptr<const term::Terminal_render_snapshot> before_polish_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(before_polish_snapshot != nullptr &&
        before_polish_snapshot->metadata.sequence == pending_snapshot->metadata.sequence &&
        !snapshot_contains_text(*before_polish_snapshot, QStringLiteral("polish-worker-echo")),
        "surface polish drain leaves queued worker output unsynced before polish");

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> polished_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(polished_snapshot != nullptr &&
        polished_snapshot->metadata.sequence > pending_snapshot->metadata.sequence &&
        snapshot_contains_text(*polished_snapshot, QStringLiteral("polish-worker-echo")),
        "surface polish drain syncs queued backend output before capture");
    const term::Terminal_surface_render_invalidation_stats_t polished_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(polished_stats.pending_update,
        "surface polish drain keeps the coalesced render update pending");
    if (polished_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        polished_snapshot->metadata.sequence),
        "surface polish drain captures the polished snapshot");

    return ok;
}

bool test_surface_no_echo_input_keeps_cursor_visible(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("\x1b[1;1Hsecret> ")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "no-echo cursor visibility surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("secret>")),
        "no-echo cursor visibility publishes baseline prompt");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "no-echo cursor visibility captures baseline prompt");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "no-echo cursor visibility reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);

    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            fixture.surface),
        "no-echo cursor visibility starts without queued backend callbacks");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"),
        QByteArrayLiteral("x"),
        "no-echo cursor visibility accepts and writes printable input");

    const Surface_frame_attempt frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    ok &= check(frame_attempt.valid,
        "no-echo cursor visibility observes a post-input frame attempt");
    if (!frame_attempt.valid) {
        return ok;
    }

    ok &= check_uint64_equal(
        frame_attempt.report.captured_snapshot_sequence,
        baseline_snapshot->metadata.sequence,
        "no-echo cursor visibility keeps the baseline snapshot");
    ok &= check(
        frame_attempt.report.captured_render_cursor.visible &&
        frame_attempt.report.captured_render_cursor.column ==
            baseline_snapshot->cursor.position.column,
        "no-echo cursor visibility keeps the terminal cursor visible");

    return ok;
}

bool test_surface_unrendered_publication_input_keeps_cursor_visible(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("\x1b[1;1Hunrendered-ready")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "unrendered publication input surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("unrendered-ready")),
        "unrendered publication input publishes baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "unrendered publication input captures baseline");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "unrendered publication input reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\x1b[2;1Hunrendered> "));
    backend_ptr->join_worker();
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> unrendered_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(unrendered_snapshot != nullptr &&
        unrendered_snapshot->metadata.publication_generation >
            baseline_snapshot->metadata.publication_generation &&
        snapshot_contains_text(*unrendered_snapshot, QStringLiteral("unrendered>")) &&
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                unrendered_snapshot->metadata.publication_generation,
        "unrendered publication input leaves live content unrendered");
    if (unrendered_snapshot == nullptr) {
        return ok;
    }

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"),
        QByteArrayLiteral("x"),
        "unrendered publication input accepts printable input");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_input =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot_after_input != nullptr &&
        snapshot_after_input->metadata.sequence == unrendered_snapshot->metadata.sequence,
        "unrendered publication input keeps pending snapshot after input");

    const Surface_frame_attempt frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    ok &= check(frame_attempt.valid,
        "unrendered publication input observes a post-input frame attempt");
    if (!frame_attempt.valid) {
        return ok;
    }

    const bool visible_stale_frame =
        frame_attempt.report.captured_snapshot_sequence ==
            unrendered_snapshot->metadata.sequence &&
        frame_attempt.report.captured_snapshot_cursor.visible &&
        frame_attempt.report.captured_render_cursor.visible &&
        frame_attempt.report.captured_render_cursor.column ==
            unrendered_snapshot->cursor.position.column;
    const bool rendered_caught_up_frame =
        frame_attempt.report.captured_snapshot_sequence >
            unrendered_snapshot->metadata.sequence &&
        frame_attempt.report.captured_render_cursor.visible;
    ok &= check(visible_stale_frame || rendered_caught_up_frame,
        "unrendered publication input keeps stale cursor visible or renders caught-up cursor");

    return ok;
}

bool test_surface_undrawn_publication_keeps_atlas_completion_pending(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("atlas-pending-ready")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "undrawn publication pending surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("atlas-pending-ready")),
        "undrawn publication pending publishes baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "undrawn publication pending captures baseline");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "undrawn publication pending reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\x1b[2;1Hpending> "));
    backend_ptr->join_worker();
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "undrawn publication pending queues output before input");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"),
        QByteArrayLiteral("x"),
        "undrawn publication pending accepts printable input");

    const std::shared_ptr<const term::Terminal_render_snapshot> undrawn_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(undrawn_snapshot != nullptr &&
        undrawn_snapshot->metadata.publication_generation >
            baseline_snapshot->metadata.publication_generation &&
        snapshot_contains_text(*undrawn_snapshot, QStringLiteral("pending>")),
        "undrawn publication pending publishes a post-input snapshot");
    if (undrawn_snapshot == nullptr) {
        return ok;
    }
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                undrawn_snapshot->metadata.publication_generation,
        "undrawn publication pending leaves the publication unrendered");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                undrawn_snapshot->metadata.publication_generation,
                false),
        "undrawn publication pending reports the publication without draw");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "undrawn publication pending keeps atlas completion pending");

    std::atomic<bool> post_boundary_callback_injected{false};
    const QMetaObject::Connection post_boundary_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!post_boundary_callback_injected.exchange(true)) {
                backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;post-title\a"));
            }
        },
        Qt::DirectConnection);
    const Surface_frame_attempt frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    QObject::disconnect(post_boundary_connection);

    ok &= check(post_boundary_callback_injected.load(std::memory_order_acquire),
        "undrawn publication pending injects post-boundary callback");
    ok &= check(frame_attempt.valid,
        "undrawn publication pending observes a frame attempt");
    ok &= check_uint64_equal(
        frame_attempt.report.captured_snapshot_sequence,
        undrawn_snapshot->metadata.sequence,
        "undrawn publication pending captures the undrawn snapshot");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                undrawn_snapshot->metadata.publication_generation,
        "undrawn publication pending remains unrendered after first capture");

    std::atomic<bool> second_post_boundary_callback_injected{false};
    const QMetaObject::Connection second_post_boundary_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!second_post_boundary_callback_injected.exchange(true)) {
                backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;post-title-again\a"));
            }
        },
        Qt::DirectConnection);
    const Surface_frame_attempt second_frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    QObject::disconnect(second_post_boundary_connection);

    ok &= check(second_post_boundary_callback_injected.load(std::memory_order_acquire),
        "undrawn publication pending injects second post-boundary callback");
    ok &= check(second_frame_attempt.valid,
        "undrawn publication pending observes a second frame attempt");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                undrawn_snapshot->metadata.publication_generation,
        "undrawn publication pending remains unrendered after second capture");

    const std::shared_ptr<const term::Terminal_render_snapshot> completion_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(completion_snapshot != nullptr,
        "undrawn publication pending has completion snapshot");
    if (completion_snapshot == nullptr) {
        return ok;
    }
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                completion_snapshot->metadata.publication_generation,
                true),
        "undrawn publication pending reports the publication rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "undrawn publication pending clears completion after draw");

    return ok;
}

bool test_surface_undrawn_publication_restores_cursor_after_render(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("no-draw-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "undrawn publication cursor surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr,
        "undrawn publication cursor publishes baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "undrawn publication cursor captures baseline");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "undrawn publication cursor reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\x1b[2;1Hnodraw> "));
    backend_ptr->join_worker();
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"),
        QByteArrayLiteral("x"),
        "undrawn publication cursor accepts printable input");

    const std::shared_ptr<const term::Terminal_render_snapshot> undrawn_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(undrawn_snapshot != nullptr &&
        undrawn_snapshot->metadata.publication_generation >
            baseline_snapshot->metadata.publication_generation &&
        snapshot_contains_text(*undrawn_snapshot, QStringLiteral("nodraw>")),
        "undrawn publication cursor publishes a post-input snapshot");
    if (undrawn_snapshot == nullptr) {
        return ok;
    }

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                undrawn_snapshot->metadata.publication_generation,
                false),
        "undrawn publication cursor reports the publication without draw");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "undrawn publication cursor leaves atlas completion pending");

    std::atomic<bool> post_boundary_callback_injected{false};
    const QMetaObject::Connection post_boundary_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!post_boundary_callback_injected.exchange(true)) {
                backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;still-pending\a"));
            }
        },
        Qt::DirectConnection);
    const Surface_frame_attempt frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    QObject::disconnect(post_boundary_connection);

    ok &= check(post_boundary_callback_injected.load(std::memory_order_acquire),
        "undrawn publication cursor injects post-boundary callback");
    ok &= check(frame_attempt.valid,
        "undrawn publication cursor observes frame attempt");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                undrawn_snapshot->metadata.publication_generation,
        "undrawn publication cursor remains unrendered after first capture");

    std::atomic<bool> second_post_boundary_callback_injected{false};
    const QMetaObject::Connection second_post_boundary_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!second_post_boundary_callback_injected.exchange(true)) {
                backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;still-pending-again\a"));
            }
        },
        Qt::DirectConnection);
    const Surface_frame_attempt second_frame_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    QObject::disconnect(second_post_boundary_connection);

    ok &= check(second_post_boundary_callback_injected.load(std::memory_order_acquire),
        "undrawn publication cursor injects second post-boundary callback");
    ok &= check(second_frame_attempt.valid,
        "undrawn publication cursor observes second frame attempt");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) <
                undrawn_snapshot->metadata.publication_generation,
        "undrawn publication cursor remains unrendered after second capture");

    const std::shared_ptr<const term::Terminal_render_snapshot> completion_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(completion_snapshot != nullptr,
        "undrawn publication cursor has completion snapshot");
    if (completion_snapshot == nullptr) {
        return ok;
    }
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                completion_snapshot->metadata.publication_generation,
                true),
        "undrawn publication cursor reports the publication rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "undrawn publication cursor clears atlas completion after draw");

    const term::Qsg_atlas_frame_report report_before_restored_cursor_frame =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
    const std::optional<term::Qsg_atlas_frame_report> restored_cursor_frame =
        capture_next_surface_frame(
            app,
            fixture.window,
            fixture.surface,
            report_before_restored_cursor_frame.capture_count);
    ok &= check(restored_cursor_frame.has_value(),
        "undrawn publication cursor observes a post-render cursor frame");
    if (restored_cursor_frame.has_value()) {
        ok &= check(
            restored_cursor_frame->capture_count >
                report_before_restored_cursor_frame.capture_count,
            "undrawn publication cursor captures a fresh post-render cursor frame");
        ok &= check_uint64_equal(
            restored_cursor_frame->captured_snapshot_sequence,
            completion_snapshot->metadata.sequence,
            "undrawn publication cursor captures the rendered snapshot");
        ok &= check_uint64_equal(
            restored_cursor_frame->captured_publication_generation,
            completion_snapshot->metadata.publication_generation,
            "undrawn publication cursor captures the rendered publication");
        ok &= check(
            restored_cursor_frame->captured_snapshot_cursor.visible &&
            restored_cursor_frame->captured_render_cursor.visible   &&
            restored_cursor_frame->captured_render_cursor.row ==
                completion_snapshot->cursor.position.row &&
            restored_cursor_frame->captured_render_cursor.column ==
                completion_snapshot->cursor.position.column,
            "undrawn publication cursor restores cursor after the publication renders");
    }

    return ok;
}

bool test_surface_frame_boundary_output_publishes_followup_dirty_rows(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral(
        "\x1b[1;1Hhold-row-zero-old\x1b[2;1Hhold-row-one-old")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "frame-boundary dirty follow-up surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_row_text(*baseline_snapshot, 0).contains(QStringLiteral("hold-row-zero-old")) &&
        snapshot_row_text(*baseline_snapshot, 1).contains(QStringLiteral("hold-row-one-old")),
        "frame-boundary dirty follow-up publishes baseline rows");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "frame-boundary dirty follow-up captures baseline");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "frame-boundary dirty follow-up reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    const std::uint64_t baseline_rendered_generation =
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface);
    ok &= check_uint64_equal(
        baseline_rendered_generation,
        baseline_snapshot->metadata.publication_generation,
        "frame-boundary dirty follow-up starts from a rendered baseline");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hhold-row-zero-new"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> first_dirty_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(first_dirty_snapshot != nullptr &&
        first_dirty_snapshot->metadata.sequence > baseline_snapshot->metadata.sequence &&
        snapshot_row_text(*first_dirty_snapshot, 0).contains(QStringLiteral("hold-row-zero-new")) &&
        snapshot_row_text(*first_dirty_snapshot, 1).contains(QStringLiteral("hold-row-one-old")),
        "frame-boundary dirty follow-up publishes first dirty row before capture");
    if (first_dirty_snapshot == nullptr) {
        return ok;
    }
    ok &= check(snapshot_dirty_ranges_contain_row(*first_dirty_snapshot, 0),
        "frame-boundary dirty follow-up first snapshot marks row zero dirty");

    const term::Qsg_atlas_frame_report report_before_held_frame =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
    std::atomic<bool> second_dirty_injected{false};
    const QMetaObject::Connection second_dirty_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!second_dirty_injected.exchange(true)) {
                backend_ptr->emit_output(QByteArrayLiteral("\x1b[2;1Hhold-row-one-new"));
            }
        },
        Qt::DirectConnection);

    const Surface_frame_attempt held_attempt =
        capture_surface_frame_attempt(app, fixture.window, fixture.surface);
    QObject::disconnect(second_dirty_connection);
    ok &= check(second_dirty_injected.load(std::memory_order_acquire),
        "frame-boundary dirty follow-up injects second dirty row after frame boundary");
    ok &= check(held_attempt.valid,
        "frame-boundary dirty follow-up observes the frame attempt");
    if (held_attempt.valid) {
        ok &= check(
            held_attempt.report.capture_count > report_before_held_frame.capture_count,
            "frame-boundary dirty follow-up captures the already-published frame");
        ok &= check(
            held_attempt.report.captured_snapshot_sequence >=
                first_dirty_snapshot->metadata.sequence,
            "frame-boundary dirty follow-up captures at least the first dirty frame");
    }

    ok &= check(pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        return
            snapshot != nullptr &&
            snapshot->metadata.sequence > first_dirty_snapshot->metadata.sequence &&
            snapshot_row_text(*snapshot, 1).contains(QStringLiteral("hold-row-one-new"));
    }),
        "frame-boundary dirty follow-up publishes the catch-up row");

    const std::shared_ptr<const term::Terminal_render_snapshot> catchup_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(catchup_snapshot != nullptr &&
        snapshot_row_text(*catchup_snapshot, 0).contains(QStringLiteral("hold-row-zero-new")) &&
        snapshot_row_text(*catchup_snapshot, 1).contains(QStringLiteral("hold-row-one-new")),
        "frame-boundary dirty follow-up catch-up snapshot has both changed rows");
    if (catchup_snapshot == nullptr) {
        return ok;
    }
    ok &= check(snapshot_dirty_ranges_contain_row(*catchup_snapshot, 0) &&
        snapshot_dirty_ranges_contain_row(*catchup_snapshot, 1),
        "frame-boundary dirty follow-up catch-up snapshot keeps both dirty rows");
    ok &= check(
        catchup_snapshot->metadata.publication_generation > baseline_rendered_generation,
        "frame-boundary dirty follow-up catch-up advances publication generation");

    const bool catchup_already_captured =
        held_attempt.valid &&
        held_attempt.report.captured_snapshot_sequence >= catchup_snapshot->metadata.sequence;
    if (!catchup_already_captured) {
        const term::Qsg_atlas_frame_report report_before_catchup_frame =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
        const std::optional<term::Qsg_atlas_frame_report> catchup_frame_report =
            capture_next_surface_frame(
                app,
                fixture.window,
                fixture.surface,
                report_before_catchup_frame.capture_count);
        ok &= check(catchup_frame_report.has_value(),
            "frame-boundary dirty follow-up captures catch-up frame");
        if (catchup_frame_report.has_value()) {
            ok &= check_uint64_equal(
                catchup_frame_report->captured_snapshot_sequence,
                catchup_snapshot->metadata.sequence,
                "frame-boundary dirty follow-up catch-up frame captures coalesced snapshot");
        }
    }
    return ok;
}

bool test_surface_backend_drain_metrics_split_stages(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("drain-stage-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface drain stage metrics starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("drain-stage-baseline")),
        "surface drain stage metrics publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface drain stage metrics captures baseline before metric checks");

    const term::Terminal_surface_backend_drain_stats_t stats_before_unbudgeted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    backend_ptr->emit_output(QByteArrayLiteral("\r\ndrain-stage-output"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const term::Terminal_surface_backend_drain_stats_t stats_after_unbudgeted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);

    ok &= check(
        stats_after_unbudgeted.unbudgeted_drain_calls ==
            stats_before_unbudgeted.unbudgeted_drain_calls + 1U,
        "surface drain stage metrics count the explicit unbudgeted drain");
    ok &= check(
        stats_after_unbudgeted.session_processing_calls ==
            stats_before_unbudgeted.session_processing_calls + 1U,
        "surface drain stage metrics count session processing stage calls");
    ok &= check(
        stats_after_unbudgeted.sync_from_session_calls ==
            stats_before_unbudgeted.sync_from_session_calls + 1U,
        "surface drain stage metrics count surface sync stage calls");
    ok &= check(
        stats_after_unbudgeted.session_processing_elapsed_ns >=
            stats_before_unbudgeted.session_processing_elapsed_ns &&
            stats_after_unbudgeted.sync_from_session_elapsed_ns >=
                stats_before_unbudgeted.sync_from_session_elapsed_ns,
        "surface drain stage metrics preserve monotonic stage elapsed counters");

    return ok;
}

bool test_surface_posted_backend_drain_uses_frame_pending_small_budget(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("posted-small-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface posted small-budget test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("posted-small-baseline")),
        "surface posted small-budget test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface posted small-budget test captures baseline");

    backend_ptr->emit_output(QByteArrayLiteral("\r\nposted-small-render-pending"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(pending_stats.pending_update,
        "surface posted small-budget test leaves render work pending");

    const term::Terminal_surface_backend_drain_stats_t stats_before_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const QString tail_text = QStringLiteral("posted-small-tail");
    const QByteArray tail   = QByteArrayLiteral("\r\nposted-small-tail");
    queue_posted_drain_budget_probe(*backend_ptr, tail);
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events_for_posted_work(
        fixture.surface);
    const term::Terminal_surface_backend_drain_stats_t stats_after_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_posted =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(
        stats_after_posted.posted_drain_calls ==
            stats_before_posted.posted_drain_calls + 1U,
        "surface posted small-budget test counts posted drains");
    ok &= check(
        stats_after_posted.posted_frame_pending_small_budget_calls ==
            stats_before_posted.posted_frame_pending_small_budget_calls + 1U,
        "surface posted small-budget test uses the frame-pending budget");
    ok &= check(
        stats_after_posted.posted_full_budget_calls ==
            stats_before_posted.posted_full_budget_calls,
        "surface posted small-budget test skips the full posted budget");
    ok &= check(
        stats_after_posted.frame_work_pending_drain_calls ==
            stats_before_posted.frame_work_pending_drain_calls + 1U,
        "surface posted small-budget test records frame-pending drain stats");
    ok &= check(
        stats_after_posted.render_update_pending_drain_calls ==
            stats_before_posted.render_update_pending_drain_calls + 1U,
        "surface posted small-budget test records render-update-pending drain stats");
    ok &= check(
        stats_after_posted.budget_exhausted_incomplete ==
            stats_before_posted.budget_exhausted_incomplete + 1U,
        "surface posted small-budget test exhausts the selected budget");
    ok &= check(
        stats_after_posted.pending_callback_after_drain ==
            stats_before_posted.pending_callback_after_drain + 1U,
        "surface posted small-budget test leaves callback work pending");
    ok &= check(
        stats_after_posted.requeue_count ==
            stats_before_posted.requeue_count + 1U,
        "surface posted small-budget test requeues incomplete posted work");
    ok &= check(snapshot_after_posted != nullptr &&
        !snapshot_contains_text(*snapshot_after_posted, tail_text),
        "surface posted small-budget test does not publish the delayed tail");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    return ok;
}

bool test_surface_frame_catchup_budget_env_config()
{
    bool ok = true;

    {
        Scoped_env_var env(
            "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_BUDGET_MS",
            std::nullopt);
        VNM_TerminalSurface surface;
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_budget_for_testing(surface));
        ok &= check(budget.count() == 4,
            "surface catch-up budget env defaults to the bounded drain slice when unset");
    }

    {
        Scoped_env_var env(
            "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_BUDGET_MS",
            QByteArrayLiteral("7"));
        VNM_TerminalSurface surface;
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_budget_for_testing(surface));
        ok &= check(budget.count() == 7,
            "surface catch-up budget env accepts millisecond values");
    }

    {
        Scoped_env_var env(
            "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_BUDGET_MS",
            QByteArrayLiteral("invalid"));
        VNM_TerminalSurface surface;
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_budget_for_testing(surface));
        ok &= check(budget.count() == 4,
            "surface catch-up budget env rejects invalid values to the default drain slice");
    }

    {
        Scoped_env_var env(
            "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_BUDGET_MS",
            QByteArrayLiteral("7"));
        VNM_TerminalSurface surface;
        term::VNM_TerminalSurface_render_bridge::
            set_backend_callback_frame_catchup_budget_for_benchmark(
                surface,
                std::chrono::milliseconds{3});
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_budget_for_testing(surface));
        ok &= check(budget.count() == 3,
            "surface catch-up budget explicit override wins over env config");
    }

    return ok;
}

bool test_surface_frame_catchup_cursor_stable_stop_extension_env_config()
{
    bool ok = true;
    const char* const env_name =
        "VNM_TERMINAL_BACKEND_CALLBACK_FRAME_CATCHUP_CURSOR_STABLE_STOP_EXTENSION_MS";

    {
        Scoped_env_var env(env_name, std::nullopt);
        VNM_TerminalSurface surface;
        const auto extension = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
                    surface));
        ok &= check(extension.count() == 0,
            "surface cursor-stable stop extension defaults to disabled when unset");
    }

    {
        Scoped_env_var env(env_name, QByteArrayLiteral("2"));
        VNM_TerminalSurface surface;
        const auto extension = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
                    surface));
        ok &= check(extension.count() == 2,
            "surface cursor-stable stop extension env accepts millisecond values");
    }

    {
        Scoped_env_var env(env_name, QByteArrayLiteral("0"));
        VNM_TerminalSurface surface;
        const auto extension = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
                    surface));
        ok &= check(extension.count() == 0,
            "surface cursor-stable stop extension env value zero disables the extension");
    }

    {
        Scoped_env_var env(env_name, QByteArrayLiteral("invalid"));
        VNM_TerminalSurface surface;
        const auto extension = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
                    surface));
        ok &= check(extension.count() == 0,
            "surface cursor-stable stop extension rejects invalid values to disabled");
    }

    {
        Scoped_env_var env(env_name, QByteArrayLiteral("5"));
        VNM_TerminalSurface surface;
        term::VNM_TerminalSurface_render_bridge::
            set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
                surface,
                std::chrono::milliseconds{2});
        const auto extension = std::chrono::duration_cast<std::chrono::milliseconds>(
            term::VNM_TerminalSurface_render_bridge::
                backend_callback_frame_catchup_cursor_stable_stop_extension_for_testing(
                    surface));
        ok &= check(extension.count() == 2,
            "surface cursor-stable stop extension explicit override wins over env config");
    }

    return ok;
}

bool test_surface_epoch_catchup_uses_frame_budget_override(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("epoch-catchup-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface epoch catch-up budget test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("epoch-catchup-baseline")),
        "surface epoch catch-up budget test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface epoch catch-up budget test captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    const term::Terminal_surface_backend_drain_stats_t stats_before_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const QString tail_text = QStringLiteral("epoch-catchup-tail");
    backend_ptr->emit_output(epoch_catchup_budget_probe_payload(tail_text.toUtf8()));
    const std::uint64_t target_epoch =
        term::VNM_TerminalSurface_render_bridge::backend_callback_enqueue_epoch(
            fixture.surface);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_polish =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const std::uint64_t processed_epoch_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_callback_processed_epoch(
            fixture.surface);

    ok &= check(
        stats_after_polish.budgeted_drain_calls ==
            stats_before_polish.budgeted_drain_calls + 1U,
        "surface epoch catch-up budget test counts polish catch-up as budgeted");
    ok &= check(
        stats_after_polish.unbudgeted_drain_calls ==
            stats_before_polish.unbudgeted_drain_calls,
        "surface epoch catch-up budget test does not use an unbudgeted epoch drain");
    ok &= check(
        stats_after_polish.budget_exhausted_incomplete ==
            stats_before_polish.budget_exhausted_incomplete + 1U,
        "surface epoch catch-up budget test exhausts the frame catch-up budget");
    ok &= check(
        stats_after_polish.pending_callback_after_drain ==
            stats_before_polish.pending_callback_after_drain + 1U,
        "surface epoch catch-up budget test leaves callback work pending");
    ok &= check(
        stats_after_polish.requeue_count ==
            stats_before_polish.requeue_count,
        "surface epoch catch-up budget test does not queue posted drain work");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            fixture.surface),
        "surface epoch catch-up budget test leaves posted drain latch clear");
    ok &= check(term::VNM_TerminalSurface_render_bridge::invalidation_stats(
            fixture.surface).pending_update,
        "surface epoch catch-up budget test keeps frame work scheduled");
    ok &= check(
        processed_epoch_after_polish < target_epoch,
        "surface epoch catch-up budget test stops before the target callback epoch");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "surface epoch catch-up budget test leaves the sliced output pending");
    ok &= check(snapshot_after_polish != nullptr &&
        !snapshot_contains_text(*snapshot_after_polish, tail_text),
        "surface epoch catch-up budget test does not publish the delayed tail");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_drain =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot_after_drain != nullptr &&
        snapshot_contains_text(*snapshot_after_drain, tail_text),
        "surface epoch catch-up budget test publishes tail after full drain");

    return ok;
}

bool test_surface_cursor_stable_extension_disabled_preserves_incomplete_boundary(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("disabled-boundary-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface disabled cursor-stable boundary test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    int title_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("disabled-boundary-baseline")),
        "surface disabled cursor-stable boundary test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface disabled cursor-stable boundary test captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?25l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_before_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const QByteArray title = QByteArrayLiteral("disabled-boundary-title");
    QByteArray output = cursor_show_boundary_title_slice(title);
    output += QByteArrayLiteral("\r\ndisabled-boundary-tail");
    ok &= check(output.startsWith(QByteArrayLiteral("\x1b]2;")) &&
            output.indexOf(QByteArrayLiteral("\x1b[?25h")) <
                k_backend_output_drain_slice_contract_bytes,
        "surface disabled cursor-stable boundary fixture shows the cursor in the first slice");
    backend_ptr->emit_output(output);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    ok &= check(
        stats_after_polish.budget_exhausted_incomplete ==
            stats_before_polish.budget_exhausted_incomplete + 1U,
        "surface disabled cursor-stable boundary records the incomplete budget stat");
    ok &= check(
        stats_after_polish.cursor_stable_incomplete ==
            stats_before_polish.cursor_stable_incomplete,
        "surface disabled cursor-stable boundary records no cursor-stable stat");
    ok &= check_int_equal(title_changed_count, 0,
        "surface disabled cursor-stable boundary defers the title notification");
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "surface disabled cursor-stable boundary leaves later output pending");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    ok &= check_int_equal(title_changed_count, 1,
        "surface disabled cursor-stable boundary delivers the title after the complete drain");
    ok &= check(fixture.surface.terminal_title() == QString::fromLatin1(title),
        "surface disabled cursor-stable boundary preserves the deferred title payload");

    return ok;
}

bool test_surface_synchronized_release_stable_with_extension_disabled_counts_cursor_stable(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);

    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("release-stable-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface release-stable stats test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("release-stable-baseline")),
        "surface release-stable stats test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface release-stable stats test captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_before_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    QByteArray output = QByteArrayLiteral("release-stable-frame\x1b[?2026l\x1b[?2026h");
    output += QByteArray(
        k_backend_output_drain_slice_contract_bytes - output.size(),
        'x');
    output += QByteArrayLiteral("release-stable-tail");
    ok &= check(output.indexOf(QByteArrayLiteral("\x1b[?2026l")) <
            k_backend_output_drain_slice_contract_bytes,
        "surface release-stable stats fixture releases inside the first slice");
    backend_ptr->emit_output(output);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_polish =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(
        stats_after_polish.budget_exhausted_incomplete ==
            stats_before_polish.budget_exhausted_incomplete,
        "surface release-stable stats test does not count release-stable as budget-exhausted");
    ok &= check(
        stats_after_polish.cursor_stable_incomplete ==
            stats_before_polish.cursor_stable_incomplete + 1U,
        "surface release-stable stats test counts release-stable as cursor-stable");
    ok &= check(
        stats_after_polish.pending_callback_after_drain ==
            stats_before_polish.pending_callback_after_drain + 1U,
        "surface release-stable stats test leaves held tail work pending");
    ok &= check(snapshot_after_polish != nullptr &&
        snapshot_contains_text(*snapshot_after_polish, QStringLiteral("release-stable-frame")),
        "surface release-stable stats test publishes the release frame");
    ok &= check(snapshot_after_polish != nullptr &&
        !snapshot_contains_text(*snapshot_after_polish, QStringLiteral("release-stable-tail")),
        "surface release-stable stats test leaves later held content unpublished");
    const term::Cursor_withhold_state_snapshot cursor_withhold_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        snapshot_after_polish != nullptr &&
        cursor_withhold_state.protected_live_content_publication_generation != 0U &&
        cursor_withhold_state.protected_live_content_publication_generation ==
            snapshot_after_polish->metadata.publication_generation &&
        cursor_withhold_state.cursor_withheld,
        "surface release-stable stats test protects cursor while tail callbacks remain pending");

    return ok;
}

bool test_surface_cursor_withhold_arms_unsafe_publication_until_settlement(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold unsafe publication surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("cursor-withhold-baseline")),
        "cursor withhold unsafe publication publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "cursor withhold unsafe publication captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    const QByteArray first_frame_marker =
        QByteArrayLiteral("\r\ncursor-withhold-frame");
    const QByteArray second_frame_marker =
        QByteArrayLiteral("\r\ncursor-withhold-next-frame");
    const QByteArray tail_marker =
        QByteArrayLiteral("\r\ncursor-withhold-tail");
    QByteArray output(
        k_backend_output_drain_slice_contract_bytes - first_frame_marker.size(),
        'x');
    output += first_frame_marker;
    output += QByteArray(
        k_backend_output_drain_slice_contract_bytes - second_frame_marker.size(),
        'y');
    output += second_frame_marker;
    output += tail_marker;
    ok &= check(
        output.indexOf(QByteArrayLiteral("cursor-withhold-tail")) >=
            2 * k_backend_output_drain_slice_contract_bytes,
        "cursor withhold unsafe publication leaves tail work outside the first slice");
    backend_ptr->emit_output(output);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> unsafe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(unsafe_snapshot != nullptr &&
        unsafe_snapshot->cursor.visible &&
        snapshot_contains_text(*unsafe_snapshot, QStringLiteral("cursor-withhold-frame")) &&
        !snapshot_contains_text(*unsafe_snapshot, QStringLiteral("cursor-withhold-tail")),
        "cursor withhold unsafe publication publishes visible-cursor content");
    if (unsafe_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.protected_live_content_publication_generation ==
            unsafe_snapshot->metadata.publication_generation &&
        unsafe_state.cursor_withheld,
        "cursor withhold unsafe publication protects the unsettled publication");
    const std::uint64_t first_protected_publication_generation =
        unsafe_state.protected_live_content_publication_generation;

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> second_unsafe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(second_unsafe_snapshot != nullptr &&
        second_unsafe_snapshot->cursor.visible &&
        snapshot_contains_text(
            *second_unsafe_snapshot,
            QStringLiteral("cursor-withhold-next-frame")) &&
        !snapshot_contains_text(*second_unsafe_snapshot, QStringLiteral("cursor-withhold-tail")),
        "cursor withhold unsafe publication publishes the second unsettled frame");
    if (second_unsafe_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot second_unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        first_protected_publication_generation != 0U &&
        second_unsafe_state.protected_live_content_publication_generation != 0U &&
        second_unsafe_state.protected_live_content_publication_generation >
            first_protected_publication_generation &&
        second_unsafe_state.protected_live_content_publication_generation ==
            second_unsafe_snapshot->metadata.publication_generation &&
        second_unsafe_state.cursor_withheld,
        "cursor withhold unsafe publication advances protection to the second unsettled publication");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> settled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(settled_snapshot != nullptr &&
        snapshot_contains_text(*settled_snapshot, QStringLiteral("cursor-withhold-tail")),
        "cursor withhold unsafe publication publishes tail after complete drain");

    const term::Cursor_withhold_state_snapshot settled_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        settled_state.protected_live_content_publication_generation == 0U &&
        !settled_state.cursor_withheld,
        "cursor withhold unsafe publication clears after proof-bearing settlement");

    return ok;
}

bool test_surface_cursor_withhold_clears_after_nonpublishing_settlement(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-empty-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold nonpublishing settlement surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    const QByteArray frame_marker =
        QByteArrayLiteral("\r\ncursor-withhold-empty-frame");
    QByteArray output(
        k_backend_output_drain_slice_contract_bytes - frame_marker.size(),
        'n');
    output += frame_marker;
    output += QByteArrayLiteral("\x1b[0m");
    backend_ptr->emit_output(output);
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> unsafe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(unsafe_snapshot != nullptr &&
        snapshot_contains_text(*unsafe_snapshot, QStringLiteral("cursor-withhold-empty-frame")),
        "cursor withhold nonpublishing settlement publishes the guarded frame");
    if (unsafe_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold nonpublishing settlement protects while callback is pending");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> settled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(settled_snapshot != nullptr &&
        settled_snapshot->metadata.publication_generation ==
            unsafe_snapshot->metadata.publication_generation,
        "cursor withhold nonpublishing settlement does not publish a replacement snapshot");

    const term::Cursor_withhold_state_snapshot settled_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        settled_state.protected_live_content_publication_generation == 0U &&
        !settled_state.cursor_withheld,
        "cursor withhold nonpublishing settlement clears after empty callback");

    return ok;
}

bool test_surface_cursor_withhold_clears_active_callback_before_follow_up(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("cursor-withhold-active-callback-baseline"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold active-callback surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    auto blocker = std::make_shared<Blocking_backend_event_hook>();
    std::atomic_bool before_follow_up_observed{false};
    term::VNM_TerminalSurface_render_bridge::
        set_backend_event_epoch_notifier_hook_for_testing(
            fixture.surface,
            [blocker] {
                blocker->block_until_released();
            });
    term::VNM_TerminalSurface_render_bridge::
        set_before_backend_callback_follow_up_hook_for_testing(
            fixture.surface,
            [&before_follow_up_observed] {
                before_follow_up_observed.store(true, std::memory_order_release);
            });

    backend_ptr->emit_output_from_worker(
        QByteArrayLiteral("\r\ncursor-withhold-active-callback-frame"));
    const bool worker_callback_held =
        blocker->wait_until_entered(std::chrono::milliseconds{1000});
    ok &= check(worker_callback_held,
        "cursor withhold active-callback test holds callback after enqueue");
    if (!worker_callback_held) {
        blocker->release();
        backend_ptr->join_worker();
        term::VNM_TerminalSurface_render_bridge::
            set_backend_event_epoch_notifier_hook_for_testing(fixture.surface, {});
        term::VNM_TerminalSurface_render_bridge::
            set_before_backend_callback_follow_up_hook_for_testing(fixture.surface, {});
        return ok;
    }

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);
    ok &= check(before_follow_up_observed.load(std::memory_order_acquire),
        "cursor withhold active-callback test queues follow-up while callback is active");
    blocker->release();
    backend_ptr->join_worker();
    term::VNM_TerminalSurface_render_bridge::
        set_backend_event_epoch_notifier_hook_for_testing(fixture.surface, {});
    term::VNM_TerminalSurface_render_bridge::
        set_before_backend_callback_follow_up_hook_for_testing(fixture.surface, {});
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) == 0U,
        "cursor withhold active-callback test leaves no callback work pending");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(
            *snapshot,
            QStringLiteral("cursor-withhold-active-callback-frame")),
        "cursor withhold active-callback test publishes the callback frame");

    const term::Cursor_withhold_state_snapshot state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        state.protected_live_content_publication_generation == 0U &&
        !state.cursor_withheld,
        "cursor withhold active-callback test clears after callback settles after follow-up");

    return ok;
}

bool test_surface_cursor_withhold_clears_incomplete_output_after_exit(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-exit-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold exit settlement surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    QByteArray output = QByteArrayLiteral("\r\ncursor-withhold-exit-frame");
    output += QByteArrayLiteral("\x1b[");
    backend_ptr->emit_output(output);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> unsafe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(unsafe_snapshot != nullptr &&
        snapshot_contains_text(*unsafe_snapshot, QStringLiteral("cursor-withhold-exit-frame")),
        "cursor withhold exit settlement publishes incomplete-output frame");
    if (unsafe_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold exit settlement protects incomplete output while running");

    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> settled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(settled_snapshot != nullptr &&
        settled_snapshot->metadata.publication_generation ==
            unsafe_snapshot->metadata.publication_generation,
        "cursor withhold exit settlement does not publish a replacement snapshot");

    const term::Cursor_withhold_state_snapshot settled_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        settled_state.protected_live_content_publication_generation == 0U &&
        !settled_state.cursor_withheld,
        "cursor withhold exit settlement clears incomplete output after exit");

    return ok;
}

bool test_surface_cursor_withhold_clears_same_drain_output_exit(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-same-drain-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold same-drain exit surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    QByteArray output = QByteArrayLiteral("\r\ncursor-withhold-same-drain-frame");
    output += QByteArrayLiteral("\x1b[");
    backend_ptr->emit_output(output);
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("cursor-withhold-same-drain-frame")),
        "cursor withhold same-drain exit publishes incomplete-output frame");

    const term::Cursor_withhold_state_snapshot state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        state.protected_live_content_publication_generation == 0U &&
        !state.cursor_withheld,
        "cursor withhold same-drain exit settles after output and exit");

    return ok;
}

bool test_surface_cursor_withhold_clears_synchronized_hold_after_exit(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-held-exit-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold held-exit surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    backend_ptr->emit_output(
        QByteArrayLiteral("cursor-withhold-held-exit-frame\x1b[?2026l\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> held_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(held_snapshot != nullptr &&
        snapshot_contains_text(*held_snapshot, QStringLiteral("cursor-withhold-held-exit-frame")),
        "cursor withhold held-exit publishes protected synchronized content");
    if (held_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot held_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        held_state.protected_live_content_publication_generation != 0U &&
        held_state.cursor_withheld,
        "cursor withhold held-exit protects before process exit");

    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot exited_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        exited_state.protected_live_content_publication_generation == 0U &&
        !exited_state.cursor_withheld,
        "cursor withhold held-exit clears without a synchronized-output release");

    return ok;
}

bool test_surface_cursor_withhold_suppresses_rendered_cursor(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-render-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold render suppression surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("cursor-withhold-render-baseline")),
        "cursor withhold render suppression publishes baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "cursor withhold render suppression captures baseline");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    backend_ptr->emit_output(
        QByteArrayLiteral("cursor-withhold-render-held\x1b[?2026l\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> held_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(held_snapshot != nullptr &&
        held_snapshot->cursor.visible &&
        snapshot_contains_text(*held_snapshot, QStringLiteral("cursor-withhold-render-held")),
        "cursor withhold render suppression publishes held visible-cursor content");
    if (held_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot held_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        held_state.protected_live_content_publication_generation != 0U &&
        held_state.cursor_withheld,
        "cursor withhold render suppression protects held content");

    const term::Qsg_atlas_frame_report report_before_withheld_frame =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
    const std::optional<term::Qsg_atlas_frame_report> withheld_frame_report =
        capture_next_surface_frame(
            app,
            fixture.window,
            fixture.surface,
            report_before_withheld_frame.capture_count);
    ok &= check(withheld_frame_report.has_value(),
        "cursor withhold render suppression captures held frame");
    if (withheld_frame_report.has_value()) {
        ok &= check_uint64_equal(
            withheld_frame_report->captured_snapshot_sequence,
            held_snapshot->metadata.sequence,
            "cursor withhold render suppression renders held snapshot");
        ok &= check(
            withheld_frame_report->captured_snapshot_cursor.visible &&
            withheld_frame_report->captured_render_cursor.valid     &&
            !withheld_frame_report->captured_render_cursor.visible,
            "cursor withhold render suppression hides rendered cursor");
    }

    const term::Cursor_withhold_state_snapshot after_withheld_frame_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_withheld_frame_state.protected_live_content_publication_generation != 0U &&
        after_withheld_frame_state.cursor_withheld,
        "cursor withhold render suppression stays withheld before settlement");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> settled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(settled_snapshot != nullptr &&
        !settled_snapshot->modes.synchronized_output,
        "cursor withhold render suppression release publishes synchronized output off");

    const term::Cursor_withhold_state_snapshot settled_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        settled_state.protected_live_content_publication_generation == 0U &&
        !settled_state.cursor_withheld,
        "cursor withhold render suppression release clears cursor protection");

    return ok;
}

bool test_surface_wheel_input_keeps_no_drain_write_output_withheld(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_alternate_screen_wheel_policy(
        VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1049hcursor-withhold-wheel-output-baseline"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold wheel output surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        baseline_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        snapshot_contains_text(
            *baseline_snapshot,
            QStringLiteral("cursor-withhold-wheel-output-baseline")),
        "cursor withhold wheel output publishes alternate-screen baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    backend_ptr->emit_output(
        QByteArrayLiteral("cursor-withhold-wheel-output-frame\x1b[?2026l\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld                  ,
        "cursor withhold wheel output starts with protected cursor");

    backend_ptr->outputs_during_write = {
        QByteArrayLiteral("cursor-withhold-wheel-output-during-write"),
    };
    const std::size_t wheel_write_index = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "cursor withhold wheel output input is accepted");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        wheel_write_index,
        { QByteArrayLiteral("\x1b[5~") },
        "cursor withhold wheel output writes page-up input");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "cursor withhold wheel output leaves backend output queued");

    const term::Cursor_withhold_state_snapshot after_write_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_write_state.protected_live_content_publication_generation != 0U &&
        after_write_state.cursor_withheld,
        "cursor withhold wheel output keeps no-drain write output withheld");

    return ok;
}

bool test_surface_cursor_withhold_keeps_hidden_backend_progress_withheld(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-stale-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold stale exemption surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    backend_ptr->emit_output(
        QByteArrayLiteral("\r\ncursor-withhold-stale-frame\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> unsafe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(unsafe_snapshot != nullptr &&
        snapshot_contains_text(*unsafe_snapshot, QStringLiteral("cursor-withhold-stale-frame")),
        "cursor withhold stale exemption publishes protected frame");
    if (unsafe_snapshot == nullptr) {
        return ok;
    }

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold stale exemption starts protected");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "cursor withhold stale exemption accepts keyboard input");

    const term::Cursor_withhold_state_snapshot after_input_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_input_state.protected_live_content_publication_generation != 0U &&
        after_input_state.cursor_withheld,
        "cursor withhold stale publication stays withheld after accepted input");

    backend_ptr->emit_output(
        QByteArray(2 * k_backend_output_drain_slice_contract_bytes, 'z'));
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> after_hidden_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(after_hidden_snapshot != nullptr &&
        after_hidden_snapshot->metadata.publication_generation ==
            unsafe_snapshot->metadata.publication_generation,
        "cursor withhold stale exemption does not publish hidden backend progress");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "cursor withhold stale exemption leaves hidden backend output unsettled");

    const term::Cursor_withhold_state_snapshot after_hidden_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_hidden_state.protected_live_content_publication_generation != 0U &&
        after_hidden_state.cursor_withheld,
        "cursor withhold stale publication keeps hidden backend progress withheld");

    return ok;
}

bool test_surface_cursor_withhold_keeps_pending_callback_withheld(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("cursor-withhold-callback-baseline"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold callback ingress surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    backend_ptr->emit_output(
        QByteArrayLiteral("\r\ncursor-withhold-callback-frame\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold callback ingress starts protected");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "cursor withhold callback ingress accepts keyboard input");

    const term::Cursor_withhold_state_snapshot after_input_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_input_state.protected_live_content_publication_generation != 0U &&
        after_input_state.cursor_withheld,
        "cursor withhold callback ingress stays withheld after accepted input");
    const std::uint64_t accepted_input_publication_generation =
        after_input_state.protected_live_content_publication_generation;

    backend_ptr->emit_error({
        term::Terminal_backend_error_code::READ_FAILED,
        QStringLiteral("cursor withhold callback ingress first error"),
    });
    backend_ptr->emit_error({
        term::Terminal_backend_error_code::READ_FAILED,
        QStringLiteral("cursor withhold callback ingress second error"),
    });
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "cursor withhold callback ingress leaves callback pending");

    const term::Cursor_withhold_state_snapshot pending_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        accepted_input_publication_generation != 0U &&
        pending_state.protected_live_content_publication_generation != 0U &&
        pending_state.protected_live_content_publication_generation ==
            accepted_input_publication_generation &&
        pending_state.cursor_withheld,
        "cursor withhold callback ingress remains protected without output progress");

    return ok;
}

bool test_surface_cursor_withhold_keeps_write_output_withheld(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-write-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold write-output surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(
        QByteArrayLiteral("\r\ncursor-withhold-write-frame\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold write-output starts protected");
    backend_ptr->outputs_during_write = {
        QByteArrayLiteral("cursor-withhold-output-during-write"),
    };
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "cursor withhold write-output accepts keyboard input");

    const term::Cursor_withhold_state_snapshot after_write_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_write_state.protected_live_content_publication_generation != 0U &&
        after_write_state.cursor_withheld,
        "cursor withhold write-output keeps synchronous backend output withheld");

    return ok;
}

bool test_surface_cursor_withhold_keeps_prescan_pending_output_withheld(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("cursor-withhold-prescan-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "cursor withhold prescan-pending surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(
        QByteArrayLiteral("\r\ncursor-withhold-prescan-frame\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot unsafe_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        unsafe_state.protected_live_content_publication_generation != 0U &&
        unsafe_state.cursor_withheld,
        "cursor withhold prescan-pending starts protected");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "cursor withhold prescan-pending accepts keyboard input");

    const term::Cursor_withhold_state_snapshot after_input_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        after_input_state.protected_live_content_publication_generation != 0U &&
        after_input_state.cursor_withheld,
        "cursor withhold prescan-pending stays withheld after accepted input");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b["));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    const term::Cursor_withhold_state_snapshot pending_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        pending_state.protected_live_content_publication_generation != 0U &&
        pending_state.cursor_withheld,
        "cursor withhold prescan-pending keeps retained incomplete output withheld");

    return ok;
}

bool test_surface_synchronized_hold_stop_preserves_drain_stats(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("held-stop-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface held stop stats test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("held-stop-baseline")),
        "surface held stop stats test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface held stop stats test captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    const term::Terminal_surface_backend_drain_stats_t stats_before_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    QByteArray output = QByteArrayLiteral("\x1b[?2026hheld-stop-a");
    output += QByteArray(
        k_backend_output_drain_slice_contract_bytes - output.size(),
        'x');
    output += QByteArrayLiteral("held-stop-b");
    backend_ptr->emit_output(output);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_polish =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(
        stats_after_polish.budget_exhausted_incomplete ==
            stats_before_polish.budget_exhausted_incomplete,
        "surface held stop stats test does not count HELD as budget-exhausted");
    ok &= check(
        stats_after_polish.cursor_stable_incomplete ==
            stats_before_polish.cursor_stable_incomplete,
        "surface held stop stats test does not count HELD as cursor-stable");
    ok &= check(
        stats_after_polish.pending_callback_after_drain ==
            stats_before_polish.pending_callback_after_drain + 1U,
        "surface held stop stats test leaves held callback work pending");
    ok &= check(snapshot_after_polish != nullptr &&
        !snapshot_contains_text(*snapshot_after_polish, QStringLiteral("held-stop-a")),
        "surface held stop stats test keeps held content unpublished");
    ok &= check(snapshot_after_polish != nullptr &&
        snapshot_after_polish->metadata.sequence == baseline_snapshot->metadata.sequence,
        "surface held stop stats test leaves the installed snapshot unchanged");

    return ok;
}

bool test_surface_epoch_catchup_pending_mouse_retry_stays_on_frame_path(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    fixture.surface.set_cursor_blink_enabled(false);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1000;1006hmouse-catchup-baseline"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface pending mouse catch-up test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(
            *baseline_snapshot,
            QStringLiteral("mouse-catchup-baseline")),
        "surface pending mouse catch-up test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface pending mouse catch-up test captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_pending_published_mouse_report_block_count_for_testing(
            fixture.surface,
            2);

    constexpr int report_row    = 0;
    constexpr int report_column = 1;
    const QPointF report_point = point_in_grid_cell(
        fixture.surface,
        report_row,
        report_column);
    const std::size_t mouse_write_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        report_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "surface pending mouse catch-up press is accepted");
    ok &= check(backend_ptr->writes.size() == mouse_write_index,
        "surface pending mouse catch-up leaves the mouse report pending");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            fixture.surface,
            std::chrono::steady_clock::duration::zero());

    const term::Terminal_surface_backend_drain_stats_t stats_before_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const QString tail_text = QStringLiteral("mouse-catchup-tail");
    backend_ptr->emit_output(epoch_catchup_budget_probe_payload(tail_text.toUtf8()));
    const std::uint64_t target_epoch =
        term::VNM_TerminalSurface_render_bridge::backend_callback_enqueue_epoch(
            fixture.surface);

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const term::Terminal_surface_backend_drain_stats_t stats_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_polish =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const std::uint64_t processed_epoch_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_callback_processed_epoch(
            fixture.surface);

    ok &= check(
        stats_after_polish.budget_exhausted_incomplete ==
            stats_before_polish.budget_exhausted_incomplete + 1U,
        "surface pending mouse catch-up exhausts the frame catch-up budget");
    ok &= check(
        stats_after_polish.pending_callback_after_drain ==
            stats_before_polish.pending_callback_after_drain + 1U,
        "surface pending mouse catch-up leaves callback work pending");
    ok &= check(
        stats_after_polish.requeue_count ==
            stats_before_polish.requeue_count,
        "surface pending mouse catch-up does not queue posted drain work");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            fixture.surface),
        "surface pending mouse catch-up leaves posted drain latch clear");
    ok &= check(term::VNM_TerminalSurface_render_bridge::invalidation_stats(
            fixture.surface).pending_update,
        "surface pending mouse catch-up keeps frame work scheduled");
    ok &= check(
        processed_epoch_after_polish < target_epoch,
        "surface pending mouse catch-up stops before the target callback epoch");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "surface pending mouse catch-up leaves the sliced output pending");
    ok &= check(backend_ptr->writes.size() == mouse_write_index,
        "surface pending mouse catch-up does not publish the mouse report");
    ok &= check(snapshot_after_polish != nullptr &&
        !snapshot_contains_text(*snapshot_after_polish, tail_text),
        "surface pending mouse catch-up does not publish the delayed tail");

    backend_ptr->emit_output(
        QByteArrayLiteral("\x1b[?2026hmouse-catchup-protected\x1b[?2026l\x1b[?2026h"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        mouse_write_index,
        { sgr_mouse_report(0, report_row, report_column, 'M') },
        "surface pending mouse catch-up publishes mouse report after full drain");
    const term::Cursor_withhold_state_snapshot retry_state =
        term::VNM_TerminalSurface_render_bridge::cursor_withhold_state_for_testing(
            fixture.surface);
    ok &= check(
        retry_state.protected_live_content_publication_generation != 0U &&
        retry_state.cursor_withheld,
        "surface pending mouse catch-up retry keeps protected cursor withheld");

    return ok;
}

bool test_surface_posted_backend_drain_uses_full_budget_without_frame_work(
    QGuiApplication& app)
{
    bool ok = true;
    pump_events(app);

    VNM_TerminalSurface surface;
    surface.setSize(QSizeF(520.0, 240.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(12.0);

    auto backend = std::make_unique<Scripted_backend>();

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface posted full-budget test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const term::Terminal_surface_render_invalidation_stats_t ready_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!ready_stats.pending_update,
        "surface posted full-budget test starts without pending frame work");

    const term::Terminal_surface_backend_drain_stats_t stats_before_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
    const QString tail_text = QStringLiteral("posted-full-tail");
    const QByteArray tail   = QByteArrayLiteral("\r\nposted-full-tail");
    queue_posted_drain_budget_probe(*backend_ptr, tail);
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events_for_posted_work(
        surface);
    const term::Terminal_surface_backend_drain_stats_t stats_after_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_posted =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);

    ok &= check(
        stats_after_posted.posted_drain_calls ==
            stats_before_posted.posted_drain_calls + 1U,
        "surface posted full-budget test counts posted drains");
    ok &= check(
        stats_after_posted.posted_full_budget_calls ==
            stats_before_posted.posted_full_budget_calls + 1U,
        "surface posted full-budget test uses the full posted budget");
    ok &= check(
        stats_after_posted.posted_frame_pending_small_budget_calls ==
            stats_before_posted.posted_frame_pending_small_budget_calls,
        "surface posted full-budget test skips the frame-pending budget");
    ok &= check(
        stats_after_posted.budget_exhausted_incomplete ==
            stats_before_posted.budget_exhausted_incomplete,
        "surface posted full-budget test completes the delayed callback work");
    ok &= check(
        stats_after_posted.pending_callback_after_drain ==
            stats_before_posted.pending_callback_after_drain,
        "surface posted full-budget test leaves no callback work pending");
    ok &= check(
        stats_after_posted.requeue_count ==
            stats_before_posted.requeue_count,
        "surface posted full-budget test does not requeue complete posted work");
    ok &= check(snapshot_after_posted != nullptr &&
        snapshot_contains_text(*snapshot_after_posted, tail_text),
        "surface posted full-budget test publishes the delayed tail");

    return ok;
}

bool test_surface_posted_backend_drain_reconciles_completed_atlas_before_budget(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("posted-atlas-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface posted atlas-budget test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("posted-atlas-baseline")),
        "surface posted atlas-budget test publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface posted atlas-budget test captures baseline");
    // The normal render-completion helpers read stats and clear the private latch.
    // Re-latch the last completed atlas report so the posted drain owns reconciliation.
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::mark_completed_atlas_completion_pending_for_testing(
            fixture.surface),
        "surface posted atlas-budget test marks completed atlas work pending");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface posted atlas-budget test leaves raw atlas completion pending");

    const term::Terminal_surface_backend_drain_stats_t stats_before_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const QString tail_text = QStringLiteral("posted-atlas-tail");
    const QByteArray tail   = QByteArrayLiteral("\r\nposted-atlas-tail");
    queue_posted_drain_budget_probe(*backend_ptr, tail);
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events_for_posted_work(
        fixture.surface);
    const term::Terminal_surface_backend_drain_stats_t stats_after_posted =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_after_posted =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface posted atlas-budget test reconciles the completed atlas latch");
    ok &= check(
        stats_after_posted.posted_full_budget_calls ==
            stats_before_posted.posted_full_budget_calls + 1U,
        "surface posted atlas-budget test uses the full posted budget");
    ok &= check(
        stats_after_posted.posted_frame_pending_small_budget_calls ==
            stats_before_posted.posted_frame_pending_small_budget_calls,
        "surface posted atlas-budget test skips the stale frame-pending budget");
    ok &= check(
        stats_after_posted.frame_work_pending_drain_calls ==
            stats_before_posted.frame_work_pending_drain_calls,
        "surface posted atlas-budget test records no stale frame-pending drain");
    ok &= check(
        stats_after_posted.atlas_completion_pending_drain_calls ==
            stats_before_posted.atlas_completion_pending_drain_calls,
        "surface posted atlas-budget test records no stale atlas-pending drain");
    ok &= check(
        stats_after_posted.budget_exhausted_incomplete ==
            stats_before_posted.budget_exhausted_incomplete,
        "surface posted atlas-budget test completes the delayed callback work");
    ok &= check(
        stats_after_posted.requeue_count ==
            stats_before_posted.requeue_count,
        "surface posted atlas-budget test does not requeue after atlas reconciliation");
    ok &= check(snapshot_after_posted != nullptr &&
        snapshot_contains_text(*snapshot_after_posted, tail_text),
        "surface posted atlas-budget test publishes the delayed tail");

    return ok;
}

bool test_surface_reported_atlas_completion_advances_rendered_publication(
    QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("atlas-publication")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "surface atlas publication test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
            snapshot->metadata.publication_generation > 0U,
        "surface atlas publication test has a published generation");
    if (snapshot == nullptr) {
        return ok;
    }

    const std::uint64_t publication_generation =
        snapshot->metadata.publication_generation;
    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        snapshot->metadata.sequence),
        "surface atlas publication test captures the current generation");
    const quint64 paint_completed_before =
        fixture.surface.paint_completed_frame_count();
    const std::uint64_t rendered_generation_before =
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                publication_generation,
                true),
        "surface atlas publication test reports the current drawn generation");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface atlas drew completion clears pending completion");
    ok &= check(
        stats.last_rendered_publication_generation == publication_generation,
        "surface atlas drew report advances rendered publication generation");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            publication_generation &&
            rendered_generation_before != publication_generation,
        "surface atlas drew report advances session rendered publication generation");
    ok &= check(
        fixture.surface.paint_completed_frame_count() ==
            paint_completed_before + 1U,
        "surface atlas drew report records paint completion in cumulative renderer stats");
    return ok;
}

bool test_surface_stale_atlas_completion_does_not_advance_rendered_publication(
    QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("stale-atlas")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "surface stale atlas completion test starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
            snapshot->metadata.publication_generation > 0U,
        "surface stale atlas completion test has a pending publication");
    if (snapshot == nullptr) {
        return ok;
    }

    const std::uint64_t current_generation =
        snapshot->metadata.publication_generation;
    const std::uint64_t stale_generation =
        current_generation > 1U ? current_generation - 1U : 0U;
    const std::uint64_t rendered_generation_before =
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface);

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                stale_generation,
                true),
        "surface stale atlas completion test reports an older rendered generation");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface stale atlas completion leaves completion pending");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(
            fixture.surface).last_rendered_publication_generation ==
            rendered_generation_before,
        "surface stale atlas completion does not advance rendered generation");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            rendered_generation_before,
        "surface stale atlas completion does not advance session rendered generation");

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                current_generation,
                false),
        "surface stale atlas completion test reports current generation without draw");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface no-draw atlas completion leaves completion pending");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            rendered_generation_before,
        "surface no-draw atlas completion does not advance session rendered generation");

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                current_generation,
                true),
        "surface stale atlas completion test reports current drawn generation");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface current drawn atlas completion clears completion pending");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(
            fixture.surface).last_rendered_publication_generation == current_generation,
        "surface current drawn atlas completion advances rendered generation");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            current_generation,
        "surface current drawn atlas completion advances session rendered generation");
    return ok;
}

bool test_surface_reset_session_clears_pending_atlas_completion(QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    pump_events(app);

    auto first_backend = std::make_unique<Scripted_backend>();
    first_backend->outputs_during_start = {QByteArrayLiteral("reset-atlas-first")};

    bool first_started = false;
    Scripted_backend* first_backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(first_backend),
        { QStringLiteral("scripted-terminal") },
        &first_started);
    ok &= check(first_started && first_backend_ptr != nullptr,
        "surface reset atlas completion first session starts");
    if (!first_started || first_backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> first_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(first_snapshot != nullptr &&
        snapshot_contains_text(*first_snapshot, QStringLiteral("reset-atlas-first")),
        "surface reset atlas completion first session publishes output");
    if (first_snapshot == nullptr) {
        return ok;
    }

    first_backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::EXITED,
        "surface reset atlas completion first session exits");

    const std::shared_ptr<const term::Terminal_render_snapshot> exited_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(exited_snapshot != nullptr,
        "surface reset atlas completion has an exited-session snapshot");
    if (exited_snapshot == nullptr) {
        return ok;
    }

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                exited_snapshot->metadata.publication_generation,
                false),
        "surface reset atlas completion latches stale no-draw completion");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface reset atlas completion starts with pending stale completion");

    auto second_backend = std::make_unique<Scripted_backend>();
    second_backend->outputs_during_start = {QByteArrayLiteral("reset-atlas-second")};

    bool second_started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(second_backend),
        { QStringLiteral("scripted-terminal") },
        &second_started);
    ok &= check(second_started,
        "surface reset atlas completion second session starts");
    ok &= check(
        !term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface reset atlas completion clears stale completion before replacement session");

    return ok;
}

bool test_surface_qsg_capture_without_draw_preserves_dirty_rows(QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("no-draw-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "surface no-draw capture dirty coalescing starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("no-draw-baseline")),
        "surface no-draw capture publishes a visible baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface no-draw capture renders the visible baseline");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                baseline_snapshot->metadata.publication_generation,
                true),
        "surface no-draw capture reports baseline rendered");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            baseline_snapshot->metadata.publication_generation,
        "surface no-draw capture starts from a rendered baseline");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?25l\x1b[2J\x1b[H"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> cleared_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(cleared_snapshot != nullptr &&
        !snapshot_contains_text(*cleared_snapshot, QStringLiteral("no-draw-baseline")) &&
        snapshot_dirty_ranges_contain_row(*cleared_snapshot, 0),
        "surface no-draw capture publishes a dirty cleared snapshot");
    if (cleared_snapshot == nullptr) {
        return ok;
    }

    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            mark_reported_atlas_completion_pending_for_testing(
                fixture.surface,
                cleared_snapshot->metadata.publication_generation,
                false),
        "surface no-draw capture reports the cleared publication without draw");
    term::VNM_TerminalSurface_render_bridge::reconcile_atlas_completion_for_testing(
        fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::atlas_completion_pending_for_testing(
            fixture.surface),
        "surface no-draw capture leaves undrawn atlas completion pending");
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::
            session_rendered_render_snapshot_generation(fixture.surface) ==
            baseline_snapshot->metadata.publication_generation,
        "surface no-draw capture does not advance rendered publication generation");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[2;1Hafter-no-draw"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> catchup_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(catchup_snapshot != nullptr &&
        snapshot_contains_text(*catchup_snapshot, QStringLiteral("after-no-draw")),
        "surface no-draw capture publishes the follow-up text snapshot");
    if (catchup_snapshot == nullptr) {
        return ok;
    }
    ok &= check(snapshot_dirty_ranges_contain_row(*catchup_snapshot, 0),
        "surface no-draw capture keeps the cleared row dirty; rendered-basis reset "
        "would drop row 0");
    ok &= check(snapshot_dirty_ranges_contain_row(*catchup_snapshot, 1),
        "surface no-draw capture marks the follow-up text row dirty");

    return ok;
}

bool test_surface_session_single_drain_coalesces_dirty_rows(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("single-drain-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface single-drain dirty coalescing starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("single-drain-baseline")),
        "surface single-drain dirty coalescing publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface single-drain dirty coalescing captures baseline");
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hsession-row-0"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[2;1Hsession-row-1"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[3;1Hsession-row-2"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-0")) &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-1")) &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-2")),
        "surface single-drain dirty coalescing latest snapshot contains every row");
    if (snapshot == nullptr) {
        return ok;
    }

    ok &= check(snapshot_dirty_ranges_contain_row(*snapshot, 0) &&
        snapshot_dirty_ranges_contain_row(*snapshot, 1) &&
        snapshot_dirty_ranges_contain_row(*snapshot, 2),
        "surface single-drain dirty coalescing carries skipped dirty rows");

    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(pending_stats.update_requests == baseline_stats.update_requests + 1U,
        "surface single-drain dirty coalescing schedules one surface snapshot");

    ok &= check(capture_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        snapshot->metadata.sequence),
        "surface single-drain dirty coalescing captures latest snapshot");
    return ok;
}

bool test_osc52_clipboard_write_signal_and_deny(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("deny-sentinel"), QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 deny surface starts");

    const QByteArray payload = QByteArrayLiteral("surface deny payload");
    backend_ptr->emit_output(osc52_write_sequence("c", payload));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 write emits one surface clipboard signal");
    if (!requests.empty()) {
        ok &= check(requests[0].request_id == 1U, "OSC 52 surface signal carries request id");
        ok &= check(requests[0].target_selection == QStringLiteral("c"),
            "OSC 52 surface signal carries target selection");
        ok &= check_bytes_equal(
            requests[0].payload,
            payload,
            "OSC 52 surface signal carries decoded payload");
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 deny response consumes pending request");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("deny-sentinel"),
        "OSC 52 deny response leaves clipboard unchanged");
    ok &= check(error_codes.empty(), "OSC 52 deny response emits no backend error");

    return ok;
}

bool test_osc52_clipboard_wrong_duplicate_and_replacement(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("replacement-sentinel"),
        QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 response policy surface starts");

    backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("first response payload")));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 first request reaches surface");
    if (!requests.empty()) {
        const quint64 first_request_id = requests[0].request_id;
        ok &= check(!fixture.surface.respond_clipboard_write(
            first_request_id + 100U,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 wrong response id fails");
        ok &= check(fixture.surface.respond_clipboard_write(
            first_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 correct response after wrong id still succeeds");
        ok &= check(!fixture.surface.respond_clipboard_write(
            first_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 duplicate response fails");
    }

    backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("late response payload")));
    backend_ptr->emit_output(
        osc52_write_sequence("p", QByteArrayLiteral("replacement payload")));
    pump_events(app);

    ok &= check(requests.size() == 3U, "OSC 52 replacement requests reach surface");
    if (requests.size() >= 3U) {
        ok &= check(requests[1].target_selection == QStringLiteral("c"),
            "OSC 52 late request carries original target");
        ok &= check_bytes_equal(
            requests[1].payload,
            QByteArrayLiteral("late response payload"),
            "OSC 52 late request carries original payload");
        ok &= check(requests[2].target_selection == QStringLiteral("p"),
            "OSC 52 replacement request carries replacement target");
        ok &= check_bytes_equal(
            requests[2].payload,
            QByteArrayLiteral("replacement payload"),
            "OSC 52 replacement request carries replacement payload");
        ok &= check(!fixture.surface.respond_clipboard_write(
            requests[1].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 replaced older response is late");
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[2].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 replacement request remains pending after late response");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("replacement-sentinel"),
        "OSC 52 wrong duplicate and late responses leave clipboard unchanged");
    ok &= check(error_codes.size() == 3U,
        "OSC 52 wrong duplicate and late responses emit backend errors");
    for (VNM_TerminalSurface::Backend_error_code code : error_codes) {
        ok &= check(code == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
            "OSC 52 failed response emits CALLBACK_MISSING");
    }

    return ok;
}

bool test_osc52_clipboard_late_exit_restart_and_targets(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("lifecycle-sentinel"),
        QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto first_backend = std::make_unique<Scripted_backend>();
    bool first_started = false;
    Scripted_backend* first_backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(first_backend),
        { QStringLiteral("scripted-terminal") },
        &first_started);
    ok &= check(first_started, "OSC 52 lifecycle first surface starts");

    first_backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("exit-stale payload")));
    pump_events(app);
    ok &= check(requests.size() == 1U, "OSC 52 lifecycle request reaches surface");
    const quint64 stale_request_id = requests.empty() ? 0U : requests[0].request_id;

    first_backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    pump_events(app);
    ok &= check(!fixture.surface.respond_clipboard_write(
        stale_request_id,
        VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
        "OSC 52 response after process exit is late");

    auto second_backend = std::make_unique<Scripted_backend>();
    bool second_started = false;
    Scripted_backend* second_backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(second_backend),
        { QStringLiteral("scripted-terminal") },
        &second_started);
    ok &= check(second_started, "OSC 52 lifecycle second surface starts");

    second_backend_ptr->emit_output(
        osc52_write_sequence("p", QByteArrayLiteral("selection-target payload")));
    pump_events(app);
    ok &= check(requests.size() == 2U, "OSC 52 second lifecycle request reaches surface");
    if (requests.size() >= 2U) {
        ok &= check(requests[1].request_id != stale_request_id,
            "OSC 52 surface request ids are not reused across restart");
        ok &= check(requests[1].target_selection == QStringLiteral("p"),
            "OSC 52 non-clipboard target is reported to host");
        ok &= check_bytes_equal(
            requests[1].payload,
            QByteArrayLiteral("selection-target payload"),
            "OSC 52 non-clipboard payload is reported to host");
        ok &= check(!fixture.surface.respond_clipboard_write(
            stale_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 stale old id cannot approve a new request");
        ok &= check(!fixture.surface.respond_clipboard_write(
            requests[1].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 unsupported target cannot be allowed as clipboard");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("lifecycle-sentinel"),
        "OSC 52 late and unsupported responses leave clipboard unchanged");
    ok &= check(error_codes.size() == 3U,
        "OSC 52 lifecycle failures emit backend errors");
    for (VNM_TerminalSurface::Backend_error_code code : error_codes) {
        ok &= check(code == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
            "OSC 52 lifecycle failure emits CALLBACK_MISSING");
    }

    return ok;
}

bool test_osc52_clipboard_allow_writes_clipboard(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("allow-sentinel"), QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 allow surface starts");

    const QByteArray payload = QByteArrayLiteral("allowed clipboard text");
    backend_ptr->emit_output(osc52_write_sequence("c", payload));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 allow request reaches surface");
    if (!requests.empty()) {
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 allow response consumes pending request");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QString::fromUtf8(payload),
        "OSC 52 allow response writes decoded payload to clipboard");
    ok &= check(!requests.empty() &&
        !fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
        "OSC 52 duplicate allow response fails");
    ok &= check(error_codes.size() == 1U &&
        error_codes[0] == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
        "OSC 52 duplicate allow response emits CALLBACK_MISSING");

    return ok;
}

bool test_keyboard_printable_controls_and_prompt_path(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard printable surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_L, Qt::NoModifier,
        QString::fromUtf8("\xce\xbb"), bytes_from_hex("cebb"),
        "printable UTF-8 writes text bytes");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Return, Qt::NoModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "Return writes carriage return");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Return, Qt::ShiftModifier,
#if defined(Q_OS_WIN)
        QStringLiteral("\r"), bytes_from_hex("1b5b31333b32383b31333b313b31363b315f"),
        "Shift+Return writes Win32 key record on Windows");
#else
        QStringLiteral("\r"), bytes_from_hex("0a"),
        "Shift+Return writes line feed");
#endif
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Tab, Qt::NoModifier,
        QStringLiteral("\t"), bytes_from_hex("09"),
        "Tab writes horizontal tab");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backspace, Qt::NoModifier,
        {}, bytes_from_hex("7f"),
        "Backspace writes DEL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Escape, Qt::NoModifier,
        {}, bytes_from_hex("1b"),
        "Escape writes ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_A, Qt::ControlModifier,
        {}, bytes_from_hex("01"),
        "Ctrl+A writes C0 SOH");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
        {}, bytes_from_hex("03"),
        "Ctrl+C without selection writes C0 ETX");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_K, Qt::ControlModifier,
        {}, bytes_from_hex("0b"),
        "Ctrl+K writes C0 VT");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_V, Qt::ControlModifier,
        {}, bytes_from_hex("16"),
        "Ctrl+V writes C0 SYN through the reusable surface");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Z, Qt::ControlModifier,
        {}, bytes_from_hex("1a"),
        "Ctrl+Z writes C0 SUB");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_BracketLeft, Qt::ControlModifier,
        {}, bytes_from_hex("1b"),
        "Ctrl+[ writes ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backslash, Qt::ControlModifier,
        {}, bytes_from_hex("1c"),
        "Ctrl+Backslash writes FS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_BracketRight, Qt::ControlModifier,
        {}, bytes_from_hex("1d"),
        "Ctrl+] writes GS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_AsciiCircum, Qt::ControlModifier,
        {}, bytes_from_hex("1e"),
        "Ctrl+^ writes RS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Underscore, Qt::ControlModifier,
        {}, bytes_from_hex("1f"),
        "Ctrl+_ writes US");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Space, Qt::ControlModifier,
        {}, bytes_from_hex("00"),
        "Ctrl+Space writes NUL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_2, Qt::ControlModifier,
        {}, bytes_from_hex("00"),
        "Ctrl+2 writes NUL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_3, Qt::ControlModifier,
        {}, bytes_from_hex("1b"),
        "Ctrl+3 writes ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_4, Qt::ControlModifier,
        {}, bytes_from_hex("1c"),
        "Ctrl+4 writes FS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::ControlModifier,
        {}, bytes_from_hex("1d"),
        "Ctrl+5 writes GS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_6, Qt::ControlModifier,
        {}, bytes_from_hex("1e"),
        "Ctrl+6 writes RS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_7, Qt::ControlModifier,
        {}, bytes_from_hex("1f"),
        "Ctrl+7 writes US");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Slash, Qt::ControlModifier,
        {}, bytes_from_hex("1f"),
        "Ctrl+/ writes US");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_8, Qt::ControlModifier,
        {}, bytes_from_hex("7f"),
        "Ctrl+8 writes DEL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_X, Qt::AltModifier,
        QStringLiteral("x"), bytes_from_hex("1b78"),
        "Alt+x prefixes printable text with ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_C,
        Qt::ControlModifier | Qt::AltModifier,
        {}, bytes_from_hex("1b03"),
        "Ctrl+Alt+C prefixes control byte with ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_At,
        Qt::ControlModifier | Qt::AltModifier,
        QStringLiteral("@"), bytes_from_hex("40"),
        "Ctrl+Alt printable text preserves layout bytes without ESC prefix");

    const std::size_t ignored_write_count = backend_ptr->writes.size();
    QKeyEvent ignored_event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
    QCoreApplication::sendEvent(&fixture.surface, &ignored_event);
    ok &= check(!ignored_event.isAccepted(), "unencoded key event is ignored");
    ok &= check(backend_ptr->writes.size() == ignored_write_count,
        "unencoded key event does not write backend bytes");

    const std::size_t prompt_write_index = backend_ptr->writes.size();
    ok &= send_key(fixture.surface, Qt::Key_Left, Qt::NoModifier, {},
        "prompt path Left is accepted");
    ok &= send_key(fixture.surface, Qt::Key_Backspace, Qt::NoModifier, {},
        "prompt path Backspace is accepted");
    for (const QChar ch : QStringLiteral("term")) {
        ok &= send_key(
            fixture.surface,
            ch.toUpper().unicode(),
            Qt::NoModifier,
            QString(ch),
            "prompt path printable key is accepted");
    }
    ok &= send_key(fixture.surface, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"),
        "prompt path Return is accepted");
    ok &= check_bytes_equal(
        joined_writes_since(backend_ptr->writes, prompt_write_index),
        bytes_from_hex("1b5b447f7465726d0d"),
        "prompt editing path writes exact byte stream");
    ok &= check(backend_error_count == 0,
        "keyboard printable success path emits no backend_error");

    return ok;
}

bool test_copy_shortcut_policy(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy shortcut surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "copy shortcut fixture has selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("copy-sentinel"),
            QClipboard::Clipboard);
        const std::size_t copy_write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C with selection is accepted by copy policy");
        ok &= check(backend_ptr->writes.size() == copy_write_count,
            "Ctrl+C with selection writes no ETX under copy policy");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "Ctrl+C with selection copies selected text to clipboard");

        fixture.surface.clear_selection();
        QGuiApplication::clipboard()->setText(QStringLiteral("copy-fallback"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "Ctrl+C without selection falls through to terminal input");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("copy-fallback"),
            "Ctrl+C fallback leaves clipboard unchanged");

        const int last_column = std::max(0, fixture.surface.columns() - 1);
        ok &= check(last_column > 5,
            "copy shortcut empty-selection fixture has enough columns");
        const QPointF empty_start = point_in_grid_cell(fixture.surface, 0, 5);
        const QPointF empty_end   = point_in_grid_cell(fixture.surface, 0, last_column);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            empty_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            empty_end,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            empty_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "copy shortcut empty selection remains active");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "copy shortcut empty selection has empty selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("empty-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t empty_selection_write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C with empty active selection is accepted by copy policy");
        ok &= check(backend_ptr->writes.size() == empty_selection_write_count,
            "Ctrl+C with empty active selection writes no ETX");
    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard).isEmpty(),
        "Ctrl+C with empty active selection copies empty text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized clear copy policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE);
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "synchronized clear copy policy selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "synchronized clear copy policy selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "synchronized clear copy policy selection release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "synchronized clear copy policy fixture has selected text");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        fixture.surface.clear_selection();
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "selected_text observes synchronized clear instead of stale visible spans");

        QGuiApplication::clipboard()->setText(QStringLiteral("synchronized-clear-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "copy-or-ignore policy consumes Ctrl+C after synchronized clear");
        ok &= check(backend_ptr->writes.size() == write_count,
            "copy after synchronized clear writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("synchronized-clear-sentinel"),
            "copy after synchronized clear leaves clipboard unchanged");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "visible-selection copy shortcut surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "visible-selection copy shortcut fixture has selected text");

        backend_ptr->emit_output(numbered_scroll_lines(80));
        QGuiApplication::clipboard()->setText(QStringLiteral("visible-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C copies the visible selection before draining queued output");
        ok &= check(backend_ptr->writes.size() == write_count,
            "visible-selection Ctrl+C writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "visible-selection Ctrl+C copies the selection visible at key time");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "scrollback-spanning copy shortcut surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int line_last_column = QStringLiteral("scroll-line-000").size() - 1;
        const bool usable_tail_snapshot =
            tail_snapshot                           != nullptr          &&
            tail_snapshot->grid_size.rows           >  5                &&
            tail_snapshot->grid_size.columns        >  line_last_column &&
            tail_snapshot->viewport.scrollback_rows >  0;
        ok &= check(usable_tail_snapshot,
            "scrollback-spanning copy shortcut fixture has usable scrollback");

        if (usable_tail_snapshot) {
            const int     anchor_row  = tail_snapshot->grid_size.rows - 2;
            const QString anchor_text = snapshot_row_text(*tail_snapshot, anchor_row);
            ok &= check(!anchor_text.isEmpty(),
                "scrollback-spanning copy shortcut anchor row has text");

            const QPointF anchor_point =
                point_in_grid_cell(fixture.surface, anchor_row, line_last_column);
            const QPointF top_point = point_in_grid_cell(fixture.surface, 0, 0);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                anchor_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                top_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut initial drag is accepted");
            ok &= send_wheel_event(
                fixture.surface,
                Qt::NoModifier,
                120,
                true,
                "scrollback-spanning copy shortcut drag scrolls upward");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                top_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut scrolled drag is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                top_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut release is accepted");

            const QString expected_text = fixture.surface.selected_text();
            const std::shared_ptr<const term::Terminal_render_snapshot> selection_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(!expected_text.isEmpty() && expected_text.contains(anchor_text),
                "scrollback-spanning copy shortcut selected_text includes offscreen text");
            ok &= check(selection_snapshot != nullptr &&
                !selection_snapshot->selection_spans.empty(),
                "scrollback-spanning copy shortcut publishes visible proven spans");
            ok &= check(selection_snapshot != nullptr &&
                !snapshot_contains_text(*selection_snapshot, anchor_text),
                "scrollback-spanning copy shortcut anchor text is outside the visible snapshot");

            QGuiApplication::clipboard()->setText(
                QStringLiteral("scrollback-spanning-sentinel"),
                QClipboard::Clipboard);
            const std::size_t write_count = backend_ptr->writes.size();
            ok &= send_key(
                fixture.surface,
                Qt::Key_C,
                Qt::ControlModifier,
                {},
                "Ctrl+C copies a scrollback-spanning selection");
            ok &= check(backend_ptr->writes.size() == write_count,
                "scrollback-spanning Ctrl+C writes no backend bytes");
            ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
                expected_text,
                "Ctrl+C copies the complete logical selection across offscreen rows");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "offscreen attached selection copy shortcut surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_tail_snapshot =
            tail_snapshot                           != nullptr &&
            tail_snapshot->grid_size.rows           >  5       &&
            tail_snapshot->grid_size.columns        >  20      &&
            tail_snapshot->viewport.scrollback_rows >  tail_snapshot->grid_size.rows;
        ok &= check(usable_tail_snapshot,
            "offscreen attached selection fixture has enough scrollback");

        if (usable_tail_snapshot) {
            const int offset_from_tail = tail_snapshot->grid_size.rows;
            ok &= check(fixture.surface.scroll_to_offset_from_tail(offset_from_tail),
                "offscreen attached selection scrolls to older scrollback");

            const std::shared_ptr<const term::Terminal_render_snapshot> selection_source =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const QString selected_row_text =
                selection_source != nullptr ? snapshot_row_text(*selection_source, 0) : QString();
            ok &= check(!selected_row_text.isEmpty(),
                "offscreen attached selection source row has text");

            const int end_column = std::max(0, static_cast<int>(selected_row_text.size()) - 1);
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(
                fixture.surface,
                0,
                end_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "offscreen attached selection press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "offscreen attached selection drag is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "offscreen attached selection release is accepted");

            const QString expected_text = fixture.surface.selected_text();
            ok &= check(!expected_text.isEmpty() && expected_text == selected_row_text,
                "offscreen attached selection captures selected text");
            ok &= check(fixture.surface.scroll_to_offset_from_tail(0),
                "offscreen attached selection returns to tail");

            const std::shared_ptr<const term::Terminal_render_snapshot> offscreen_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(offscreen_snapshot != nullptr &&
                offscreen_snapshot->viewport.offset_from_tail == 0,
                "offscreen attached selection returns snapshot to tail");
            ok &= check(offscreen_snapshot != nullptr &&
                offscreen_snapshot->selection_spans.empty(),
                "offscreen attached selection has no visible spans at tail");
            ok &= check(offscreen_snapshot != nullptr &&
                !snapshot_contains_text(*offscreen_snapshot, expected_text),
                "offscreen attached selection text is outside the tail viewport");

            QGuiApplication::clipboard()->setText(
                QStringLiteral("offscreen-attached-sentinel"),
                QClipboard::Clipboard);
            const std::size_t write_count = backend_ptr->writes.size();
            ok &= send_key(
                fixture.surface,
                Qt::Key_C,
                Qt::ControlModifier,
                {},
                "Ctrl+C copies an attached selection scrolled fully offscreen");
            ok &= check(backend_ptr->writes.size() == write_count,
                "offscreen attached selection Ctrl+C writes no ETX");
            ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
                expected_text,
                "Ctrl+C copies the offscreen attached selection");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "terminal-input copy policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("terminal-input-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "terminal-input copy policy sends Ctrl+C to backend despite selection");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("terminal-input-sentinel"),
            "terminal-input copy policy leaves clipboard unchanged");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy-or-ignore policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE);
        QGuiApplication::clipboard()->setText(QStringLiteral("copy-or-ignore-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "copy-or-ignore policy consumes Ctrl+C without selection");
        ok &= check(backend_ptr->writes.size() == write_count,
            "copy-or-ignore policy with no selection writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("copy-or-ignore-sentinel"),
            "copy-or-ignore policy with no selection leaves clipboard unchanged");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy-or-ignore selection policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE);
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("copy-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "copy-or-ignore policy consumes Ctrl+C with selection");
        ok &= check(backend_ptr->writes.size() == write_count,
            "copy-or-ignore policy with selection writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "copy-or-ignore policy with selection copies selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "Ctrl+Shift+C pass-through surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("shift-copy-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface,
            *backend_ptr,
            Qt::Key_C,
            Qt::ControlModifier | Qt::ShiftModifier,
            {},
            bytes_from_hex("03"),
            "Ctrl+Shift+C is terminal input at the reusable surface");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("shift-copy-sentinel"),
            "Ctrl+Shift+C reusable surface path leaves clipboard unchanged");
    }

    return ok;
}

bool test_control_wheel_font_zoom(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int font_size_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::font_size_changed,
        &fixture.surface,
        [&font_size_changed_count] {
            ++font_size_changed_count;
        });

    const qreal initial_font_size = fixture.surface.font_size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        false,
        "plain wheel with no session remains unconsumed");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "plain wheel leaves font size unchanged");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "first high-resolution Ctrl+wheel fragment is retained");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "first high-resolution Ctrl+wheel fragment does not zoom alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "second high-resolution Ctrl+wheel fragment is retained");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "second high-resolution Ctrl+wheel fragment does not zoom alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "third high-resolution Ctrl+wheel fragment completes one zoom step");
    ok &= check(fixture.surface.font_size() == initial_font_size + 1.0,
        "three high-resolution Ctrl+wheel fragments zoom by one pixel");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        -120,
        true,
        "Ctrl+wheel-down after high-resolution fragments is consumed");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "Ctrl+wheel-down after high-resolution fragments restores font size");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "Ctrl+wheel-up is consumed by font zoom");
    ok &= check(fixture.surface.font_size() == initial_font_size + 1.0,
        "Ctrl+wheel-up increases font size by one pixel");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        -120,
        true,
        "Ctrl+wheel-down is consumed by font zoom");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "Ctrl+wheel-down decreases font size by one pixel");

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "Ctrl+wheel zoom surface starts");
    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "Ctrl+wheel zoom is consumed while a session is active");
    ok &= check(backend_ptr->writes.size() == write_count,
        "Ctrl+wheel zoom does not write terminal input bytes");
    ok &= check(font_size_changed_count == 5,
        "Ctrl+wheel zoom emits one font-size signal per actual change");

    fixture.surface.set_font_size(72.0);
    std::vector<bool> backpressure_states;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_backpressure_changed,
        &fixture.surface,
        [&backpressure_states](bool active) {
            backpressure_states.push_back(active);
        });

    QByteArray zoom_drain_output(1100 * 1024, 'x');
    zoom_drain_output += QByteArrayLiteral("\r\nzoom-drain-output");
    backend_ptr->emit_output_from_worker(std::move(zoom_drain_output));
    backend_ptr->join_worker();
    const std::shared_ptr<const term::Terminal_render_snapshot> pre_zoom_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_zoom_snapshot != nullptr &&
        !snapshot_contains_text(*pre_zoom_snapshot, QStringLiteral("zoom-drain")),
        "deferred backend output is still pending before clamped Ctrl+wheel");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "clamped Ctrl+wheel does not drain deferred backend output");
    const std::shared_ptr<const term::Terminal_render_snapshot> post_zoom_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_zoom_snapshot != nullptr &&
        !snapshot_contains_text(*post_zoom_snapshot, QStringLiteral("zoom-drain")),
        "clamped Ctrl+wheel leaves deferred backend output pending");
    ok &= check(fixture.surface.font_size() == 72.0,
        "clamped Ctrl+wheel leaves maximum font size unchanged");
    ok &= check(backend_ptr->output_paused,
        "clamped Ctrl+wheel leaves deferred backend output paused");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> drained_zoom_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(drained_zoom_snapshot != nullptr &&
        snapshot_contains_text(*drained_zoom_snapshot, QStringLiteral("zoom-drain")),
        "normal drain exposes deferred backend output after clamped Ctrl+wheel");
    ok &= check(!backend_ptr->output_paused,
        "normal drain after clamped Ctrl+wheel resumes backend output");
    ok &= check(!backpressure_states.empty() && !backpressure_states.back(),
        "normal drain after clamped Ctrl+wheel publishes final backpressure release");
    const std::size_t post_zoom_write_count = backend_ptr->writes.size();
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "typing after clamped Ctrl+wheel drain remains writable");
    ok &= check(backend_ptr->writes.size() == post_zoom_write_count + 1U,
        "typing after clamped Ctrl+wheel drain reaches backend once");

    {
        Surface_fixture clamped_fixture;
        pump_events(app);

        clamped_fixture.surface.set_font_size(6.0);
        ok &= send_wheel_event(
            clamped_fixture.surface,
            Qt::ControlModifier,
            -120,
            true,
            "Ctrl+wheel at the minimum font size is still consumed");
        ok &= check(clamped_fixture.surface.font_size() == 6.0,
            "Ctrl+wheel at the minimum font size leaves zoom clamped");
    }

    return ok;
}

bool test_plain_wheel_scrolls_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "plain-wheel scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "plain-wheel scrollback has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    const QString tail_first_row = snapshot_row_text(*tail_snapshot, 0);
    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "plain-wheel scrollback starts on the primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows > 0,
        "plain-wheel scrollback fixture has scrollback rows");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "plain-wheel scrollback starts at the tail");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up is consumed by primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "plain wheel up publishes a scrolled render snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail == 3,
            "plain wheel up moves the viewport three rows from tail");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 3) == tail_first_row,
            "plain wheel up shifts the previous first tail row down by three");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain wheel up does not write backend bytes");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        -120,
        true,
        "plain wheel down is consumed when returning toward tail");
    const std::shared_ptr<const term::Terminal_render_snapshot> returned_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(returned_snapshot != nullptr,
        "plain wheel down publishes a returned render snapshot");
    if (returned_snapshot != nullptr) {
        ok &= check(returned_snapshot->viewport.offset_from_tail == 0,
            "plain wheel down returns the viewport to the tail");
        ok &= check(snapshot_row_text(*returned_snapshot, 0) == tail_first_row,
            "plain wheel down restores tail snapshot text");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain wheel down does not write backend bytes");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "first high-resolution plain-wheel fragment is retained");
    std::shared_ptr<const term::Terminal_render_snapshot> high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 0,
        "first high-resolution plain-wheel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "second high-resolution plain-wheel fragment is retained");
    high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 0,
        "second high-resolution plain-wheel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "third high-resolution plain-wheel fragment completes one scroll step");
    high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 3,
        "three high-resolution plain-wheel fragments scroll by one angle step");
    ok &= check(backend_ptr->writes.size() == write_count,
        "high-resolution plain-wheel fragments do not write backend bytes");

    const int first_pixel_fragment = std::max(
        1,
        static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
    const int second_pixel_fragment = std::max(
        1,
        static_cast<int>(
            std::ceil(current_cell_metrics(fixture.surface).height -
                static_cast<qreal>(first_pixel_fragment))));
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        first_pixel_fragment,
        0,
        true,
        "first plain-wheel pixel fragment is retained");
    std::shared_ptr<const term::Terminal_render_snapshot> pixel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pixel_snapshot != nullptr &&
        pixel_snapshot->viewport.offset_from_tail == 3,
        "first plain-wheel pixel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        second_pixel_fragment,
        0,
        true,
        "second plain-wheel pixel fragment completes one scroll line");
    pixel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pixel_snapshot != nullptr &&
        pixel_snapshot->viewport.offset_from_tail == 4,
        "plain-wheel pixel fragments scroll after one cell-height step");
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain-wheel pixel fragments do not write backend bytes");

    return ok;
}

bool test_public_viewport_scroll_api(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    int viewport_change_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::viewport_changed,
        &fixture.surface,
        [&viewport_change_count] {
            ++viewport_change_count;
        });

    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport scroll rejects missing session");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(1),
        "public viewport offset rejects missing session");
    ok &= check(fixture.surface.scrollback_rows() == 0,
        "public viewport starts with no scrollback rows");
    ok &= check(fixture.surface.viewport_offset_from_tail() == 0,
        "public viewport starts at tail");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport reports initial tail state");

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "public viewport API surface starts");
    ok &= check(backend_ptr != nullptr, "public viewport API retains backend");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "public viewport API has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check_int_equal(
        fixture.surface.scrollback_rows(),
        tail_snapshot->viewport.scrollback_rows,
        "public viewport exposes rendered scrollback rows");
    ok &= check_int_equal(
        fixture.surface.viewport_visible_rows(),
        tail_snapshot->viewport.visible_rows,
        "public viewport exposes rendered visible rows");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport starts at rendered tail");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport at-tail property starts true");

    const int initial_viewport_change_count = viewport_change_count;
    ok &= check(fixture.surface.scroll_viewport_lines(4),
        "public viewport line scroll moves viewport");
    const std::shared_ptr<const term::Terminal_render_snapshot> line_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(line_snapshot != nullptr,
        "public viewport line scroll publishes snapshot");
    if (line_snapshot != nullptr) {
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            line_snapshot->viewport.offset_from_tail,
            "public viewport line scroll property matches snapshot");
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            4,
            "public viewport line scroll reaches requested offset");
        ok &= check(!fixture.surface.viewport_at_tail(),
            "public viewport reports detached state");
    }
    ok &= check(viewport_change_count > initial_viewport_change_count,
        "public viewport line scroll emits viewport_changed");

    ok &= check(fixture.surface.scroll_to_offset_from_tail(100000),
        "public viewport offset scroll clamps to top");
    const std::shared_ptr<const term::Terminal_render_snapshot> top_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(top_snapshot != nullptr,
        "public viewport offset scroll publishes snapshot");
    if (top_snapshot != nullptr) {
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            top_snapshot->viewport.scrollback_rows,
            "public viewport offset clamps to maximum scrollback");
    }

    ok &= check(fixture.surface.scroll_to_offset_from_tail(0),
        "public viewport offset scroll returns to tail");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport offset returns to zero");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport reports tail after offset zero");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(0),
        "public viewport offset no-op reports no movement");

    const int pre_sync_viewport_change_count = viewport_change_count;
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport scroll rejects hidden synchronized output");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport stays at visible tail while synchronized output is hidden");
    ok &= check(viewport_change_count == pre_sync_viewport_change_count,
        "public viewport hidden synchronized scroll emits no viewport_changed");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> sync_released_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(sync_released_snapshot != nullptr &&
        sync_released_snapshot->viewport.offset_from_tail == 0,
        "public viewport synchronized output release remains at tail");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049halternate"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> alternate_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(alternate_snapshot != nullptr &&
        alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "public viewport API fixture enters alternate screen");
    const int pre_alternate_scroll_change_count = viewport_change_count;
    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport line scroll rejects alternate screen");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(1),
        "public viewport offset scroll rejects alternate screen");
    ok &= check(viewport_change_count == pre_alternate_scroll_change_count,
        "public viewport alternate-screen scroll emits no viewport_changed");

    return ok;
}

bool test_immediate_public_projection_public_scroll_api(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION);
    pump_events(app);

    ok &= check(
        fixture.surface.synchronized_output_scroll_policy() ==
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION,
        "immediate public projection policy is publicly readable");

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "immediate public projection API surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> safe_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(safe_snapshot != nullptr &&
        safe_snapshot->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        safe_snapshot->viewport.offset_from_tail == 0,
        "immediate public projection API starts from live content tail");
    if (safe_snapshot == nullptr) {
        return ok;
    }
    const int safe_scrollback_rows = fixture.surface.scrollback_rows();

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hhidden"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check_int_equal(
        fixture.surface.scrollback_rows(),
        safe_scrollback_rows,
        "immediate public projection hides held scrollback growth before public scroll");

    ok &= check(fixture.surface.scroll_viewport_lines(2),
        "immediate public projection public line scroll is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> public_scroll =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(public_scroll != nullptr &&
        public_scroll->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
        public_scroll->purpose == term::Terminal_render_snapshot_purpose::SCROLL &&
        public_scroll->viewport.offset_from_tail == 2 &&
        fixture.surface.viewport_offset_from_tail() == 2 &&
        fixture.surface.scrollback_rows() == public_scroll->viewport.scrollback_rows,
        "immediate public projection line scroll publishes visible public projection state");
    if (public_scroll == nullptr) {
        return ok;
    }

    const int public_scrollback_rows = fixture.surface.scrollback_rows();
    const std::uint64_t public_scroll_sequence = public_scroll->metadata.sequence;
    backend_ptr->emit_output(numbered_scroll_lines(5));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> after_hidden_growth =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(after_hidden_growth != nullptr &&
        after_hidden_growth->metadata.sequence == public_scroll_sequence &&
        fixture.surface.scrollback_rows() == public_scrollback_rows &&
        fixture.surface.viewport_offset_from_tail() == 2,
        "immediate public projection hidden live growth does not move scrollbar-visible state");

    ok &= check(fixture.surface.scroll_to_offset_from_tail(0),
        "immediate public projection public offset scroll is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> public_tail_scroll =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(public_tail_scroll != nullptr &&
        public_tail_scroll->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
        public_tail_scroll->purpose == term::Terminal_render_snapshot_purpose::SCROLL &&
        fixture.surface.viewport_at_tail() &&
        fixture.surface.viewport_offset_from_tail() == 0,
        "immediate public projection offset scroll publishes public tail state");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> release =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(release != nullptr &&
        release->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        fixture.surface.scrollback_rows() == release->viewport.scrollback_rows &&
        fixture.surface.scrollback_rows() > public_scrollback_rows,
        "immediate public projection release updates public state from reconciled live content");

    return ok;
}

bool test_public_viewport_scroll_source_labels(QGuiApplication& app)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    bool ok = true;

    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "public source-label temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }

    const QString transcript_path =
        transcript_dir.filePath(QStringLiteral("public_source_labels.ndjson"));
    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        fixture.surface.set_transcript_capture_path(transcript_path);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        (void)start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "public source-label surface starts");
        if (!started) {
            return ok;
        }

        const VNM_TerminalSurface::wheel_scroll_diagnostic_result_t api_lines =
            fixture.surface.scroll_viewport_lines_with_diagnostics(
                1,
                QStringLiteral("api.lines"));
        ok &= check(api_lines.event_accepted,
            "public source-label api.lines scroll is accepted");
        ok &= check(fixture.surface.scroll_to_offset_from_tail_from_source(
            3,
            QStringLiteral("api.offset")),
            "public source-label api.offset scroll is accepted");
        ok &= check(fixture.surface.scroll_viewport_lines_with_diagnostics(
            -1,
            QStringLiteral("app.scrollbar.wheel")).event_accepted,
            "public source-label app wheel scroll is accepted");
        ok &= check(fixture.surface.scroll_to_offset_from_tail_from_source(
            5,
            QStringLiteral("app.scrollbar.track")),
            "public source-label app track scroll is accepted");
        ok &= check(fixture.surface.scroll_viewport_lines_with_diagnostics(
            1,
            QStringLiteral("app.scrollbar.page")).event_accepted,
            "public source-label app page scroll is accepted");
        ok &= check(fixture.surface.scroll_to_offset_from_tail_from_source(
            0,
            QStringLiteral("app.scrollbar.thumb")),
            "public source-label app thumb scroll is accepted");
    }

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
    ok &= check(events.has_value(), "public source-label transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }

    const QStringList sources = {
        QStringLiteral("api.lines"),
        QStringLiteral("api.offset"),
        QStringLiteral("app.scrollbar.wheel"),
        QStringLiteral("app.scrollbar.track"),
        QStringLiteral("app.scrollbar.page"),
        QStringLiteral("app.scrollbar.thumb"),
    };
    for (const QString& source : sources) {
        ok &= check(transcript_has_source_event(
            *events,
            QStringLiteral("surface.scroll_intent"),
            source),
            "public source-label intent source is recorded");
        ok &= check(transcript_has_source_event(
            *events,
            QStringLiteral("surface.scroll"),
            source),
            "public source-label scroll source is recorded");
    }

    return ok;
#else
    (void)app;
    return true;
#endif
}

bool test_immediate_public_projection_page_keys(QGuiApplication& app)
{
    bool ok = true;

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "immediate page-key transcript temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }
    const QString transcript_path =
        transcript_dir.filePath(QStringLiteral("immediate_page_keys.ndjson"));
#endif

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        fixture.surface.set_synchronized_output_scroll_policy(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
        fixture.surface.set_transcript_capture_path(transcript_path);
#endif
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "immediate page-key surface starts");
        if (!started || backend_ptr == nullptr) {
            return ok;
        }

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::size_t write_count = backend_ptr->writes.size();
        const int expected_page_offset = std::min(
            fixture.surface.viewport_visible_rows(),
            fixture.surface.scrollback_rows());

        ok &= send_key(
            fixture.surface,
            Qt::Key_PageUp,
            Qt::NoModifier,
            {},
            "immediate PageUp is accepted by public projection path");
        ok &= check(backend_ptr->writes.size() == write_count,
            "immediate PageUp writes no backend bytes");
        const std::shared_ptr<const term::Terminal_render_snapshot> page_up_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(page_up_snapshot != nullptr &&
            page_up_snapshot->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            page_up_snapshot->purpose == term::Terminal_render_snapshot_purpose::SCROLL &&
            page_up_snapshot->public_scroll_diagnostics.visible_scroll_applied &&
            page_up_snapshot->viewport.offset_from_tail == expected_page_offset,
            "immediate PageUp publishes visible public projection scroll");

        ok &= send_key(
            fixture.surface,
            Qt::Key_PageDown,
            Qt::NoModifier,
            {},
            "immediate PageDown is accepted by public projection path");
        ok &= check(backend_ptr->writes.size() == write_count,
            "immediate PageDown writes no backend bytes");
        const std::shared_ptr<const term::Terminal_render_snapshot> page_down_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(page_down_snapshot != nullptr &&
            page_down_snapshot->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            page_down_snapshot->purpose == term::Terminal_render_snapshot_purpose::SCROLL &&
            page_down_snapshot->public_scroll_diagnostics.visible_scroll_applied &&
            page_down_snapshot->viewport.offset_from_tail == 0,
            "immediate PageDown publishes visible public projection scroll");
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
    ok &= check(events.has_value(), "immediate page-key transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }
    ok &= check(transcript_has_source_event(
        *events,
        QStringLiteral("surface.scroll_intent"),
        QStringLiteral("key.page")),
        "immediate page-key transcript records key.page intent source");
    ok &= check(transcript_has_source_event(
        *events,
        QStringLiteral("surface.scroll"),
        QStringLiteral("key.page")),
        "immediate page-key transcript records key.page scroll source");
#endif

    return ok;
}

bool test_public_scroll_api_records_deferred_after_projection_invalidation(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION);
    pump_events(app);

    int viewport_change_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::viewport_changed,
        &fixture.surface,
        [&viewport_change_count] {
            ++viewport_change_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "public invalidation API surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check(fixture.surface.scroll_viewport_lines(2),
        "public invalidation API initial public scroll is accepted");

    const int frozen_change_count = viewport_change_count;
    const int frozen_scrollback_rows = fixture.surface.scrollback_rows();
    const int frozen_visible_rows = fixture.surface.viewport_visible_rows();
    const int frozen_offset = fixture.surface.viewport_offset_from_tail();
    const bool frozen_at_tail = fixture.surface.viewport_at_tail();

    term::VNM_TerminalSurface_render_bridge::invalidate_public_projection_for_testing(
        fixture.surface,
        term::Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED);
    const VNM_TerminalSurface::wheel_scroll_diagnostic_result_t deferred_diagnostic =
        fixture.surface.scroll_viewport_lines_with_diagnostics(
            1,
            QStringLiteral("api.lines"));
    ok &= check(deferred_diagnostic.event_accepted,
        "public invalidation diagnostics accepts deferred line intent");
    ok &= check(deferred_diagnostic.deferred_intent_recorded,
        "public invalidation diagnostics records deferred intent");
    ok &= check(!deferred_diagnostic.local_scroll_applied,
        "public invalidation diagnostics does not claim local scroll");
    ok &= check(!deferred_diagnostic.visible_scroll_applied,
        "public invalidation diagnostics does not claim visible scroll");
    ok &= check(fixture.surface.scroll_viewport_lines(1),
        "public invalidation API line scroll records deferred intent");
    ok &= check(fixture.surface.scroll_to_offset_from_tail(frozen_offset + 1),
        "public invalidation API offset scroll records deferred intent");
    ok &= check_int_equal(
        fixture.surface.scrollback_rows(),
        frozen_scrollback_rows,
        "public invalidation API keeps public scrollback frozen");
    ok &= check_int_equal(
        fixture.surface.viewport_visible_rows(),
        frozen_visible_rows,
        "public invalidation API keeps public visible rows frozen");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        frozen_offset,
        "public invalidation API keeps public offset frozen");
    ok &= check(fixture.surface.viewport_at_tail() == frozen_at_tail,
        "public invalidation API keeps public at-tail flag frozen");
    ok &= check(viewport_change_count == frozen_change_count,
        "public invalidation API emits no viewport_changed before release");

    backend_ptr->emit_output(numbered_scroll_lines(5));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> release =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(release != nullptr &&
        release->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        release->public_scroll_diagnostics.release_reconciliation_result ==
            term::Terminal_release_reconciliation_result::DEFERRED_OFFSET,
        "public invalidation API release reconciles the deferred offset");
    ok &= check(viewport_change_count > frozen_change_count,
        "public invalidation API release emits viewport_changed");

    return ok;
}

bool test_app_route_boundaries_record_deferred_after_projection_invalidation(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    fixture.surface.set_synchronized_output_scroll_policy(
        VNM_TerminalSurface::Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION);
    pump_events(app);

    int viewport_change_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::viewport_changed,
        &fixture.surface,
        [&viewport_change_count] {
            ++viewport_change_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "app route invalidation boundary surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check(fixture.surface.scroll_to_offset_from_tail(fixture.surface.scrollback_rows()),
        "app route invalidation boundary setup reaches top boundary");

    const Frozen_public_viewport_state frozen =
        frozen_public_viewport_state(fixture.surface, viewport_change_count);
    ok &= check(frozen.offset_from_tail == frozen.scrollback_rows,
        "app route invalidation boundary setup captures top boundary");

    term::VNM_TerminalSurface_render_bridge::invalidate_public_projection_for_testing(
        fixture.surface,
        term::Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED);

    struct Route_case
    {
        QString source;
        bool    offset_route = false;
    };

    const Route_case routes[] = {
        {QStringLiteral("app.scrollbar.wheel"), false},
        {QStringLiteral("app.scrollbar.page"),  false},
        {QStringLiteral("app.scrollbar.track"), true},
        {QStringLiteral("app.scrollbar.thumb"), true},
    };

    for (const Route_case& route : routes) {
        const VNM_TerminalSurface::wheel_scroll_diagnostic_result_t diagnostic =
            route.offset_route
                ? fixture.surface.scroll_to_offset_from_tail_with_diagnostics(
                    frozen.offset_from_tail + 1,
                    route.source)
                : fixture.surface.scroll_viewport_lines_with_diagnostics(
                    1,
                    route.source);
        ok &= check_route(
            diagnostic.event_accepted,
            route.source,
            " accepts invalidated boundary deferred intent");
        ok &= check_route(
            diagnostic.deferred_intent_recorded,
            route.source,
            " records invalidated boundary deferred intent");
        ok &= check_route(
            !diagnostic.local_scroll_applied,
            route.source,
            " does not claim local scroll after invalidation");
        ok &= check_route(
            !diagnostic.visible_scroll_applied,
            route.source,
            " does not claim visible scroll after invalidation");
        ok &= check_public_viewport_frozen(
            fixture.surface,
            frozen,
            viewport_change_count,
            route.source);
    }

    backend_ptr->emit_output(numbered_scroll_lines(5));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> release =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(release != nullptr &&
        release->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        release->public_scroll_diagnostics.release_reconciliation_result ==
            term::Terminal_release_reconciliation_result::DEFERRED_OFFSET,
        "app route invalidation boundary release reconciles deferred offset");
    ok &= check(viewport_change_count > frozen.viewport_change_count,
        "app route invalidation boundary release emits viewport_changed");

    return ok;
}

bool test_mid_hold_policy_flip_keeps_text_area_wheel_boundary_input(
    QGuiApplication& app)
{
    bool ok = true;

    struct Test_case
    {
        int         start_offset_from_tail;
        int         wheel_delta;
        const char* label;
    };

    const Test_case cases[] = {
        {0, -120, "tail-down"},
        {-1, 120, "top-up"},
    };

    for (const Test_case& test_case : cases) {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
        fixture.surface.set_synchronized_output_scroll_policy(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mid-hold policy flip surface starts");
        if (!started || backend_ptr == nullptr) {
            continue;
        }

        if (test_case.start_offset_from_tail < 0) {
            ok &= check(fixture.surface.scroll_to_offset_from_tail(
                fixture.surface.scrollback_rows()),
                "mid-hold policy flip scrolls to top boundary");
        }
        else {
            ok &= check(fixture.surface.viewport_offset_from_tail() == 0,
                "mid-hold policy flip starts at tail boundary");
        }

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        fixture.surface.set_synchronized_output_scroll_policy(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION);
        term::VNM_TerminalSurface_render_bridge::simulate_scene_graph_invalidated(fixture.surface);

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            test_case.wheel_delta,
            false,
            test_case.label);
        ok &= check(backend_ptr->writes.size() == write_count,
            "mid-hold policy flip boundary wheel writes no backend bytes");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> first_release =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(first_release != nullptr &&
            first_release->public_scroll_diagnostics.effective_policy ==
                term::Terminal_synchronized_output_scroll_policy::
                    IMMEDIATE_PUBLIC_PROJECTION &&
            first_release->public_scroll_diagnostics.policy_change_event ==
                term::Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD,
            "mid-hold policy flip keeps immediate policy latched through release");

        ok &= check(fixture.surface.scroll_to_offset_from_tail(1),
            "mid-hold policy flip primes second hold with scrollable public viewport");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hnext-held"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> second_hold_before =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(second_hold_before != nullptr,
            "mid-hold policy flip second hold keeps a public snapshot");
        const std::uint64_t second_hold_sequence =
            second_hold_before != nullptr
                ? second_hold_before->metadata.sequence
                : 0U;
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "mid-hold policy flip second hold accepts scroll under deferred policy");
        const std::shared_ptr<const term::Terminal_render_snapshot> second_hold_after =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(second_hold_after != nullptr &&
            second_hold_after->metadata.sequence == second_hold_sequence &&
            second_hold_after->basis != term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION,
            "mid-hold policy flip second hold does not publish immediate projection scroll");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> second_release =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(second_release != nullptr &&
            second_release->public_scroll_diagnostics.effective_policy ==
                term::Terminal_synchronized_output_scroll_policy::
                    DEFER_UNTIL_CONTENT_PUBLICATION &&
            second_release->public_scroll_diagnostics.policy_change_event ==
                term::Terminal_synchronized_output_policy_change_event::NONE,
            "mid-hold policy flip next hold uses configured deferred policy");
    }

    return ok;
}

bool test_plain_wheel_scrolls_scroll_region_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[1;1Htop-one"
            "\x1b[2;1Htop-two"
            "\x1b[3;1Hview"
            "\x1b[4;1Hbelow"
            "\x1b[5;1Hprompt"
            "\x1b[4;5r\x1b[4;1H\x1b" "M"
            "\x1b[r"
            "\x1b[1;4r\x1b[3;1H\r\nHIST"
            "\x1b[r\x1b[5;1H"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "scroll-region scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "scroll-region scrollback has a render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "scroll-region scrollback remains on primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows == 0,
        "scroll-region first insert does not create scrollback before overflow");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "scroll-region scrollback starts at tail");
    ok &= check(snapshot_row_text(*tail_snapshot, 3) == QStringLiteral("HIST"),
        "scroll-region first insert writes history row");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        false,
        "plain wheel up has no scroll-region scrollback before overflow");
    ok &= check(backend_ptr->writes.size() == write_count,
        "scroll-region pre-overflow wheel writes no backend bytes");

    backend_ptr->emit_output(
        QByteArrayLiteral("\x1b[1;4r\x1b[4;1H\r\nNEXT"
            "\x1b[r\x1b[5;1H"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> overflow_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(overflow_snapshot != nullptr,
        "scroll-region overflow publishes a snapshot");
    if (overflow_snapshot != nullptr) {
        ok &= check(overflow_snapshot->viewport.scrollback_rows == 1,
            "scroll-region overflow creates one primary scrollback row");
        ok &= check(snapshot_row_text(*overflow_snapshot, 0) == QStringLiteral("top-two"),
            "scroll-region overflow tail shows shifted primary content");
    }

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up scrolls overflowing scroll-region primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "scroll-region overflowing wheel scroll publishes a snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail == 1,
            "scroll-region overflowing wheel scroll moves to the available scrollback row");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("top-one"),
            "scroll-region overflowing wheel scroll reveals the inserted scrollback row");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "scroll-region overflowing wheel scroll writes no backend bytes");

    return ok;
}

bool test_plain_wheel_scrolls_csi_scroll_up_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[1;1H111"
            "\x1b[2;1H222"
            "\x1b[3;1H333"
            "\x1b[4;1H444"
            "\x1b[1;3r\x1b[2S\x1b[r"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "CSI SU scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr, "CSI SU scrollback has a render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "CSI SU scrollback remains on primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows == 2,
        "CSI SU creates primary scrollback rows");
    ok &= check(snapshot_row_text(*tail_snapshot, 0) == QStringLiteral("333"),
        "CSI SU tail shows shifted content");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up scrolls CSI SU primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "CSI SU wheel scroll publishes a snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail > 0,
            "CSI SU wheel scroll moves into scrollback");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("111"),
            "CSI SU wheel scroll reveals earliest scrolled row");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "CSI SU wheel scroll writes no backend bytes");

    return ok;
}

bool test_page_keys_scroll_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "page-key scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "page-key scrollback has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "page-key scrollback starts on the primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows > 0,
        "page-key scrollback fixture has scrollback rows");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "page-key scrollback starts at the tail");

    std::size_t write_count = backend_ptr->writes.size();
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        "PageDown at tail is terminal-owned when scrollback exists");
    std::shared_ptr<const term::Terminal_render_snapshot> boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(boundary_snapshot != nullptr &&
        boundary_snapshot->viewport.offset_from_tail == 0,
        "PageDown at tail leaves the viewport at the tail");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageDown at tail writes no backend bytes when scrollback exists");

    write_count = backend_ptr->writes.size();
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        "PageUp is consumed by primary scrollback");
    std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "PageUp publishes a scrolled render snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail ==
            tail_snapshot->viewport.visible_rows,
            "PageUp moves the viewport by one visible page");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp scrollback writes no backend bytes");

    ok &= send_key(
        fixture.surface,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        "PageDown is consumed when returning toward tail");
    scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr &&
        scrolled_snapshot->viewport.offset_from_tail == 0,
        "PageDown returns the viewport to the tail");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageDown scrollback writes no backend bytes");

    write_count = backend_ptr->writes.size();
    bool reached_top = false;
    for (int i = 0; i < tail_snapshot->viewport.scrollback_rows + 2; ++i) {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        if (snapshot                            == nullptr ||
            snapshot->viewport.offset_from_tail >= snapshot->viewport.scrollback_rows)
        {
            reached_top = snapshot != nullptr &&
                snapshot->viewport.offset_from_tail >= snapshot->viewport.scrollback_rows;
            break;
        }

        ok &= send_key(
            fixture.surface,
            Qt::Key_PageUp,
            Qt::NoModifier,
            {},
            "PageUp advances toward the top of primary scrollback");
    }
    ok &= check(reached_top, "PageUp loop reaches the top of primary scrollback");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp toward top writes no backend bytes");
    boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const int top_offset =
        boundary_snapshot != nullptr
            ? boundary_snapshot->viewport.offset_from_tail
            : -1;
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        "PageUp at scrollback top is terminal-owned");
    boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(boundary_snapshot != nullptr &&
        boundary_snapshot->viewport.offset_from_tail == top_offset,
        "PageUp at scrollback top leaves the viewport at the top");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp at scrollback top writes no backend bytes");

    return ok;
}

bool test_page_keys_fall_through_on_alternate_screen(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("primary-before\r\n\x1b[?1049halternate-screen"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "alternate Page-key surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate Page-key fixture enters the alternate screen");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        QByteArrayLiteral("\x1b[5~"),
        "PageUp on alternate screen writes terminal input");
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        QByteArrayLiteral("\x1b[6~"),
        "PageDown on alternate screen writes terminal input");

    return ok;
}

bool test_plain_wheel_boundaries_and_alternate_input(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "no-scrollback wheel boundary surface starts");

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            false,
            "plain wheel with no scrollback remains unconsumed");
        ok &= check(backend_ptr->writes.size() == write_count,
            "plain wheel with no scrollback writes no backend bytes");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "pending alternate-screen wheel surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(initial_snapshot != nullptr &&
            initial_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
            initial_snapshot->viewport.scrollback_rows > 0 &&
            initial_snapshot->viewport.offset_from_tail == 0,
            "pending alternate-screen fixture starts on primary scrollback tail");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049hpending-alternate"));
        const std::size_t pending_alternate_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "pending alternate-screen transition does not drain before wheel routing");
        const std::shared_ptr<const term::Terminal_render_snapshot> pending_wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(pending_wheel_snapshot != nullptr &&
            pending_wheel_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
            pending_wheel_snapshot->viewport.offset_from_tail > 0,
            "pending alternate-screen wheel uses published primary scrollback");
        ok &= check(backend_ptr->writes.size() == pending_alternate_wheel_index,
            "pending alternate-screen wheel writes no terminal input");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> drained_alternate_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(drained_alternate_snapshot != nullptr &&
            drained_alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
            "normal drain applies pending alternate-screen transition");

        const std::size_t drained_alternate_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "drained alternate-screen transition affects future wheel routing");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drained_alternate_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "future alternate-screen wheel writes cursor-up input after normal drain");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen wheel input surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
            "alternate-screen wheel boundary fixture enters the alternate screen");

        const std::size_t wheel_up_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "plain wheel up on alternate screen falls back to terminal input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_up_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "plain wheel up on alternate screen writes cursor-up input");

        const std::size_t partial_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "first alternate-screen high-resolution wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "first alternate-screen high-resolution wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "second alternate-screen high-resolution wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "second alternate-screen high-resolution wheel fragment writes no input");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1007h"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            80,
            true,
            "pending alternate-scroll toggle is accepted conservatively");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "pending alternate-scroll toggle writes no stale fragment input");
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "first post-toggle high-resolution wheel fragment is retained");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "first post-toggle high-resolution wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            80,
            true,
            "second post-toggle high-resolution wheel fragment completes a fresh step");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            partial_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "alternate-screen high-resolution wheel fragments write after a fresh full step");

        const std::size_t wheel_down_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            -120,
            true,
            "plain wheel down on alternate screen sends terminal input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_down_index,
            {
                QByteArrayLiteral("\x1b[B"),
                QByteArrayLiteral("\x1b[B"),
                QByteArrayLiteral("\x1b[B"),
            },
            "plain wheel down on alternate screen writes cursor-down input");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1h"));
        const std::size_t application_cursor_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "alternate-screen wheel with pending application-cursor mode is accepted");
        ok &= check(backend_ptr->writes.size() == application_cursor_index,
            "pending application-cursor mode prevents stale alternate-screen input");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::size_t drained_application_cursor_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "drained application-cursor mode affects future alternate-screen wheel");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drained_application_cursor_index,
            {
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
            },
            "future alternate-screen wheel input honors drained application-cursor mode");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1007l"));
        const std::size_t alternate_scroll_reset_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "plain wheel with pending DEC 1007 reset is accepted conservatively");
        ok &= check(backend_ptr->writes.size() == alternate_scroll_reset_index,
            "pending DEC 1007 reset prevents stale alternate-screen input");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::size_t drained_alternate_scroll_reset_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "plain wheel after drained DEC 1007 reset still uses fallback input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drained_alternate_scroll_reset_index,
            {
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
            },
            "plain wheel after drained DEC 1007 reset writes application-cursor input");

        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_ONLY);
        const std::size_t local_only_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            false,
            "local-only wheel policy leaves alternate-screen wheel unconsumed");
        ok &= check(backend_ptr->writes.size() == local_only_index,
            "local-only wheel policy writes no alternate-screen input");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized alternate-scroll surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?1007h"));
        const std::size_t synchronized_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "synchronized hidden alternate-scroll wheel is accepted conservatively");
        ok &= check(backend_ptr->writes.size() == synchronized_wheel_index,
            "alternate-scroll wheel does not use hidden synchronized-output mode");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1007halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "local-first alternate-screen wheel surface starts");

        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
        const std::size_t partial_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "first local-first alternate-screen wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "first local-first alternate-screen wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "second local-first alternate-screen wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "second local-first alternate-screen wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "third local-first alternate-screen wheel fragment writes input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            partial_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "local-first alternate-screen fragments are not lost to local scroll");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen page-wheel policy surface starts");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t wheel_up_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "alternate-screen page-wheel policy accepts wheel up");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_up_index,
            { QByteArrayLiteral("\x1b[5~") },
            "alternate-screen page-wheel policy writes PageUp input");

        const std::size_t wheel_down_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            -120,
            true,
            "alternate-screen page-wheel policy accepts wheel down");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_down_index,
            { QByteArrayLiteral("\x1b[6~") },
            "alternate-screen page-wheel policy writes PageDown input");

        const int first_pixel_fragment = std::max(
            1,
            static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
        const int second_pixel_fragment = std::max(
            1,
            static_cast<int>(
                std::ceil(current_cell_metrics(fixture.surface).height -
                    static_cast<qreal>(first_pixel_fragment))));
        const std::size_t pixel_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            first_pixel_fragment,
            0,
            true,
            "first alternate-screen page-wheel pixel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == pixel_wheel_index,
            "first alternate-screen page-wheel pixel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            second_pixel_fragment,
            0,
            true,
            "second alternate-screen page-wheel pixel fragment completes one page key");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            pixel_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "alternate-screen page-wheel pixel fragments write after one cell-height step");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1000hlegacy-mouse-alt"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen legacy mouse wheel surface starts");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "legacy non-SGR mouse wheel is accepted by unsupported mouse mode");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "legacy non-SGR mouse wheel writes no alternate fallback input by default");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t page_policy_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "page-policy legacy non-SGR mouse wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            page_policy_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "page-policy legacy non-SGR mouse wheel writes PageUp input");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000h") + numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "primary legacy mouse wheel surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
            snapshot->viewport.scrollback_rows > 0 &&
            snapshot->viewport.offset_from_tail == 0,
            "primary legacy mouse wheel fixture starts at scrollback tail");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "primary legacy non-SGR mouse wheel is terminal-owned");
        const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(wheel_snapshot != nullptr &&
            wheel_snapshot->viewport.offset_from_tail == 0,
            "primary legacy non-SGR mouse wheel does not scroll locally");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "primary legacy non-SGR mouse wheel writes no unsupported bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("\x1b[?1000hlegacy-ready")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "primary no-scrollback legacy mouse wheel surface starts");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "primary no-scrollback legacy non-SGR mouse wheel is terminal-owned");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "primary no-scrollback legacy non-SGR mouse wheel writes no unsupported bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("\x1b[?1000hlegacy-ready")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit legacy non-SGR mouse surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        const std::size_t write_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 1),
            Qt::MiddleButton,
            Qt::MiddleButton,
            Qt::NoModifier,
            false,
            "post-exit legacy non-SGR mouse press is not accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            point_in_grid_cell(fixture.surface, 0, 2),
            Qt::NoButton,
            Qt::MiddleButton,
            Qt::NoModifier,
            false,
            "post-exit legacy non-SGR synthetic drag is not accepted");
        ok &= check(backend_ptr->writes.size() == write_index,
            "post-exit legacy non-SGR mouse events write no backend bytes");
    }

    return ok;
}

bool test_mouse_reporting_surface_events(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006h") + numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse reporting surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(initial_snapshot != nullptr,
            "mouse reporting surface has a render snapshot");
        if (initial_snapshot == nullptr) {
            return ok;
        }

        const int report_row =
            std::min(2, std::max(0, initial_snapshot->grid_size.rows - 1));
        const int report_column =
            std::min(3, std::max(0, initial_snapshot->grid_size.columns - 1));
        const QPointF report_point =
            point_in_grid_cell(fixture.surface, report_row, report_column);
        ok &= check(report_row != 0 || report_column != 0,
            "mouse reporting fixture uses a nonzero grid cell");

        QQuickItem other_item;
        other_item.setParentItem(fixture.window.contentItem());
        other_item.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
            "mouse reporting starts with another item focused");

        const std::size_t click_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            report_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "active mouse press is accepted");
        ok &= check(fixture.surface.hasActiveFocus(),
            "accepted mouse press restores active terminal focus");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            report_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "active mouse release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            click_index,
            {
                sgr_mouse_report(0, report_row, report_column, 'M'),
                sgr_mouse_report(0, report_row, report_column, 'm'),
            },
            "active click and release write SGR bytes");

        fixture.surface.set_mouse_reporting_policy(
            VNM_TerminalSurface::Mouse_reporting_policy::DISABLED);
        const std::size_t disabled_write_count = backend_ptr->writes.size();
        other_item.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
            "disabled mouse reporting focus fixture moves focus away");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            QPointF(1.0, 1.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "disabled mouse reporting routes left press to selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            QPointF(1.0, 1.0),
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "disabled mouse reporting routes left release to selection");
        ok &= check(fixture.surface.hasActiveFocus(),
            "disabled mouse reporting press still restores terminal focus");
        ok &= check(backend_ptr->writes.size() == disabled_write_count,
            "disabled mouse reporting writes nothing");

        fixture.surface.set_mouse_reporting_policy(
            VNM_TerminalSurface::Mouse_reporting_policy::APPLICATION_CONTROLLED);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            QPointF(fixture.surface.width() + 10.0, fixture.surface.height() + 10.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "out-of-bounds mouse press remains unaccepted");
        ok &= check(backend_ptr->writes.size() == disabled_write_count,
            "out-of-bounds mouse press writes nothing");

        ok &= check(initial_snapshot->viewport.offset_from_tail == 0,
            "mouse wheel reporting starts at scrollback tail");
        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            240,
            true,
            "active mouse wheel is accepted by terminal reporting");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_index,
            {
                sgr_mouse_report(64, report_row, report_column, 'M'),
                sgr_mouse_report(64, report_row, report_column, 'M'),
            },
            "active mouse wheel writes one SGR report per completed step");
        const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(wheel_snapshot != nullptr &&
            wheel_snapshot->viewport.offset_from_tail == 0,
            "active mouse wheel does not scroll scrollback");

        const std::size_t small_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "first small mouse wheel delta is retained");
        ok &= check(backend_ptr->writes.size() == small_wheel_index,
            "first small mouse wheel delta writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "second small mouse wheel delta is retained");
        ok &= check(backend_ptr->writes.size() == small_wheel_index,
            "second small mouse wheel delta writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "third small mouse wheel delta completes one report");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            small_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "small mouse wheel deltas write only after a full step");

        const int first_pixel_fragment = std::max(
            1,
            static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
        const int second_pixel_fragment = std::max(
            1,
            static_cast<int>(
                std::ceil(current_cell_metrics(fixture.surface).height -
                    static_cast<qreal>(first_pixel_fragment))));
        const std::size_t pixel_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            first_pixel_fragment,
            0,
            true,
            "first mouse-reporting pixel wheel fragment is retained");
        ok &= check(backend_ptr->writes.size() == pixel_wheel_index,
            "first mouse-reporting pixel wheel fragment writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            second_pixel_fragment,
            0,
            true,
            "second mouse-reporting pixel wheel fragment completes one report");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            pixel_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "mouse-reporting pixel wheel fragments write after one cell-height step");

        const std::size_t mode_boundary_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before mode reset");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "mouse wheel partial step before mode reset writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "pending mouse DECRST is accepted conservatively");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "pending mouse DECRST after partial wheel writes no stale SGR report");
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "drained mouse DECSET starts a fresh wheel remainder after reset");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "drained mouse DECSET after reset does not reuse stale wheel remainder");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "fresh mouse wheel remainder completes after new partials");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            mode_boundary_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "fresh mouse wheel remainder writes only after a new full step");

        const std::size_t collapsed_boundary_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before collapsed mode toggle");
        ok &= check(backend_ptr->writes.size() == collapsed_boundary_wheel_index,
            "mouse wheel partial step before collapsed toggle writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l\x1b[?1000;1006h"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "pending collapsed mouse mode toggle is accepted conservatively");
        ok &= check(backend_ptr->writes.size() == collapsed_boundary_wheel_index,
            "pending collapsed mouse mode toggle writes no stale SGR report");
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "drained collapsed mouse mode toggle starts a fresh wheel remainder");
        ok &= check(backend_ptr->writes.size() == collapsed_boundary_wheel_index,
            "drained collapsed mouse mode toggle does not reuse stale wheel remainder");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "collapsed mouse mode toggle allows new full wheel step");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            collapsed_boundary_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "collapsed mouse mode toggle writes only after a new full step");

        const std::size_t ordinary_output_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before ordinary output");
        ok &= check(backend_ptr->writes.size() == ordinary_output_wheel_index,
            "mouse wheel partial step before ordinary output writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("ordinary-output"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "ordinary output preserves mouse wheel remainder");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            ordinary_output_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "ordinary output snapshot does not clear mouse wheel remainder");

        const qreal       font_size_before_zoom  = fixture.surface.font_size();
        const std::size_t ctrl_wheel_write_count = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::ControlModifier,
            report_point,
            0,
            120,
            true,
            "Ctrl+wheel keeps zoom priority while mouse reporting is active");
        ok &= check(fixture.surface.font_size() == font_size_before_zoom + 1.0,
            "Ctrl+wheel zoom still updates font size");
        ok &= check(backend_ptr->writes.size() == ctrl_wheel_write_count,
            "Ctrl+wheel zoom writes no mouse reporting bytes");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l"));
        const std::size_t pending_reset_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "pending mouse DECRST leaves current wheel terminal-owned");
        ok &= check(backend_ptr->writes.size() == pending_reset_wheel_index,
            "pending mouse DECRST suppresses stale SGR wheel bytes without draining");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::size_t drained_reset_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "drained mouse DECRST affects future wheel routing");
        ok &= check(backend_ptr->writes.size() == drained_reset_wheel_index,
            "future wheel after drained mouse DECRST writes no SGR bytes");
        const std::shared_ptr<const term::Terminal_render_snapshot> drained_reset_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(drained_reset_snapshot != nullptr &&
            drained_reset_snapshot->viewport.offset_from_tail > 0,
            "future wheel after drained mouse DECRST scrolls published primary scrollback");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006h") + numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "detached viewport SGR press surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(tail_snapshot != nullptr &&
            tail_snapshot->viewport.scrollback_rows > 0 &&
            tail_snapshot->viewport.offset_from_tail == 0,
            "detached viewport SGR press fixture starts at tail with scrollback");

        constexpr int detached_offset = 3;
        ok &= check(fixture.surface.scroll_viewport_lines(detached_offset),
            "detached viewport SGR press fixture detaches the viewport");
        const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(detached_snapshot != nullptr &&
            detached_snapshot->viewport.offset_from_tail == detached_offset,
            "detached viewport SGR press fixture publishes detached viewport");

        constexpr int report_row    = 0;
        constexpr int report_column = 1;
        const std::size_t press_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, report_row, report_column),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "detached viewport SGR press is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            press_index,
            { sgr_mouse_report(0, report_row, report_column, 'M') },
            "detached viewport SGR press writes press bytes");

        const std::shared_ptr<const term::Terminal_render_snapshot> post_press_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(post_press_snapshot != nullptr &&
            post_press_snapshot->viewport.offset_from_tail == 0,
            "detached viewport SGR press returns the viewport to tail");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006hsgr-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit SGR mouse surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);
        const std::size_t write_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 1),
            Qt::MiddleButton,
            Qt::MiddleButton,
            Qt::NoModifier,
            false,
            "post-exit SGR mouse press is not accepted");
        ok &= check(backend_ptr->writes.size() == write_index,
            "post-exit SGR mouse press writes no backend bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        observe_backend_error_codes(fixture.surface, error_codes);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006hsgr-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "write-rejected SGR mouse surface starts");

        backend_ptr->reject_writes = true;
        const std::size_t write_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 1),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "write-rejected SGR mouse press is terminal-owned");
        ok &= check(backend_ptr->writes.size() == write_index + 1U,
            "write-rejected SGR mouse press reaches backend write");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_index,
            { sgr_mouse_report(0, 0, 1, 'M') },
            "write-rejected SGR mouse press records attempted press bytes");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "write-rejected SGR mouse press reports one WRITE_FAILED backend_error");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "pending mouse DECSET wheel surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr,
            "pending mouse DECSET wheel surface has a render snapshot");
        if (snapshot == nullptr) {
            return ok;
        }

        const QPointF report_point = point_in_grid_cell(fixture.surface, 0, 0);
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        const std::size_t pending_set_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "pending mouse DECSET does not drain before wheel routing");
        ok &= check(backend_ptr->writes.size() == pending_set_wheel_index,
            "pending mouse DECSET writes no SGR bytes on the current wheel");
        const std::shared_ptr<const term::Terminal_render_snapshot> pending_set_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(pending_set_snapshot != nullptr &&
            pending_set_snapshot->viewport.offset_from_tail > 0,
            "pending mouse DECSET current wheel uses published local scrollback");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::size_t drained_set_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "drained mouse DECSET routes future wheel to SGR reporting");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drained_set_wheel_index,
            { QByteArrayLiteral("\x1b[<64;1;1M") },
            "future wheel after drained mouse DECSET writes SGR reporting");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1000;1006halt-mouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen mouse wheel precedence surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
            "alternate-screen mouse wheel precedence fixture enters alternate screen");

        const QPointF     report_point = point_in_grid_cell(fixture.surface, 0, 0);
        const std::size_t wheel_index  = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "mouse-reporting alternate-screen wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_index,
            { QByteArrayLiteral("\x1b[<64;1;1M") },
            "mouse-reporting alternate-screen wheel prefers SGR over cursor keys");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t page_policy_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "page-policy alternate-screen mouse wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            page_policy_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "page-policy alternate-screen wheel sends PageUp before SGR mouse");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hdrag-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "drag mouse reporting surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr,
            "drag mouse reporting surface has a render snapshot");
        if (snapshot == nullptr) {
            return ok;
        }

        const int report_row =
            std::min(2, std::max(0, snapshot->grid_size.rows - 1));
        const int report_column =
            std::min(3, std::max(0, snapshot->grid_size.columns - 1));
        const QPointF report_point =
            point_in_grid_cell(fixture.surface, report_row, report_column);
        const QPointF outside_point(
            fixture.surface.width() + 10.0,
            fixture.surface.height() + 10.0);

        const std::size_t drag_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            report_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "button-event mouse press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            outside_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "button-event grabbed drag outside the grid is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            outside_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "button-event grabbed release outside the grid is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drag_index,
            {
                sgr_mouse_report(0,  report_row, report_column, 'M'),
                sgr_mouse_report(32, report_row, report_column, 'M'),
                sgr_mouse_report(0,  report_row, report_column, 'm'),
            },
            "button-event grabbed drag and release preserve the last in-grid cell");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1003;1006hmotion-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "all-motion mouse reporting surface starts");

        const std::size_t motion_index = backend_ptr->writes.size();
        ok &= send_hover_move(
            fixture.surface,
            QPointF(1.0, 1.0),
            Qt::NoModifier,
            true,
            "all-motion hover move is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            motion_index,
            { QByteArrayLiteral("\x1b[<35;1;1M") },
            "all-motion hover writes SGR passive-motion bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("wheel-ready")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized wheel mouse reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?1000;1006h"));
        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            false,
            "synchronized hidden mouse-reporting wheel is not consumed from stale state");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "wheel reporting does not use hidden synchronized-output mouse mode");
    }

    return ok;
}

bool test_row_timestamp_tooltip_signal_contract(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    // A stationary pointer can poison this test from outside: the window can
    // spawn under the desktop cursor, and the offscreen platform latches a
    // virtual-cursor enter near the window origin no matter where the window
    // sits. Qt Quick then re-delivers synthetic hover moves at the latched
    // position on every frame with dirty items. The contract under test must
    // ignore exactly that heartbeat, but here it would interleave a second
    // pointer stream with the synthesized one. Flush the spawn-time hover
    // state, park the window away from the cursor, and inset the surface so
    // a latched origin-area point cannot land on it.
    fixture.window.hide();
    pump_events(app);
    const QRect  screen_geometry = fixture.window.screen()->geometry();
    const QPoint cursor_position = QCursor::pos();
    fixture.window.setPosition(
        cursor_position.x() < screen_geometry.center().x()
            ? screen_geometry.right() - fixture.window.width()
            : screen_geometry.left(),
        cursor_position.y() < screen_geometry.center().y()
            ? screen_geometry.bottom() - fixture.window.height()
            : screen_geometry.top());
    fixture.surface.setPosition(QPointF(64.0, 64.0));
    fixture.window.show();
    pump_events(app);

    ok &= check(fixture.surface.row_timestamp_tooltip_enabled(),
        "row timestamp tooltip is enabled by default");

    int       requested_count     = 0;
    int       dismissed_count     = 0;
    int       enabled_change_count = 0;
    qreal     requested_x         = -1.0;
    qreal     requested_y         = -1.0;
    QDateTime requested_timestamp;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_requested,
        &fixture.surface,
        [&](qreal x, qreal y, const QDateTime& timestamp) {
            ++requested_count;
            requested_x         = x;
            requested_y         = y;
            requested_timestamp = timestamp;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_dismissed,
        &fixture.surface,
        [&dismissed_count] {
            ++dismissed_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_enabled_changed,
        &fixture.surface,
        [&enabled_change_count] {
            ++enabled_change_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("stamped output")};

    const qint64 before_output_ms = QDateTime::currentMSecsSinceEpoch();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "row timestamp tooltip fixture starts scripted backend");
    pump_events(app);

    const QPointF stamped_point = point_in_grid_cell(fixture.surface, 0, 2);
    ok &= send_hover_move(fixture.surface, stamped_point, Qt::NoModifier, false,
        "hover move over the stamped row is not consumed");
    ok &= check(
        pump_until(app, [&requested_count] { return requested_count == 1; }, 300),
        "hover idle over a stamped row requests the timestamp tooltip");
    const qint64 after_request_ms = QDateTime::currentMSecsSinceEpoch();
    ok &= check(dismissed_count == 0,
        "tooltip request alone emits no dismissal");
    ok &= check(
        nearly_equal(requested_x, stamped_point.x()) &&
        nearly_equal(requested_y, stamped_point.y()),
        "tooltip request reports the resting pointer position");
    ok &= check(
        requested_timestamp.isValid()                              &&
        requested_timestamp.toMSecsSinceEpoch() >= before_output_ms &&
        requested_timestamp.toMSecsSinceEpoch() <= after_request_ms,
        "tooltip request carries the row's output wall-clock timestamp");

    // Qt Quick re-delivers a hover move at the unchanged cursor position
    // whenever scene content changes under a stationary pointer, and the
    // shown tooltip itself causes such a change. The redelivery is not user
    // activity: it must neither dismiss the tooltip nor restart the idle
    // timer (a restart would re-fire the request without a dismissal).
    ok &= send_hover_move(fixture.surface, stamped_point, Qt::NoModifier, false,
        "same-position hover redelivery is not consumed");
    ok &= check(dismissed_count == 0,
        "same-position hover redelivery does not dismiss the shown tooltip");
    pump_for(app, 1300);
    ok &= check(requested_count == 1 && dismissed_count == 0,
        "same-position hover redelivery does not restart the idle timer");

    // Snapshot publication with an unchanged viewport is the per-frame sync
    // path while output overwrites the current row; it is not user activity.
    const std::shared_ptr<const term::Terminal_render_snapshot> pre_publication_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    backend_ptr->emit_output(QByteArrayLiteral(" overwritten in place"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check(
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface) !=
            pre_publication_snapshot,
        "unchanged-viewport output publishes a fresh snapshot");
    ok &= check(dismissed_count == 0,
        "snapshot publication with an unchanged viewport does not dismiss");

    // The dismissal must arrive synchronously with the next real pointer
    // motion (one logical pixel is enough), and a press must both stay silent
    // (nothing shown anymore) and stop the re-armed idle timer.
    const QPointF nudged_point = stamped_point + QPointF(1.0, 0.0);
    ok &= send_hover_move(fixture.surface, nudged_point, Qt::NoModifier, false,
        "one-pixel hover move is not consumed");
    ok &= check(dismissed_count == 1,
        "one-pixel pointer motion dismisses the shown tooltip");
    {
        QMouseEvent press(
            QEvent::MouseButtonPress,
            nudged_point, nudged_point, nudged_point,
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.surface, &press);
        QMouseEvent release(
            QEvent::MouseButtonRelease,
            nudged_point, nudged_point, nudged_point,
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.surface, &release);
    }
    ok &= check(dismissed_count == 1,
        "press and release after a dismissal stay silent");
    pump_for(app, 1300);
    ok &= check(requested_count == 1,
        "press cancels the re-armed hover idle timer");

    ok &= send_hover_move(
        fixture.surface,
        point_in_grid_cell(fixture.surface, 3, 2),
        Qt::NoModifier,
        false,
        "hover move over a blank row is not consumed");
    pump_for(app, 1300);
    ok &= check(requested_count == 1,
        "hover idle over a never-written row requests no tooltip");

    ok &= send_hover_move(fixture.surface, stamped_point, Qt::NoModifier, false,
        "hover move back to the stamped row is not consumed");
    ok &= check(
        pump_until(app, [&requested_count] { return requested_count == 2; }, 300),
        "hover idle re-requests the tooltip after activity");

    // Streaming output that only grows the scrollback row count publishes a
    // changed viewport without any user activity; the tooltip must survive
    // it. A real scroll then moves different rows under the resting pointer
    // and must dismiss through the same viewport publication path.
    backend_ptr->emit_output(numbered_scroll_lines(40));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check(fixture.surface.scrollback_rows() > 0,
        "streaming output grows the scrollback row count");
    ok &= check(dismissed_count == 1,
        "scrollback growth at the tail does not dismiss the shown tooltip");

    ok &= check(fixture.surface.scroll_viewport_lines(3),
        "viewport scroll request is accepted");
    ok &= check(
        pump_until(app, [&dismissed_count] { return dismissed_count == 2; }, 300),
        "a real viewport scroll dismisses the shown tooltip");

    const QPointF scrolled_point = point_in_grid_cell(fixture.surface, 2, 2);
    ok &= send_hover_move(fixture.surface, scrolled_point, Qt::NoModifier, false,
        "hover move over a scrolled stamped row is not consumed");
    ok &= check(
        pump_until(app, [&requested_count] { return requested_count == 3; }, 300),
        "hover idle re-requests the tooltip after the scroll");
    {
        QWheelEvent wheel(
            scrolled_point,
            scrolled_point,
            QPoint(0, 0),
            QPoint(0, -120),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false);
        QCoreApplication::sendEvent(&fixture.surface, &wheel);
    }
    ok &= check(dismissed_count == 3,
        "wheel input dismisses the shown tooltip");

    ok &= send_hover_move(
        fixture.surface,
        point_in_grid_cell(fixture.surface, 1, 2),
        Qt::NoModifier,
        false,
        "hover move before disabling the tooltip is not consumed");
    ok &= check(
        pump_until(app, [&requested_count] { return requested_count == 4; }, 300),
        "hover idle requests the tooltip before the property turns off");
    fixture.surface.set_row_timestamp_tooltip_enabled(false);
    ok &= check(dismissed_count == 4 && enabled_change_count == 1,
        "disabling the property dismisses the shown tooltip and notifies");
    ok &= check(!fixture.surface.row_timestamp_tooltip_enabled(),
        "property reads back as disabled");

    ok &= send_hover_move(
        fixture.surface,
        point_in_grid_cell(fixture.surface, 1, 4),
        Qt::NoModifier,
        false,
        "hover move while disabled is not consumed");
    pump_for(app, 1300);
    ok &= check(requested_count == 4,
        "hover idle requests no tooltip while disabled");

    fixture.surface.set_row_timestamp_tooltip_enabled(true);
    ok &= check(enabled_change_count == 2 && dismissed_count == 4,
        "re-enabling notifies without a spurious dismissal");

    return ok;
}

bool test_local_first_wheel_scroll_keeps_callbacks_queued_without_backend_drain(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    fixture.surface.set_wheel_event_policy(
        VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "local-first post-barrier callback surface starts");
    if (!started) {
        return ok;
    }

    bool post_barrier_output_queued = false;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            if (post_barrier_output_queued) {
                return;
            }

            post_barrier_output_queued = true;
            (void)backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049hpost-barrier-alt"));
        });

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;post-barrier-title\a"));
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "local-first wheel scroll with post-barrier callback is accepted");
    ok &= check(!post_barrier_output_queued,
        "local-first wheel leaves pre-existing backend callback queued");

    const std::shared_ptr<const term::Terminal_render_snapshot> post_wheel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_wheel_snapshot != nullptr &&
        post_wheel_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "local-first post-barrier callback stays queued until owner drain");
    ok &= check(post_wheel_snapshot != nullptr &&
        post_wheel_snapshot->viewport.offset_from_tail > 0,
        "local-first wheel still applies primary scrollback movement");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    ok &= check(post_barrier_output_queued,
        "normal drain processes pre-existing callback and queues follow-up output");
    const std::shared_ptr<const term::Terminal_render_snapshot> first_drained_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(first_drained_snapshot != nullptr &&
        first_drained_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "follow-up output from notification callback waits for a later owner drain");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> second_drained_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(second_drained_snapshot != nullptr &&
        second_drained_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "local-first queued post-barrier callback is applied by the later drain");

    return ok;
}

bool test_wheel_input_stops_after_post_barrier_callbacks_become_pending(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_alternate_screen_wheel_policy(
        VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1049halternate-ready"),
    };
    backend->outputs_during_write = {
        QByteArrayLiteral("\x1b[?1049lpost-wheel-output\r\n"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "wheel post-barrier callback surface starts");
    if (!started) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(initial_snapshot != nullptr &&
        initial_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "wheel post-barrier callback fixture starts on alternate screen");

    const std::size_t wheel_write_index = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        240,
        true,
        "alternate-screen page wheel accepts multi-step input");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        wheel_write_index,
        { QByteArrayLiteral("\x1b[5~") },
        "alternate-screen page wheel stops after callback becomes pending");

    const std::shared_ptr<const term::Terminal_render_snapshot> post_wheel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_wheel_snapshot != nullptr &&
        post_wheel_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "post-barrier backend callback stays queued until owner drain");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> drained_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(drained_snapshot != nullptr &&
        drained_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "queued post-barrier backend callback is applied by the next drain");

    return ok;
}

bool test_wheel_mouse_reporting_stops_after_post_barrier_callbacks_become_pending(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1000;1006hmouse-ready"),
    };
    backend->outputs_during_write = {
        QByteArrayLiteral("\x1b[?1049hpost-mouse-alt"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "mouse wheel post-barrier callback surface starts");
    if (!started) {
        return ok;
    }

    const int report_row       = 0;
    const int report_column    = 0;
    const QPointF report_point = point_in_grid_cell(fixture.surface, report_row, report_column);

    const std::size_t wheel_write_index = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        report_point,
        0,
        240,
        true,
        "mouse-reporting wheel accepts multi-step input");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        wheel_write_index,
        { sgr_mouse_report(64, report_row, report_column, 'M') },
        "mouse-reporting wheel stops after callback becomes pending");

    const std::shared_ptr<const term::Terminal_render_snapshot> post_wheel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_wheel_snapshot != nullptr &&
        post_wheel_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "mouse wheel post-barrier callback stays queued until owner drain");

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> drained_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(drained_snapshot != nullptr &&
        drained_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "mouse wheel queued post-barrier callback is applied by the next drain");

    return ok;
}

bool test_mouse_press_ignores_hidden_pending_mouse_enable(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("visible-no-mouse")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "hidden mouse-enable press surface starts");
    if (!started) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> visible_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(visible_snapshot != nullptr &&
        visible_snapshot->modes.mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "hidden mouse-enable press starts from a non-mouse published frame");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?1000;1006h"));
    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        point_in_grid_cell(fixture.surface, 0, 0),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        false,
        "hidden mouse-enable press is not consumed from hidden live mode");
    ok &= check(backend_ptr->writes.size() == write_count,
        "hidden mouse-enable press writes no stale SGR report");

    const std::shared_ptr<const term::Terminal_render_snapshot> post_press_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_press_snapshot != nullptr &&
        post_press_snapshot->modes.mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "hidden mouse-enable press keeps the visible frame non-mouse");

    return ok;
}

bool test_mouse_release_pending_report_uses_published_modes(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1000;1006hvisible-mouse"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "hidden mouse-disable release surface starts");
    if (!started) {
        return ok;
    }

    constexpr int report_row    = 0;
    constexpr int report_column = 4;
    const QPointF point = point_in_grid_cell(
        fixture.surface,
        report_row,
        report_column);
    const term::terminal_cell_metrics_t original_metrics =
        current_cell_metrics(fixture.surface);
    const std::size_t press_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "hidden mouse-disable release setup press is accepted");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        press_index,
        { sgr_mouse_report(0, report_row, report_column, 'M') },
        "hidden mouse-disable release setup writes one press");

    int title_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
            if (title_changed_count == 1) {
                (void)backend_ptr->emit_output(
                    QByteArrayLiteral(
                        "\x1b]0;deferred-release-basis\a"
                        "\x1b[?1000;1006l"));
                return;
            }
            if (title_changed_count == 2) {
                fixture.surface.set_font_size(fixture.surface.font_size() * 2.0);
            }
        });

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;entry-release-basis\a"));
    const std::size_t release_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "hidden mouse-disable release is delivered after pending callback");
    const term::terminal_cell_metrics_t current_metrics =
        current_cell_metrics(fixture.surface);
    ok &= check(current_metrics.width > original_metrics.width,
        "deferred release callback changes the live coordinate basis");
    const int recomputed_column =
        static_cast<int>(std::floor(point.x() / current_metrics.width));
    ok &= check(recomputed_column != report_column,
        "deferred release would use a different column without recompute");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        release_index,
        { sgr_mouse_report(0, report_row, report_column, 'm') },
        "deferred release writes exactly one published-frame release");
    ok &= check(title_changed_count == 2,
        "deferred release drains the post-entry backend callback");

    const std::shared_ptr<const term::Terminal_render_snapshot> post_release_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_release_snapshot != nullptr &&
        post_release_snapshot->modes.mouse_tracking == term::Terminal_mouse_tracking_mode::NONE &&
        !post_release_snapshot->modes.sgr_mouse_encoding,
        "deferred release survives the published mouse disable");

    return ok;
}

bool test_mouse_press_release_pending_callbacks_clear_grab(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1002;1006hvisible-button-event"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "pending mouse pair surface starts");
    if (!started) {
        return ok;
    }

    int post_entry_output_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++post_entry_output_count;
            (void)backend_ptr->emit_output(QByteArrayLiteral("post-entry-callback"));
        });

    const QPointF point = point_in_grid_cell(fixture.surface, 0, 1);
    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;press-deferral\a"));
    const std::size_t write_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "pending mouse pair press is accepted");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;release-deferral\a"));
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "pending mouse pair release is accepted");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        write_index,
        {
            sgr_mouse_report(0, 0, 1, 'M'),
            sgr_mouse_report(0, 0, 1, 'm'),
        },
        "pending mouse pair writes one press and one release");
    ok &= check(post_entry_output_count == 2,
        "pending mouse pair exercises deferred press and release writes");

    const std::size_t probe_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        QPointF(fixture.surface.width() + 10.0, fixture.surface.height() + 10.0),
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        false,
        "post-release synthetic drag is not accepted after grab clears");
    ok &= check(backend_ptr->writes.size() == probe_index,
        "post-release synthetic drag writes no stuck-grab report");

    return ok;
}

bool test_pending_mouse_report_preserves_following_key_input(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006hsingle-pending-mouse"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "single pending mouse/key preservation surface starts");
        if (!started) {
            return ok;
        }

        term::VNM_TerminalSurface_render_bridge::
            set_pending_published_mouse_report_block_count_for_testing(
                fixture.surface,
                2);
        const QPointF point = point_in_grid_cell(fixture.surface, 0, 1);
        const std::size_t write_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "single pending mouse/key preservation press is accepted");
        ok &= check(backend_ptr->writes.size() == write_index,
            "single pending mouse/key preservation press remains pending while blocked");

        ok &= send_key(
            fixture.surface,
            Qt::Key_X,
            Qt::NoModifier,
            QStringLiteral("x"),
            "single pending mouse/key preservation key is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_index,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                QByteArrayLiteral("x"),
            },
            "single pending mouse/key preservation writes mouse before following key");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006hmulti-pending-mouse"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "multi pending mouse/key preservation surface starts");
        if (!started) {
            return ok;
        }

        term::VNM_TerminalSurface_render_bridge::
            set_pending_published_mouse_report_block_count_for_testing(
                fixture.surface,
                5);
        const QPointF point = point_in_grid_cell(fixture.surface, 0, 1);
        const std::size_t write_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi pending mouse/key preservation press is accepted");
        ok &= check(backend_ptr->writes.size() == write_index,
            "multi pending mouse/key preservation press remains pending while blocked");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi pending mouse/key preservation release is accepted");
        ok &= check(backend_ptr->writes.size() == write_index,
            "multi pending mouse/key preservation release remains pending while blocked");

        ok &= send_key(
            fixture.surface,
            Qt::Key_X,
            Qt::NoModifier,
            QStringLiteral("x"),
            "multi pending mouse/key preservation key is accepted while mouse is blocked");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_index,
            { QByteArrayLiteral("x") },
            "multi pending mouse/key preservation cancels the stale mouse queue and writes the key");
    }

    return ok;
}

bool test_mouse_passive_motion_preserves_detached_viewport(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1003;1006h") + numbered_scroll_lines(80),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "detached viewport hover mouse surface starts");
    if (!started) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr &&
        tail_snapshot->modes.mouse_tracking == term::Terminal_mouse_tracking_mode::ANY &&
        tail_snapshot->modes.sgr_mouse_encoding &&
        tail_snapshot->viewport.scrollback_rows > 0 &&
        tail_snapshot->viewport.offset_from_tail == 0,
        "detached viewport hover fixture starts with all-motion mouse reporting");

    constexpr int detached_offset = 3;
    ok &= check(fixture.surface.scroll_viewport_lines(detached_offset),
        "detached viewport hover fixture detaches the viewport");
    const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == detached_offset,
        "detached viewport hover fixture publishes detached viewport");

    constexpr int report_row    = 0;
    constexpr int report_column = 4;
    const std::size_t hover_index = backend_ptr->writes.size();
    ok &= send_hover_move(
        fixture.surface,
        point_in_grid_cell(fixture.surface, report_row, report_column),
        Qt::NoModifier,
        true,
        "all-motion hover over detached viewport is accepted");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        hover_index,
        { sgr_mouse_report(35, report_row, report_column, 'M') },
        "all-motion hover writes one passive move report");

    const std::shared_ptr<const term::Terminal_render_snapshot> hover_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(hover_snapshot != nullptr &&
        hover_snapshot->viewport.offset_from_tail == detached_offset,
        "all-motion hover preserves the detached viewport");

    const std::size_t move_index = backend_ptr->writes.size();
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        point_in_grid_cell(fixture.surface, report_row, report_column + 1),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "all-motion passive mouse move over detached viewport is accepted");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        move_index,
        { sgr_mouse_report(35, report_row, report_column + 1, 'M') },
        "all-motion passive mouse move writes one passive move report");

    const std::shared_ptr<const term::Terminal_render_snapshot> move_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(move_snapshot != nullptr &&
        move_snapshot->viewport.offset_from_tail == detached_offset,
        "all-motion passive mouse move preserves the detached viewport");

    return ok;
}

bool test_local_first_wheel_trace_records_ingress_before_route(QGuiApplication& app)
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    bool ok = true;

    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "local-first wheel trace temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }

    const QString transcript_path =
        transcript_dir.filePath(QStringLiteral("local_first_wheel.ndjson"));
    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        fixture.surface.set_transcript_capture_path(transcript_path);
        fixture.surface.set_wheel_trace_enabled(true);
        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        (void)start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "local-first no-drain wheel surface starts");
        if (!started) {
            return ok;
        }

        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "local-first traced wheel scroll is accepted");
    }

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
    ok &= check(events.has_value(), "local-first wheel trace transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }

    const std::optional<term::Terminal_transcript_event> wheel_trace =
        first_transcript_event(*events, QStringLiteral("surface.wheel_trace"));
    ok &= check(wheel_trace.has_value(), "local-first wheel trace is present");
    const std::optional<term::Terminal_transcript_event> wheel_ingress =
        first_transcript_event(*events, QStringLiteral("surface.wheel_ingress"));
    ok &= check(wheel_ingress.has_value(), "local-first wheel ingress trace is present");
    if (wheel_ingress.has_value() && wheel_trace.has_value()) {
        ok &= check(wheel_ingress->event_index < wheel_trace->event_index,
            "wheel ingress trace precedes routed wheel trace");
    }
    return ok;
#else
    (void)app;
    return true;
#endif
}

bool test_local_first_wheel_scroll_applies_during_synchronized_output_block(
    QGuiApplication& app)
{
    bool ok = true;

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "synchronized-output wheel trace temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }

    const QString transcript_path =
        transcript_dir.filePath(QStringLiteral("synchronized_output_blocked_wheel.ndjson"));
#endif

    {
        constexpr int k_detached_offset_from_tail = 6;
        constexpr int k_blocked_wheel_event_count = 2;
        constexpr int k_expected_wheel_delta =
            k_blocked_wheel_event_count * 3;

        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
        fixture.surface.set_transcript_capture_path(transcript_path);
        fixture.surface.set_wheel_trace_enabled(true);
#endif
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized-output blocked wheel surface starts");
        if (!started) {
            return ok;
        }

        ok &= check(fixture.surface.scroll_to_offset_from_tail(k_detached_offset_from_tail),
            "synchronized-output blocked wheel setup detaches viewport from tail");
        const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(detached_snapshot != nullptr &&
            !detached_snapshot->viewport.follow_tail &&
            detached_snapshot->viewport.offset_from_tail == k_detached_offset_from_tail,
            "synchronized-output blocked wheel setup publishes detached viewport");
        const std::uint64_t blocked_sequence =
            detached_snapshot != nullptr ? detached_snapshot->metadata.sequence : 0U;

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hblocked synchronized output"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);

        const std::shared_ptr<const term::Terminal_render_snapshot> blocked_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(blocked_snapshot != nullptr &&
            !blocked_snapshot->viewport.follow_tail &&
            blocked_snapshot->viewport.offset_from_tail == k_detached_offset_from_tail,
            "synchronized-output block leaves detached publication in place before wheel");
        ok &= check(blocked_snapshot != nullptr &&
            blocked_snapshot->metadata.sequence == blocked_sequence,
            "synchronized-output block does not publish hidden output before wheel");

        const std::size_t backend_write_count = backend_ptr->writes.size();
        for (int i = 0; i < k_blocked_wheel_event_count; ++i) {
            ok &= send_wheel_event(
                fixture.surface,
                Qt::NoModifier,
                120,
                true,
                "local-first wheel during synchronized-output block is accepted");
        }
        ok &= check(backend_ptr->writes.size() == backend_write_count,
            "local-first wheel during synchronized-output block writes no backend input");

        const std::shared_ptr<const term::Terminal_render_snapshot> post_wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(post_wheel_snapshot != nullptr &&
            blocked_snapshot != nullptr &&
            !post_wheel_snapshot->viewport.follow_tail &&
            post_wheel_snapshot->viewport.offset_from_tail ==
                blocked_snapshot->viewport.offset_from_tail,
            "local-first wheel defers publication while synchronized output is hidden");
        ok &= check(post_wheel_snapshot != nullptr &&
            post_wheel_snapshot->metadata.sequence == blocked_sequence,
            "local-first wheel does not publish a snapshot while synchronized output is hidden");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> released_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(released_snapshot != nullptr &&
            blocked_snapshot != nullptr &&
            !released_snapshot->viewport.follow_tail &&
            released_snapshot->viewport.offset_from_tail ==
                blocked_snapshot->viewport.offset_from_tail + k_expected_wheel_delta,
            "synchronized-output release publishes accumulated deferred local wheel scroll");
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
    ok &= check(events.has_value(), "synchronized-output blocked wheel transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }

    const std::optional<term::Terminal_transcript_event> wheel_ingress =
        last_transcript_event(*events, QStringLiteral("surface.wheel_ingress"));
    ok &= check(wheel_ingress.has_value(),
        "synchronized-output blocked wheel ingress trace is present");

    const std::optional<term::Terminal_transcript_event> wheel_trace =
        last_transcript_event(*events, QStringLiteral("surface.wheel_trace"));
    ok &= check(wheel_trace.has_value(),
        "synchronized-output blocked wheel trace is present");

    if (wheel_ingress.has_value() && wheel_trace.has_value()) {
        ok &= check(wheel_ingress->event_index < wheel_trace->event_index,
            "synchronized-output blocked wheel ingress precedes route trace");
        ok &= check(
            transcript_has_event_after(
                *events,
                QStringLiteral("surface.scroll"),
                *wheel_ingress),
            "synchronized-output blocked wheel records a surface.scroll event");
    }

    if (wheel_trace.has_value()) {
        const QJsonObject& object = wheel_trace->object;
        ok &= check(object.value(QStringLiteral("source")).toString() ==
            QStringLiteral("surface.text_area.wheel"),
            "synchronized-output blocked wheel trace records text-area source");
        ok &= check(object.value(QStringLiteral("route")).toString() ==
            QStringLiteral("local_scroll"),
            "synchronized-output blocked wheel trace routes to local scroll");
        ok &= check(object.value(QStringLiteral("render_publication_blocked")).toBool(),
            "synchronized-output blocked wheel trace marks blocked render publication");
        ok &= check(object.value(QStringLiteral("local_scroll_possible")).toBool(),
            "synchronized-output blocked wheel trace marks local scroll possible");
        ok &= check(object.value(QStringLiteral("local_scroll_applied")).toBool(),
            "synchronized-output blocked wheel trace marks local scroll applied");
        ok &= check(!object.value(QStringLiteral("visible_scroll_applied")).toBool(true),
            "synchronized-output blocked wheel trace marks visible scroll deferred");
        ok &= check(object.value(QStringLiteral("scroll_action")).toString() ==
            QStringLiteral("viewport_moved"),
            "synchronized-output blocked wheel trace records viewport movement");
        ok &= check(object.value(QStringLiteral("local_scroll_block_reason")).toString() !=
            QStringLiteral("synchronized_output_deferred"),
            "synchronized-output blocked wheel trace does not block local scroll");
        ok &= check(object.value(QStringLiteral("outcome")).toString() ==
            QStringLiteral("local_scroll_publication_deferred"),
            "synchronized-output blocked wheel trace reports deferred publication");
        ok &= check(object.value(QStringLiteral("backend_drain_calls")).toInt(-1) == 0,
            "synchronized-output blocked wheel trace records no backend drain");
    }
    const std::optional<term::Terminal_transcript_event> surface_scroll =
        last_transcript_event(*events, QStringLiteral("surface.scroll"));
    ok &= check(surface_scroll.has_value(),
        "synchronized-output blocked wheel records surface scroll transcript");
#endif

    return ok;
}

bool test_transcript_timing_diagnostics_records_hot_paths()
{
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    bool ok = true;

    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "transcript timing temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }

    const QByteArray hot_payload(8 * 1024 * 1024, 'x');
    const QString disabled_path =
        transcript_dir.filePath(QStringLiteral("timing_disabled.ndjson"));
    {
        QString error;
        std::shared_ptr<term::Terminal_transcript_recorder> recorder =
            term::Terminal_transcript_recorder::create(disabled_path, false, false, &error);
        ok &= check(recorder != nullptr, "disabled transcript timing recorder opens");
        if (recorder != nullptr) {
            ok &= check(
                recorder->record_backend_output(1U, QByteArrayView(hot_payload)),
                "disabled transcript timing recorder writes hot backend output");
        }
    }

    QString error;
    std::optional<std::vector<term::Terminal_transcript_event>> disabled_events =
        term::read_terminal_transcript(disabled_path, &error);
    ok &= check(disabled_events.has_value(), "disabled transcript timing transcript parses");
    if (!disabled_events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }
    ok &= check(
        !first_transcript_event(*disabled_events, QStringLiteral("transcript.timing")).has_value(),
        "transcript timing records are absent when timing diagnostics are disabled");

    const QString enabled_path =
        transcript_dir.filePath(QStringLiteral("timing_enabled.ndjson"));
    {
        QString enabled_error;
        std::shared_ptr<term::Terminal_transcript_recorder> recorder =
            term::Terminal_transcript_recorder::create(enabled_path, false, true, &enabled_error);
        ok &= check(recorder != nullptr, "enabled transcript timing recorder opens");
        if (recorder != nullptr) {
            ok &= check(
                recorder->record_backend_output(1U, QByteArrayView(hot_payload)),
                "enabled transcript timing recorder writes hot backend output");
        }
    }

    std::optional<std::vector<term::Terminal_transcript_event>> enabled_events =
        term::read_terminal_transcript(enabled_path, &error);
    ok &= check(enabled_events.has_value(), "enabled transcript timing transcript parses");
    if (!enabled_events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }

    std::optional<term::Terminal_transcript_event> backend_timing;
    for (const term::Terminal_transcript_event& event : *enabled_events) {
        if (event.kind == QStringLiteral("transcript.timing") &&
            event.object.value(QStringLiteral("record_kind")).toString() ==
                QStringLiteral("backend.output"))
        {
            backend_timing = event;
            break;
        }
    }

    ok &= check(backend_timing.has_value(),
        "enabled transcript timing records hot backend output");
    if (backend_timing.has_value()) {
        ok &= check(
            static_cast<qint64>(
                backend_timing->object.value(QStringLiteral("payload_byte_count")).toDouble(-1.0)) ==
                hot_payload.size(),
            "backend output timing records payload byte count");
        ok &= check(
            static_cast<qint64>(
                backend_timing->object.value(QStringLiteral("ndjson_byte_count")).toDouble(0.0)) >
                hot_payload.size(),
            "backend output timing records serialized byte count");
        ok &= check(
            static_cast<qint64>(
                backend_timing->object.value(QStringLiteral("total_elapsed_ns")).toDouble(0.0)) > 0,
            "backend output timing records elapsed time");
    }

    return ok;
#else
    return true;
#endif
}

bool test_selection_drag_and_selected_text(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "selection surface starts");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "selected_text is empty before selection");

        int selection_changed_count = 0;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::selection_changed,
            &fixture.surface,
            [&selection_changed_count] {
                ++selection_changed_count;
            });

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "plain left press starts selection");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "plain left press alone does not activate selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "plain left drag extends selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "plain left release finishes selection");

        const std::shared_ptr<const term::Terminal_render_snapshot> selection_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count,
            "plain selection drag writes no mouse reporting bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "plain selection drag activates public selection state");
        ok &= check_int_equal(
            selection_changed_count,
            1,
            "plain selection drag emits selection_changed for the ACTIVE transition");
        ok &= check(selection_snapshot != nullptr &&
            selection_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*selection_snapshot, 0, 1, 4),
            "plain selection drag publishes a visible selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "plain selection drag exposes selected_text");

        const QPointF replacement_start = point_in_grid_cell(fixture.surface, 1, 0);
        const QPointF replacement_end   = point_in_grid_cell(fixture.surface, 1, 1);
        ok                             &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "new left press starts replacement selection");
        ok                             &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "new left release finishes replacement selection");

        const std::shared_ptr<const term::Terminal_render_snapshot> replaced_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_int_equal(
            selection_changed_count,
            3,
            "replacement selection emits clear and activate selection_changed transitions");
        ok &= check(replaced_snapshot != nullptr &&
            replaced_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*replaced_snapshot, 1, 0, 2),
            "new selection replaces old selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("be"),
            "new selection replaces old selected text");

        const QPointF single_click_point = point_in_grid_cell(fixture.surface, 0, 0);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            single_click_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "single left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            single_click_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "single left release is accepted");

        const std::shared_ptr<const term::Terminal_render_snapshot> click_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_int_equal(
            selection_changed_count,
            4,
            "single left click emits selection_changed for the clear transition");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "single left click clears selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "single left click leaves selected_text empty");
        ok &= check(click_snapshot != nullptr && click_snapshot->selection_spans.empty(),
            "single left click publishes no selection spans");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "clear_selection fixture starts another selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "clear_selection fixture finishes another selection");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE &&
            !fixture.surface.selected_text().isEmpty(),
            "clear_selection fixture has an active selection before clearing");
        ok &= check_int_equal(
            selection_changed_count,
            5,
            "press-release replacement emits selection_changed for reactivation");
        fixture.surface.clear_selection();
        const std::shared_ptr<const term::Terminal_render_snapshot> cleared_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "clear_selection clears public selection state");
        ok &= check_int_equal(
            selection_changed_count,
            6,
            "clear_selection emits selection_changed for the NONE transition");
        ok &= check(cleared_snapshot != nullptr && cleared_snapshot->selection_spans.empty(),
            "clear_selection publishes a snapshot without selection spans");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            replacement_end,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-row selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> multi_row_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int multi_row_tail_columns =
            multi_row_snapshot != nullptr ? multi_row_snapshot->grid_size.columns - 1 : 0;
        ok &= check(multi_row_snapshot != nullptr &&
            multi_row_snapshot->selection_spans.size() == 2U &&
            snapshot_has_selection_span(
                *multi_row_snapshot, 0, 1, multi_row_tail_columns) &&
            snapshot_has_selection_span(*multi_row_snapshot, 1, 0, 2),
            "surface multi-row drag publishes row-boundary selection spans");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha\nbe"),
            "surface multi-row drag exposes selected text across rows");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            end_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            start_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> reverse_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(reverse_snapshot != nullptr &&
            reverse_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*reverse_snapshot, 0, 1, 4),
            "same-row reverse drag publishes the expected selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "same-row reverse drag exposes selected text");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_end,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            start_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> reverse_multi_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(reverse_multi_snapshot != nullptr &&
            reverse_multi_snapshot->selection_spans.size() == 2U &&
            snapshot_has_selection_span(
                *reverse_multi_snapshot, 0, 1, multi_row_tail_columns) &&
            snapshot_has_selection_span(*reverse_multi_snapshot, 1, 0, 2),
            "multi-row reverse drag publishes row-boundary selection spans");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha\nbe"),
            "multi-row reverse drag exposes selected text across rows");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "buffer-switch selection surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "buffer-switch selection press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049halt"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "buffer-switch selection drag is cancelled");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "buffer switch during selection drag leaves no active selection");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "buffer switch during selection drag leaves no selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse-reporting selection suppression surface starts");

        const QPointF     point       = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     drag_point  = point_in_grid_cell(fixture.surface, 0, 3);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            drag_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            drag_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left release is accepted");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(32, 0, 3, 'M'),
                sgr_mouse_report(0, 0, 3, 'm'),
            },
            "mouse-reporting drag writes SGR bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "mouse-reporting drag keeps public selection state empty");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "mouse-reporting drag does not start selection text");
        ok &= check(snapshot != nullptr && snapshot->selection_spans.empty(),
            "mouse-reporting drag does not publish selection spans");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "multi-button mouse-reporting surface starts");

        const QPointF     left_point  = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     right_point = point_in_grid_cell(fixture.surface, 0, 2);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            left_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-button left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            right_point,
            Qt::RightButton,
            Qt::LeftButton | Qt::RightButton,
            Qt::NoModifier,
            true,
            "multi-button right press is accepted while left remains down");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            left_point,
            Qt::LeftButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "multi-button left release is accepted while right remains down");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            right_point,
            Qt::RightButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-button right final release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(2, 0, 2, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
                sgr_mouse_report(2, 0, 2, 'm'),
            },
            "multi-button reporting preserves per-button press/release pairing");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "unhandled mouse move position surface starts");

        const QPointF press_point         = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF disabled_move_point = point_in_grid_cell(fixture.surface, 0, 3);
        const QPointF out_of_bounds_point =
            QPointF(fixture.surface.width() + 10.0, fixture.surface.height() + 10.0);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            press_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "unhandled-position press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1002;1006l"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            disabled_move_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "disabled-mode mouse move is unhandled");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1002;1006h"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            out_of_bounds_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "reenabled out-of-bounds release uses last handled position");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
            },
            "unhandled mouse move does not replace last reported position");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse-reporting clears existing selection surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "pre-reporting drag creates a selection");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting takeover press is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            { sgr_mouse_report(0, 0, 1, 'M') },
            "mouse-reporting takeover writes the SGR press");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "mouse-reporting takeover clears existing selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "mouse-reporting takeover clears selected text");
        ok &= check(snapshot != nullptr && snapshot->selection_spans.empty(),
            "mouse-reporting takeover clears selection spans");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "mouse-reporting takeover release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
            },
            "mouse-reporting takeover preserves press/release pairing");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1003;1006halpha\r\nbeta"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "shift-selection mouse-reporting surface starts");

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "shift left press starts local selection under mouse reporting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "shift left drag extends local selection under mouse reporting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::ShiftModifier,
            true,
            "shift left release finishes local selection under mouse reporting");
        ok &= check(backend_ptr->writes.size() == write_count,
            "shift local selection writes no mouse reporting bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "shift local selection activates public selection state");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "shift local selection exposes selected text");
        ok &= send_hover_move(
            fixture.surface,
            end_point,
            Qt::NoModifier,
            true,
            "passive mouse reporting hover is accepted after shift selection");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            { sgr_mouse_report(35, 0, 4, 'M') },
            "passive hover after shift selection writes SGR bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "passive hover after shift selection preserves selection state");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "passive hover after shift selection preserves selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "shift-mid-drag mouse-reporting surface starts");

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     drag_point  = point_in_grid_cell(fixture.surface, 0, 3);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-owned drag press is accepted before Shift modifier");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            drag_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "terminal-owned drag continues reporting after Shift modifier");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            drag_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::ShiftModifier,
            true,
            "terminal-owned drag release is reported after Shift modifier");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0,  0, 1, 'M'),
                sgr_mouse_report(36, 0, 3, 'M'),
                sgr_mouse_report(4,  0, 3, 'm'),
            },
            "terminal-owned Shift drag preserves press-drag-release reporting");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "detached scrollback selection surface starts");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "detached scrollback selection scrolls up");
        const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        if (scrolled_snapshot != nullptr) {
            const int first_visible_logical_row =
                scrolled_snapshot->viewport.scrollback_rows -
                scrolled_snapshot->viewport.offset_from_tail;
            const QString expected_text =
                QStringLiteral("scroll-line-%1")
                    .arg(first_visible_logical_row + 1, 3, 10, QLatin1Char('0'));
            ok &= check(snapshot_row_text(*scrolled_snapshot, 1) == expected_text,
                "detached scrollback selection fixture independently shows row text");
            const QPointF start_point = point_in_grid_cell(fixture.surface, 1, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 1, 14);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection drag is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection release is accepted");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "detached scrollback selection uses logical scrollback coordinates");
        }
        else {
            ok &= check(false, "detached scrollback selection snapshot is available");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "pending-output selection surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("scroll-line-080\r\n"));
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 14);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pending-output selection press drains callbacks before anchoring");
        const std::shared_ptr<const term::Terminal_render_snapshot> post_press_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int queued_line_count = 81;
        const int expected_first_line = post_press_snapshot != nullptr
            ? queued_line_count - post_press_snapshot->grid_size.rows + 1
            : -1;
        const QString expected_text =
            QStringLiteral("scroll-line-%1")
                .arg(expected_first_line, 3, 10, QLatin1Char('0'));
        ok &= check(post_press_snapshot != nullptr &&
            snapshot_row_text(*post_press_snapshot, 0) == expected_text &&
            snapshot_contains_text(*post_press_snapshot, QStringLiteral("scroll-line-080")),
            "pending-output selection press drains queued output before selecting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pending-output selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "pending-output selection release is accepted");
        ok &= check(!expected_text.isEmpty() &&
            fixture.surface.selected_text() == expected_text,
            "pending-output selection anchors against the drained snapshot");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized-output local selection surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hbravo"));
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "local selection press is ignored while synchronized output is hidden");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "local selection drag is ignored while synchronized output is hidden");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            false,
            "local selection release is ignored while synchronized output is hidden");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "hidden synchronized output does not create local selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "hidden synchronized output does not expose hidden selected text");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(0);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "selected_text drain surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "selected_text drain fixture starts with selected text");

        backend_ptr->emit_output(numbered_scroll_lines(80));
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "selected_text drains queued output while retaining evicted selection payload");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "selected_text drain keeps public selection state after eviction");
    }

    return ok;
}

bool test_synchronized_output_scroll_policy_property(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    const QMetaObject* meta_object = fixture.surface.metaObject();
    const int property_index =
        meta_object->indexOfProperty("synchronizedOutputScrollPolicy");
    ok &= check(property_index >= 0,
        "synchronizedOutputScrollPolicy property is registered");
    if (property_index < 0) {
        return ok;
    }

    const QMetaProperty property = meta_object->property(property_index);
    int changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::synchronized_output_scroll_policy_changed,
        &fixture.surface,
        [&changed_count] {
            ++changed_count;
        });

    ok &= check(property.read(&fixture.surface).toInt() ==
        static_cast<int>(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION),
        "synchronizedOutputScrollPolicy property reads the default policy");
    ok &= check(property.write(
            &fixture.surface,
            QVariant::fromValue(static_cast<int>(
                VNM_TerminalSurface::Synchronized_output_scroll_policy::
                    IMMEDIATE_PUBLIC_PROJECTION))),
        "synchronizedOutputScrollPolicy property writes immediate policy");
    ok &= check(fixture.surface.synchronized_output_scroll_policy() ==
        VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION,
        "synchronizedOutputScrollPolicy property updates the public getter");
    ok &= check(property.read(&fixture.surface).toInt() ==
        static_cast<int>(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION),
        "synchronizedOutputScrollPolicy property rereads immediate policy");
    ok &= check(changed_count == 1,
        "synchronizedOutputScrollPolicy property write emits notify once");
    ok &= check(property.write(
            &fixture.surface,
            QVariant::fromValue(static_cast<int>(
                VNM_TerminalSurface::Synchronized_output_scroll_policy::
                    IMMEDIATE_PUBLIC_PROJECTION))),
        "synchronizedOutputScrollPolicy property accepts idempotent write");
    ok &= check(changed_count == 1,
        "synchronizedOutputScrollPolicy idempotent write emits no notify");

    return ok;
}

bool test_scroll_diagnostic_enum_name_table(QGuiApplication& app)
{
    using Surface = VNM_TerminalSurface;
    bool ok = true;

    // NONE maps to an empty QString; every other enumerator maps to its
    // diagnostic spelling.
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::NONE).isEmpty(),
        "scroll_noop_cause_name(NONE) is empty");
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::ZERO_LINE_DELTA) ==
        QStringLiteral("zero_line_delta"),
        "scroll_noop_cause_name(ZERO_LINE_DELTA) is zero_line_delta");
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::NO_SESSION) ==
        QStringLiteral("no_session"),
        "scroll_noop_cause_name(NO_SESSION) is no_session");
    ok &= check(Surface::scroll_noop_cause_name(
            Surface::Scroll_noop_cause::SYNCHRONIZED_OUTPUT_DEFERRED) ==
        QStringLiteral("synchronized_output_deferred"),
        "scroll_noop_cause_name(SYNCHRONIZED_OUTPUT_DEFERRED) is synchronized_output_deferred");
    ok &= check(Surface::scroll_noop_cause_name(
            Surface::Scroll_noop_cause::SYNCHRONIZED_OUTPUT_PUBLISHED) ==
        QStringLiteral("synchronized_output_published"),
        "scroll_noop_cause_name(SYNCHRONIZED_OUTPUT_PUBLISHED) is synchronized_output_published");
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::ALTERNATE_SCREEN) ==
        QStringLiteral("alternate_screen"),
        "scroll_noop_cause_name(ALTERNATE_SCREEN) is alternate_screen");
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::BOUNDARY_OR_CLAMP) ==
        QStringLiteral("boundary_or_clamp"),
        "scroll_noop_cause_name(BOUNDARY_OR_CLAMP) is boundary_or_clamp");
    ok &= check(Surface::scroll_noop_cause_name(Surface::Scroll_noop_cause::NO_PUBLICATION) ==
        QStringLiteral("no_publication"),
        "scroll_noop_cause_name(NO_PUBLICATION) is no_publication");

    ok &= check(Surface::scroll_action_name(Surface::Scroll_action::NONE).isEmpty(),
        "scroll_action_name(NONE) is empty");
    ok &= check(Surface::scroll_action_name(Surface::Scroll_action::VIEWPORT_MOVED) ==
        QStringLiteral("viewport_moved"),
        "scroll_action_name(VIEWPORT_MOVED) is viewport_moved");
    ok &= check(Surface::scroll_action_name(Surface::Scroll_action::AT_BOUNDARY) ==
        QStringLiteral("at_boundary"),
        "scroll_action_name(AT_BOUNDARY) is at_boundary");
    ok &= check(Surface::scroll_action_name(Surface::Scroll_action::DEFERRED_INTENT_RECORDED) ==
        QStringLiteral("deferred_intent_recorded"),
        "scroll_action_name(DEFERRED_INTENT_RECORDED) is deferred_intent_recorded");
    ok &= check(Surface::scroll_action_name(Surface::Scroll_action::TERMINAL_INPUT) ==
        QStringLiteral("terminal_input"),
        "scroll_action_name(TERMINAL_INPUT) is terminal_input");

    // Drive real no-op scrolls through the public diagnostics API and confirm
    // the resulting Scroll_noop_cause, so the converter contract is anchored to
    // an actual producer of the enum and not just the table above. A fresh
    // surface has no session, so a unit line delta is a no-op for NO_SESSION,
    // and a zero line delta is a no-op for ZERO_LINE_DELTA regardless of session.
    Surface_fixture fixture;
    pump_events(app);

    const Surface::wheel_scroll_diagnostic_result_t zero_delta =
        fixture.surface.scroll_viewport_lines_with_diagnostics(0, QStringLiteral("api.lines"));
    ok &= check(zero_delta.no_op_cause == Surface::Scroll_noop_cause::ZERO_LINE_DELTA,
        "zero line delta no-op reports ZERO_LINE_DELTA cause");
    ok &= check(!zero_delta.event_accepted,
        "zero line delta no-op is not accepted");

    const Surface::wheel_scroll_diagnostic_result_t no_session =
        fixture.surface.scroll_viewport_lines_with_diagnostics(1, QStringLiteral("api.lines"));
    ok &= check(no_session.no_op_cause == Surface::Scroll_noop_cause::NO_SESSION,
        "session-less line scroll no-op reports NO_SESSION cause");
    ok &= check(!no_session.session_present,
        "session-less line scroll no-op marks no session present");
    ok &= check(!no_session.event_accepted,
        "session-less line scroll no-op is not accepted");

    return ok;
}

bool test_no_payload_copy_fallback_states(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "drag-armed copy fallback surface starts");
        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT);

        const QPointF arm_point = point_in_grid_cell(fixture.surface, 0, 1);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            arm_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "drag-armed copy fallback press is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "drag-armed copy fallback has no public copyable selection");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "drag-armed copy fallback exposes no selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("drag-armed-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "Ctrl+C while drag-armed falls through to terminal input");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("drag-armed-sentinel"),
            "Ctrl+C while drag-armed leaves clipboard unchanged");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            arm_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "drag-armed copy fallback release is accepted");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "cancelled replacement copy fallback surface starts");
        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT);

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "cancelled replacement initial press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "cancelled replacement initial drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "cancelled replacement initial release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "cancelled replacement fixture starts with a committed selection");

        const QPointF replacement_point = point_in_grid_cell(fixture.surface, 0, 5);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "cancelled replacement arm press is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "cancelled replacement arm clears the prior copy payload");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "cancelled replacement arm exposes no selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("cancelled-replacement-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "Ctrl+C during cancelled replacement falls through to terminal input");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("cancelled-replacement-sentinel"),
            "Ctrl+C during cancelled replacement leaves clipboard unchanged");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "cancelled replacement release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "cancelled replacement returns to no selection");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "cancelled replacement leaves no selected text");
    }

    return ok;
}

bool test_selection_visual_detach_after_row_mutation(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;

    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("original\r\nstable")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface mutation-detach selection starts");

    const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
    const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 7);
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        start_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "surface mutation-detach selection press is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "surface mutation-detach selection drag is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        end_point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "surface mutation-detach selection release is accepted");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("original"),
        "surface mutation-detach captures the original selected text");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hmutated!"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> mutated_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(mutated_snapshot != nullptr &&
        snapshot_row_text(*mutated_snapshot, 0) == QStringLiteral("mutated!"),
        "surface mutation-detach snapshot contains replacement row text");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("original"),
        "surface mutation-detach retains selected_text payload");
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "surface mutation-detach keeps public selection state active for retained payload");
    ok &= check(mutated_snapshot != nullptr &&
        mutated_snapshot->selection_spans.empty(),
        "surface mutation-detach emits no stale highlight over replacement text");
    backend_ptr->emit_output(QByteArrayLiteral("\r"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> followup_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selected_text() == QStringLiteral("original"),
        "surface mutation-detach follow-up retains selected_text payload");
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "surface mutation-detach follow-up keeps public selection state active");
    ok &= check(followup_snapshot != nullptr &&
        followup_snapshot->selection_spans.empty(),
        "surface mutation-detach cursor-only follow-up does not reattach stale spans");

    QGuiApplication::clipboard()->setText(QStringLiteral("mutation-detach-sentinel"),
        QClipboard::Clipboard);
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_C,
        Qt::ControlModifier,
        {},
        bytes_from_hex("03"),
        "surface mutation-detach Ctrl+C falls through to terminal input");
    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("mutation-detach-sentinel"),
        "surface mutation-detach Ctrl+C leaves retained payload out of the clipboard");

    return ok;
}

bool test_selection_drag_remaps_live_scrollback_viewport_spans(QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    fixture.window.resize(640, 680);
    fixture.surface.setSize(QSizeF(520.0, 600.0));
    fixture.surface.set_scrollback_limit(96);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(90)};

    bool started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface live-scroll selection remap starts");
    ok &= check(surface_scrolls_to_first_visible_row(fixture.surface, 18),
        "surface live-scroll selection remap scrolls to top row 018");

    const std::shared_ptr<const term::Terminal_render_snapshot> top_018 =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(top_018 != nullptr &&
        top_018->grid_size.rows    > 24 &&
        top_018->grid_size.columns > 16,
        "surface live-scroll selection remap fixture exposes the selected row range");
    if (top_018 == nullptr || top_018->grid_size.rows <= 24 || top_018->grid_size.columns <= 16) {
        return ok;
    }

    const QPointF start_point = point_in_grid_cell(fixture.surface, 18, 0);
    const QPointF end_point   = point_in_grid_cell(fixture.surface, 24, 16);
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        start_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "surface live-scroll selection remap press is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "surface live-scroll selection remap drag is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        end_point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "surface live-scroll selection remap release is accepted");

    const std::shared_ptr<const term::Terminal_render_snapshot> selected =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "surface live-scroll selection remap activates public selection state");
    ok &= check(selected != nullptr &&
        surface_public_prefix_selection_is_highlighted(*selected, 18),
        "surface live-scroll selection remap highlights selected rows at top row 018");

    ok &= check(surface_scrolls_to_first_visible_row(fixture.surface, 21),
        "surface live-scroll selection remap scrolls to top row 021");
    const std::shared_ptr<const term::Terminal_render_snapshot> top_021 =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(top_021 != nullptr &&
        surface_public_prefix_selection_is_highlighted(*top_021, 21),
        "surface live-scroll selection remap keeps spans at top row 021");

    ok &= check(surface_scrolls_to_first_visible_row(fixture.surface, 18),
        "surface live-scroll selection remap returns to top row 018");
    const std::shared_ptr<const term::Terminal_render_snapshot> returned_from_021 =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(returned_from_021 != nullptr &&
        surface_public_prefix_selection_is_highlighted(*returned_from_021, 18),
        "surface live-scroll selection remap restores spans after top row 021");

    ok &= check(surface_scrolls_to_first_visible_row(fixture.surface, 15),
        "surface live-scroll selection remap scrolls to top row 015");
    const std::shared_ptr<const term::Terminal_render_snapshot> top_015 =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(top_015 != nullptr &&
        surface_public_prefix_selection_is_highlighted(*top_015, 15),
        "surface live-scroll selection remap keeps spans at top row 015");

    ok &= check(surface_scrolls_to_first_visible_row(fixture.surface, 18),
        "surface live-scroll selection remap returns to top row 018 again");
    const std::shared_ptr<const term::Terminal_render_snapshot> returned_from_015 =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(returned_from_015 != nullptr &&
        surface_public_prefix_selection_is_highlighted(*returned_from_015, 18),
        "surface live-scroll selection remap restores spans after top row 015");

    return ok;
}

bool test_selection_drag_survives_unrelated_row_backend_output(QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(0);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "backend-output interleaved drag surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const std::uint64_t initial_sequence =
        initial_snapshot != nullptr ? initial_snapshot->metadata.sequence : 0U;
    ok &= check(initial_snapshot != nullptr &&
        snapshot_row_text(*initial_snapshot, 0) == QStringLiteral("alpha") &&
        snapshot_row_text(*initial_snapshot, 1) == QStringLiteral("beta"),
        "backend-output interleaved drag starts with stable visible content");

    const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
    const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        start_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "backend-output interleaved drag press is accepted");
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::NONE,
        "backend-output interleaved drag press alone does not activate selection");

    backend_ptr->emit_output(QByteArrayLiteral("!"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> interleaved_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(interleaved_snapshot != nullptr &&
        interleaved_snapshot->metadata.sequence > initial_sequence &&
        snapshot_row_text(*interleaved_snapshot, 0) == QStringLiteral("alpha") &&
        snapshot_row_text(*interleaved_snapshot, 1) == QStringLiteral("beta!"),
        "backend-output interleaved drag publishes unrelated backend output before first move");

    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "backend-output interleaved drag move is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> moved_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "backend-output interleaved drag move keeps public selection state active");
    ok &= check(moved_snapshot != nullptr &&
        moved_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*moved_snapshot, 0, 1, 4),
        "backend-output interleaved drag move publishes a visible selection span");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        end_point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "backend-output interleaved drag release is accepted");

    const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "backend-output interleaved drag keeps public selection state active");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
        "backend-output interleaved drag exposes selected text from the anchor row");
    ok &= check(final_snapshot != nullptr &&
        final_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*final_snapshot, 0, 1, 4),
        "backend-output interleaved drag publishes a visible selection span");

    backend_ptr->emit_output(QByteArrayLiteral("?"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> post_release_output_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "post-release unrelated output keeps public selection state active");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
        "post-release unrelated output keeps selected text from the anchor row");
    ok &= check(post_release_output_snapshot != nullptr &&
        snapshot_row_text(*post_release_output_snapshot, 1) == QStringLiteral("beta!?") &&
        post_release_output_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*post_release_output_snapshot, 0, 1, 4),
        "post-release unrelated output keeps committed selection spans renderable");

    return ok;
}

bool test_selection_drag_survives_worker_unrelated_row_backend_output(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "worker unrelated-row selection surface starts");

    const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
    const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        start_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "worker unrelated-row selection press is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "worker unrelated-row selection move is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        end_point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "worker unrelated-row selection release is accepted");

    const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(selected_snapshot != nullptr &&
        selected_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*selected_snapshot, 0, 1, 4),
        "worker unrelated-row selection publishes committed selection span");

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\x1b[2;5H!"));
    backend_ptr->join_worker();
    pump_events(app);

    const std::shared_ptr<const term::Terminal_render_snapshot> worker_output_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(worker_output_snapshot != nullptr &&
        snapshot_row_text(*worker_output_snapshot, 1) == QStringLiteral("beta!") &&
        worker_output_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*worker_output_snapshot, 0, 1, 4),
        "worker unrelated-row backend output keeps committed selection styling");

    {
        Surface_fixture release_fixture;
        pump_events(app);

        auto release_backend = std::make_unique<Scripted_backend>();
        release_backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool release_started = false;
        Scripted_backend* release_backend_ptr = start_surface_with_backend(
            release_fixture.surface,
            std::move(release_backend),
            { QStringLiteral("scripted-terminal") },
            &release_started);
        ok &= check(release_started, "worker release-drain selection surface starts");

        const QPointF release_start_point = point_in_grid_cell(release_fixture.surface, 0, 1);
        const QPointF release_end_point   = point_in_grid_cell(release_fixture.surface, 0, 4);
        ok &= send_mouse_event(
            release_fixture.surface,
            QEvent::MouseButtonPress,
            release_start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "worker release-drain selection press is accepted");
        ok &= send_mouse_event(
            release_fixture.surface,
            QEvent::MouseMove,
            release_end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "worker release-drain selection move is accepted");

        release_backend_ptr->emit_output_from_worker(QByteArrayLiteral("\x1b[2;5H!"));
        release_backend_ptr->join_worker();
        ok &= check(
            !term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
                release_fixture.surface),
            "worker release-drain does not queue posted drain work before mouse release");
        ok &= check(
            term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
                release_fixture.surface) > 0U,
            "worker release-drain has pending backend output before mouse release");
        const std::shared_ptr<const term::Terminal_render_snapshot> queued_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(release_fixture.surface);
        ok &= check(queued_snapshot != nullptr &&
            snapshot_row_text(*queued_snapshot, 1) == QStringLiteral("beta"),
            "worker release-drain does not mutate snapshot before release");

        ok &= send_mouse_event(
            release_fixture.surface,
            QEvent::MouseButtonRelease,
            release_end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "worker release-drain selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> release_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(release_fixture.surface);
        ok &= check(release_fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "worker release-drain keeps public selection state active");
        ok &= check(release_fixture.surface.selected_text() == QStringLiteral("lpha"),
            "worker release-drain keeps selected text from anchor row");
        ok &= check(release_snapshot != nullptr &&
            snapshot_row_text(*release_snapshot, 1) == QStringLiteral("beta!") &&
            release_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*release_snapshot, 0, 1, 4),
            "worker release-drain keeps committed selection styling after drain");
    }

    return ok;
}

bool test_selection_drag_survives_at_tail_streaming_output(QGuiApplication& app)
{
    bool ok = true;

    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "at-tail streaming drag surface starts");

    constexpr int move_cycle_count          = 4;
    constexpr int emits_per_move            = 2;
    constexpr int release_tail_append_count = 2;
    constexpr int anchor_viewport_row =
        move_cycle_count * emits_per_move + release_tail_append_count;
    constexpr int release_viewport_row =
        anchor_viewport_row - move_cycle_count * emits_per_move - release_tail_append_count;
    const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const bool usable_anchor_snapshot =
        anchor_snapshot                            != nullptr             &&
        anchor_snapshot->grid_size.rows            >  anchor_viewport_row &&
        anchor_snapshot->viewport.offset_from_tail == 0                   &&
        anchor_snapshot->viewport.scrollback_rows  >  0                   &&
        release_viewport_row                       >= 0;
    ok &= check(usable_anchor_snapshot,
        "at-tail streaming drag fixture starts at tail with enough visible rows");

    if (usable_anchor_snapshot) {
        const QString expected_text =
            snapshot_row_text(*anchor_snapshot, anchor_viewport_row);
        const int line_last_column = expected_text.size() - 1;
        ok &= check(!expected_text.isEmpty() &&
            anchor_snapshot->grid_size.columns > line_last_column,
            "at-tail streaming drag anchor row has selectable text");

        const QPointF start_point =
            point_in_grid_cell(fixture.surface, anchor_viewport_row, 0);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "at-tail streaming drag press is accepted");

        for (int cycle = 0; cycle < move_cycle_count; ++cycle) {
            for (int emit_index = 0; emit_index < emits_per_move; ++emit_index) {
                backend_ptr->emit_output(
                    QStringLiteral("stream-line-%1\r\n")
                        .arg(
                            80 + cycle * emits_per_move + emit_index,
                            3,
                            10,
                            QLatin1Char('0'))
                        .toUtf8());
            }

            const int current_viewport_row =
                anchor_viewport_row - (cycle + 1) * emits_per_move;
            const QPointF move_point =
                point_in_grid_cell(fixture.surface, current_viewport_row, line_last_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                move_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "at-tail streaming drag move is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> move_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(move_snapshot != nullptr &&
                move_snapshot->viewport.scrollback_rows >
                    anchor_snapshot->viewport.scrollback_rows &&
                snapshot_row_text(*move_snapshot, current_viewport_row) == expected_text,
                "at-tail streaming drag keeps the selected logical row visible");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "at-tail streaming drag keeps public selection active after a move");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "at-tail streaming drag keeps selected_text on the stable logical row");
            ok &= check(move_snapshot != nullptr &&
                move_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(
                    *move_snapshot,
                    current_viewport_row,
                    0,
                    line_last_column + 1),
                "at-tail streaming drag publishes a moved selection span");
        }

        for (int emit_index = 0; emit_index < release_tail_append_count; ++emit_index) {
            backend_ptr->emit_output(
                QStringLiteral("stream-line-%1\r\n")
                    .arg(
                        80 + move_cycle_count * emits_per_move + emit_index,
                        3,
                        10,
                        QLatin1Char('0'))
                    .toUtf8());
        }
        const QPointF release_point =
            point_in_grid_cell(fixture.surface, release_viewport_row, line_last_column);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            release_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "at-tail streaming drag release is accepted");

        const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "at-tail streaming drag keeps public selection active after release");
        ok &= check(fixture.surface.selected_text() == expected_text,
            "at-tail streaming drag selects the stable logical row");
        ok &= check(final_snapshot != nullptr &&
            final_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(
                *final_snapshot,
                release_viewport_row,
                0,
                line_last_column + 1),
            "at-tail streaming drag publishes the final shifted selection span");
    }

    return ok;
}

bool test_selection_drag_stationary_pointer_at_tail_streaming_contract(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "stationary streaming provable drag surface starts");

        constexpr int anchor_viewport_row = 4;
        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot                            != nullptr                  &&
            anchor_snapshot->grid_size.rows            >  anchor_viewport_row + 1 &&
            anchor_snapshot->viewport.offset_from_tail == 0                        &&
            anchor_snapshot->viewport.scrollback_rows  >  0;
        ok &= check(usable_anchor_snapshot,
            "stationary streaming provable drag fixture starts at tail with enough rows");

        if (usable_anchor_snapshot) {
            const QString expected_first =
                snapshot_row_text(*anchor_snapshot, anchor_viewport_row);
            const QString expected_second =
                snapshot_row_text(*anchor_snapshot, anchor_viewport_row + 1);
            const QString expected_text = expected_first + QLatin1Char('\n') + expected_second;
            const int line_last_column  = expected_first.size() - 1;
            ok &= check(!expected_first.isEmpty()             &&
                expected_first.size() == expected_second.size() &&
                anchor_snapshot->grid_size.columns > line_last_column,
                "stationary streaming provable drag anchor rows have selectable text");

            const QPointF start_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, 0);
            const QPointF fixed_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, line_last_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming provable drag press is accepted");

            backend_ptr->emit_output(QByteArrayLiteral("stationary-tail-000\r\n"));
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                fixed_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming provable drag move is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> move_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(move_snapshot != nullptr &&
                snapshot_row_text(*move_snapshot, anchor_viewport_row - 1) == expected_first &&
                snapshot_row_text(*move_snapshot, anchor_viewport_row)     == expected_second,
                "stationary streaming provable drag keeps selected logical rows visible");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "stationary streaming provable drag keeps public selection active");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "stationary streaming provable drag selects the proven logical range");
            const int move_first_row_span_columns =
                move_snapshot != nullptr ? move_snapshot->grid_size.columns : 0;
            ok &= check(move_snapshot != nullptr &&
                move_snapshot->selection_spans.size() == 2U &&
                snapshot_has_selection_span(
                    *move_snapshot,
                    anchor_viewport_row - 1,
                    0,
                    move_first_row_span_columns) &&
                snapshot_has_selection_span(
                    *move_snapshot,
                    anchor_viewport_row,
                    0,
                    line_last_column + 1),
                "stationary streaming provable drag move publishes shifted selection spans");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                fixed_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "stationary streaming provable drag release is accepted");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "stationary streaming provable drag remains active after release");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "stationary streaming provable drag keeps selected_text after release");
            const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const int final_first_row_span_columns =
                final_snapshot != nullptr ? final_snapshot->grid_size.columns : 0;
            ok &= check(final_snapshot != nullptr &&
                final_snapshot->selection_spans.size() == 2U &&
                snapshot_has_selection_span(
                    *final_snapshot,
                    anchor_viewport_row - 1,
                    0,
                    final_first_row_span_columns) &&
                snapshot_has_selection_span(
                    *final_snapshot,
                    anchor_viewport_row,
                    0,
                    line_last_column + 1),
                "stationary streaming provable drag release keeps shifted selection spans");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "stationary streaming unmappable first-move drag surface starts");

        constexpr int anchor_viewport_row = 4;
        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot                            != nullptr             &&
            anchor_snapshot->grid_size.rows            >  anchor_viewport_row &&
            anchor_snapshot->viewport.offset_from_tail == 0                   &&
            anchor_snapshot->viewport.scrollback_rows  >  0;
        ok &= check(usable_anchor_snapshot,
            "stationary streaming unmappable first-move fixture starts at tail");

        if (usable_anchor_snapshot) {
            const QString expected_text =
                snapshot_row_text(*anchor_snapshot, anchor_viewport_row);
            const int line_last_column = expected_text.size() - 1;
            ok &= check(!expected_text.isEmpty() &&
                anchor_snapshot->grid_size.columns > line_last_column,
                "stationary streaming unmappable first-move anchor row has selectable text");

            const QPointF start_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, 0);
            const QPointF fixed_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, line_last_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable first-move press is accepted");

            backend_ptr->emit_output(numbered_scroll_lines(anchor_snapshot->grid_size.rows + 1));
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                fixed_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable first-move drag move is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                fixed_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable first-move drag release is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::NONE,
                "stationary streaming unmappable first move cancels without prior payload");
            ok &= check(fixture.surface.selected_text().isEmpty(),
                "stationary streaming unmappable first move exposes no selected_text");
            ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
                "stationary streaming unmappable first move emits no selection spans");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "stationary streaming unmappable payload drag surface starts");

        constexpr int anchor_viewport_row = 4;
        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot                            != nullptr             &&
            anchor_snapshot->grid_size.rows            >  anchor_viewport_row &&
            anchor_snapshot->viewport.offset_from_tail == 0                   &&
            anchor_snapshot->viewport.scrollback_rows  >  0;
        ok &= check(usable_anchor_snapshot,
            "stationary streaming unmappable payload fixture starts at tail");

        if (usable_anchor_snapshot) {
            const QString expected_text =
                snapshot_row_text(*anchor_snapshot, anchor_viewport_row);
            const int line_last_column = expected_text.size() - 1;
            ok &= check(!expected_text.isEmpty() &&
                anchor_snapshot->grid_size.columns > line_last_column,
                "stationary streaming unmappable payload anchor row has selectable text");

            const QPointF start_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, 0);
            const QPointF fixed_point =
                point_in_grid_cell(fixture.surface, anchor_viewport_row, line_last_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable payload press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                fixed_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable payload initial move is accepted");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "stationary streaming unmappable payload captures initial payload");

            backend_ptr->emit_output(numbered_scroll_lines(anchor_snapshot->grid_size.rows + 1));
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                fixed_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable payload drift move is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "stationary streaming unmappable payload keeps public selection active");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "stationary streaming unmappable payload preserves prior selected_text");
            ok &= check(detached_snapshot != nullptr &&
                detached_snapshot->selection_spans.empty(),
                "stationary streaming unmappable payload emits no stale spans");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                fixed_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "stationary streaming unmappable payload release is accepted");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "stationary streaming unmappable payload remains active after release");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "stationary streaming unmappable payload keeps selected_text after release");
        }
    }

    return ok;
}

bool test_selection_drag_survives_stable_row_content_drift(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta\r\ngamma")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "reverse drift drag surface starts");

        const QPointF anchor_point = point_in_grid_cell(fixture.surface, 1, 2);
        const QPointF extent_point = point_in_grid_cell(fixture.surface, 0, 1);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            anchor_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "reverse drift drag press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[3;6H!"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> drift_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(drift_snapshot != nullptr &&
            snapshot_row_text(*drift_snapshot, 2) == QStringLiteral("gamma!"),
            "reverse drift drag mutates only an unselected row");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            extent_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "reverse drift drag move is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            extent_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "reverse drift drag release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "reverse drift drag keeps public selection state active");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha\nbet"),
            "reverse drift drag captures selected rows after unrelated row output");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "detached append drift drag surface starts");

        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "detached append drift drag scrolls up");
        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int line_last_column = QStringLiteral("scroll-line-000").size() - 1;
        const bool usable_anchor_snapshot =
            anchor_snapshot                         != nullptr          &&
            anchor_snapshot->grid_size.rows         >  2                &&
            anchor_snapshot->grid_size.columns      >  line_last_column &&
            anchor_snapshot->viewport.scrollback_rows > 0               &&
            anchor_snapshot->viewport.offset_from_tail > 0;
        ok &= check(usable_anchor_snapshot,
            "detached append drift drag fixture has detached scrollback");

        if (usable_anchor_snapshot) {
            const int first_visible_logical_row =
                anchor_snapshot->viewport.scrollback_rows -
                anchor_snapshot->viewport.offset_from_tail;
            const QString expected_text =
                QStringLiteral("scroll-line-%1")
                    .arg(first_visible_logical_row + 1, 3, 10, QLatin1Char('0'));
            ok &= check(snapshot_row_text(*anchor_snapshot, 1) == expected_text,
                "detached append drift drag anchor row has expected text");

            const QPointF start_point = point_in_grid_cell(fixture.surface, 1, 0);
            const QPointF end_point   =
                point_in_grid_cell(fixture.surface, 1, line_last_column);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached append drift drag press is accepted");

            backend_ptr->emit_output(QByteArrayLiteral("scroll-line-080\r\n"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> append_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const int append_first_visible_logical_row = append_snapshot != nullptr
                ? append_snapshot->viewport.scrollback_rows -
                    append_snapshot->viewport.offset_from_tail
                : -1;
            ok &= check(append_snapshot != nullptr &&
                append_snapshot->viewport.scrollback_rows >
                    anchor_snapshot->viewport.scrollback_rows &&
                append_first_visible_logical_row == first_visible_logical_row &&
                snapshot_row_text(*append_snapshot, 1) == expected_text,
                "detached append drift preserves visible logical rows");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached append drift drag move is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "detached append drift drag release is accepted");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "detached append drift drag keeps public selection state active");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "detached append drift drag selects stable detached scrollback row");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(0);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            repeated_scroll_lines(fixture.surface.rows() + 2, QByteArrayLiteral("repeat")),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "zero-scrollback reuse drag surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                         &&
            anchor_snapshot->grid_size.columns > 5             &&
            anchor_snapshot->viewport.scrollback_rows == 0     &&
            snapshot_row_text(*anchor_snapshot, 0) == QStringLiteral("repeat");
        ok &= check(usable_anchor_snapshot,
            "zero-scrollback reuse drag starts with repeated visible row text");

        if (usable_anchor_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 5);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "zero-scrollback reuse drag press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "zero-scrollback reuse drag initial move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "zero-scrollback reuse drag captures payload before row reuse");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "zero-scrollback reuse drag captures repeated row payload");
            ok &= check(selected_snapshot != nullptr &&
                selected_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(*selected_snapshot, 0, 0, 6),
                "zero-scrollback reuse drag initially publishes a selection span");

            backend_ptr->emit_output(
                repeated_scroll_lines(fixture.surface.rows() + 2, QByteArrayLiteral("repeat")));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> reused_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(reused_snapshot != nullptr &&
                reused_snapshot->viewport.scrollback_rows == 0 &&
                snapshot_row_text(*reused_snapshot, 0) == QStringLiteral("repeat"),
                "zero-scrollback reuse drag keeps repeated row text after discard");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "zero-scrollback reuse drag reuse move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "zero-scrollback reuse drag preserves payload after row-origin ambiguity");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "zero-scrollback reuse drag keeps prior selected text after ambiguity");
            ok &= check(detached_snapshot != nullptr &&
                detached_snapshot->selection_spans.empty(),
                "zero-scrollback reuse drag does not reattach stale spans");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "zero-scrollback reuse drag release is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "zero-scrollback reuse drag keeps selected text after release");
            ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
                "zero-scrollback reuse drag release keeps stale spans detached");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(0);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            repeated_scroll_lines(fixture.surface.rows() + 2, QByteArrayLiteral("repeat")),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "hidden synchronized zero-scrollback reuse surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                         &&
            anchor_snapshot->grid_size.columns > 5             &&
            anchor_snapshot->viewport.scrollback_rows == 0     &&
            snapshot_row_text(*anchor_snapshot, 0) == QStringLiteral("repeat");
        ok &= check(usable_anchor_snapshot,
            "hidden synchronized zero-scrollback reuse starts with repeated visible row text");

        if (usable_anchor_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 5);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "hidden synchronized zero-scrollback reuse press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "hidden synchronized zero-scrollback reuse initial move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const std::uint64_t selected_sequence =
                selected_snapshot != nullptr ? selected_snapshot->metadata.sequence : 0U;
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "hidden synchronized zero-scrollback reuse captures payload before hold");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "hidden synchronized zero-scrollback reuse captures repeated row payload");
            ok &= check(selected_snapshot != nullptr &&
                selected_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(*selected_snapshot, 0, 0, 6),
                "hidden synchronized zero-scrollback reuse initially publishes a selection span");

            backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            backend_ptr->emit_output(
                repeated_scroll_lines(fixture.surface.rows() + 2, QByteArrayLiteral("repeat")));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> held_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(held_snapshot != nullptr &&
                held_snapshot->metadata.sequence == selected_sequence &&
                held_snapshot->selection_spans.size() == 1U,
                "hidden synchronized zero-scrollback reuse does not publish held reuse");

            backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "hidden synchronized zero-scrollback reuse post-release move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "hidden synchronized zero-scrollback reuse preserves payload after ambiguity");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "hidden synchronized zero-scrollback reuse keeps prior selected text");
            ok &= check(detached_snapshot != nullptr &&
                detached_snapshot->selection_spans.empty(),
                "hidden synchronized zero-scrollback reuse does not reattach stale spans");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "hidden synchronized zero-scrollback reuse release is accepted");
        }
    }

    {
        constexpr int scrollback_limit = 4;

        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(scrollback_limit);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "eviction-rebased drag surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                              &&
            anchor_snapshot->grid_size.columns > 5                  &&
            anchor_snapshot->viewport.scrollback_rows == scrollback_limit;
        ok &= check(usable_anchor_snapshot,
            "eviction-rebased drag fixture fills scrollback limit");

        if (usable_anchor_snapshot) {
            const QString anchor_text = snapshot_row_text(*anchor_snapshot, 0);
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "eviction-rebased drag press is accepted");

            backend_ptr->emit_output(QByteArrayLiteral("scroll-line-999\r\n"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> eviction_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const QString current_text = eviction_snapshot != nullptr
                ? snapshot_row_text(*eviction_snapshot, 0)
                : QString();
            const int line_last_column = current_text.size() - 1;
            const bool usable_eviction_snapshot =
                eviction_snapshot != nullptr                                      &&
                eviction_snapshot->viewport.scrollback_rows == scrollback_limit &&
                !current_text.isEmpty()                                         &&
                current_text != anchor_text                                     &&
                eviction_snapshot->grid_size.columns > line_last_column;
            ok &= check(usable_eviction_snapshot,
                "eviction-rebased drag shifts to distinct visible text after eviction");

            if (usable_eviction_snapshot) {
                const QPointF end_point =
                    point_in_grid_cell(fixture.surface, 0, line_last_column);
                ok &= send_mouse_event(
                    fixture.surface,
                    QEvent::MouseMove,
                    end_point,
                    Qt::NoButton,
                    Qt::LeftButton,
                    Qt::NoModifier,
                    true,
                    "eviction-rebased drag move is accepted");
                ok &= send_mouse_event(
                    fixture.surface,
                    QEvent::MouseButtonRelease,
                    end_point,
                    Qt::LeftButton,
                    Qt::NoButton,
                    Qt::NoModifier,
                    true,
                    "eviction-rebased drag release is accepted");
                const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                    term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
                ok &= check(fixture.surface.selection_state() ==
                    VNM_TerminalSurface::Selection_state::ACTIVE,
                    "eviction-rebased drag keeps current visible selection active");
                ok &= check(fixture.surface.selected_text() == current_text,
                    "eviction-rebased drag selects the current visible text");
                ok &= check(final_snapshot != nullptr &&
                    final_snapshot->selection_spans.size() == 1U &&
                    snapshot_has_selection_span(
                        *final_snapshot,
                        0,
                        0,
                        line_last_column + 1),
                    "eviction-rebased drag publishes a current visible selection span");
            }
        }
    }

    {
        constexpr int scrollback_limit = 3;

        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(scrollback_limit);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "eviction-rebased click surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                              &&
            anchor_snapshot->grid_size.columns > 0                  &&
            anchor_snapshot->viewport.scrollback_rows == scrollback_limit;
        ok &= check(usable_anchor_snapshot,
            "eviction-rebased click fixture fills scrollback limit");

        if (usable_anchor_snapshot) {
            const QString anchor_text = snapshot_row_text(*anchor_snapshot, 0);
            const QPointF click_point = point_in_grid_cell(fixture.surface, 0, 0);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                click_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "eviction-rebased click press is accepted");

            backend_ptr->emit_output(QByteArrayLiteral("scroll-line-999\r\n"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> eviction_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(eviction_snapshot != nullptr &&
                eviction_snapshot->viewport.scrollback_rows == scrollback_limit &&
                snapshot_row_text(*eviction_snapshot, 0) != anchor_text,
                "eviction-rebased click shifts the same viewport cell to new text");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                click_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "eviction-rebased click release is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::NONE,
                "eviction-rebased click keeps click semantics after eviction");
            ok &= check(fixture.surface.selected_text().isEmpty(),
                "eviction-rebased click exposes no selected text");
            ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
                "eviction-rebased click publishes no selection span");
        }
    }

    {
        constexpr int scrollback_limit = 4;

        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(scrollback_limit);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "eviction-boundary drag surface starts");

        std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot;
        const int max_fill_line_count = fixture.surface.rows() + scrollback_limit + 8;
        for (int line = 0; line < max_fill_line_count; ++line) {
            anchor_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            if (anchor_snapshot != nullptr &&
                anchor_snapshot->viewport.scrollback_rows == scrollback_limit - 1)
            {
                break;
            }

            backend_ptr->emit_output(QByteArrayLiteral("repeat\r\n"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
        }

        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                                      &&
            anchor_snapshot->grid_size.rows            >  4                 &&
            anchor_snapshot->grid_size.columns         >  5                 &&
            anchor_snapshot->viewport.offset_from_tail == 0                 &&
            anchor_snapshot->viewport.scrollback_rows  == scrollback_limit - 1;
        ok &= check(usable_anchor_snapshot,
            "eviction-boundary drag fixture stops one row below scrollback limit");

        if (usable_anchor_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 2, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 2, 5);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "eviction-boundary drag press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "eviction-boundary drag initial move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "eviction-boundary drag captures payload before cap ambiguity");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "eviction-boundary drag captures selected text before cap ambiguity");
            ok &= check(selected_snapshot != nullptr &&
                selected_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(*selected_snapshot, 2, 0, 6),
                "eviction-boundary drag initially publishes a selection span");

            backend_ptr->emit_output(QByteArrayLiteral("repeat\r\nrepeat\r\n"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> eviction_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(eviction_snapshot != nullptr &&
                eviction_snapshot->viewport.scrollback_rows == scrollback_limit &&
                snapshot_row_text(*eviction_snapshot, 2) == QStringLiteral("repeat"),
                "eviction-boundary drag reaches scrollback cap with repeated visible text");

            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "eviction-boundary drag move is accepted");
            const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "eviction-boundary drag preserves payload after cap ambiguity");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "eviction-boundary drag keeps selected text after cap ambiguity");
            ok &= check(detached_snapshot != nullptr &&
                detached_snapshot->selection_spans.empty(),
                "eviction-boundary drag detaches stale cap-ambiguous spans");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "eviction-boundary drag release is accepted");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "eviction-boundary drag keeps payload active after release");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("repeat"),
                "eviction-boundary drag keeps selected text after release");
            const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
                "eviction-boundary drag release emits no stale selection spans");
        }
    }

    return ok;
}

bool test_selection_drag_preserves_payload_after_mid_drag_drift(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;

    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("alpha")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "mid-drag payload detach surface starts");

    const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
    const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonPress,
        start_point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "mid-drag payload detach press is accepted");
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "mid-drag payload detach initial move is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
        "mid-drag payload detach captures the initial payload");
    ok &= check(selected_snapshot != nullptr &&
        selected_snapshot->selection_spans.size() == 1U &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 5),
        "mid-drag payload detach initially publishes a visible span");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hbravo"));
    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseMove,
        end_point,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        true,
        "mid-drag payload detach drift move is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot != nullptr &&
        snapshot_row_text(*detached_snapshot, 0) == QStringLiteral("bravo"),
        "mid-drag payload detach publishes the mutated selected row");
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "mid-drag payload detach keeps public selection active");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
        "mid-drag payload detach preserves the prior safe payload");
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->selection_spans.empty(),
        "mid-drag payload detach emits no stale span");

    QGuiApplication::clipboard()->setText(QStringLiteral("mid-drag-detach-sentinel"),
        QClipboard::Clipboard);
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_C,
        Qt::ControlModifier,
        {},
        bytes_from_hex("03"),
        "mid-drag payload detach Ctrl+C falls through to terminal input");
    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("mid-drag-detach-sentinel"),
        "mid-drag payload detach Ctrl+C leaves retained payload out of the clipboard");

    ok &= send_mouse_event(
        fixture.surface,
        QEvent::MouseButtonRelease,
        end_point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        true,
        "mid-drag payload detach release is accepted");
    const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(fixture.surface.selection_state() ==
        VNM_TerminalSurface::Selection_state::ACTIVE,
        "mid-drag payload detach keeps public selection active after release");
    ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
        "mid-drag payload detach keeps the prior payload after release");
    ok &= check(final_snapshot != nullptr &&
        final_snapshot->selection_spans.empty(),
        "mid-drag payload detach release emits no stale span");

    QGuiApplication::clipboard()->setText(QStringLiteral("mid-drag-release-sentinel"),
        QClipboard::Clipboard);
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_C,
        Qt::ControlModifier,
        {},
        bytes_from_hex("03"),
        "mid-drag payload detach post-release Ctrl+C falls through to terminal input");
    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("mid-drag-release-sentinel"),
        "mid-drag payload detach post-release Ctrl+C leaves retained payload out of the clipboard");

    return ok;
}

bool test_selection_drag_rejects_snapshot_change(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "snapshot-changing drag selection starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "snapshot-changing drag press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hbravo"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> changed_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(changed_snapshot != nullptr &&
            snapshot_row_text(*changed_snapshot, 0) == QStringLiteral("bravo"),
            "snapshot-changing drag publishes changed text before drag extent");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "snapshot-changing drag move is accepted without mixing sources");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "snapshot-changing drag release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(final_snapshot != nullptr,
            "snapshot-changing drag has a final render snapshot");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "snapshot-changing drag does not create selection state from incompatible sources");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "snapshot-changing drag exposes no mixed-source selected text");
        ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
            "snapshot-changing drag emits no mixed-source selection spans");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        (void)start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "resize-incompatible mid-drag surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_initial_snapshot =
            initial_snapshot != nullptr && initial_snapshot->grid_size.columns > 4;
        ok &= check(usable_initial_snapshot,
            "resize-incompatible mid-drag fixture has selectable initial grid");

        if (usable_initial_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "resize-incompatible mid-drag press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "resize-incompatible mid-drag initial move is accepted");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "resize-incompatible mid-drag captures selection before resize");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
                "resize-incompatible mid-drag captures payload before resize");

            const term::terminal_grid_size_t initial_grid_size = initial_snapshot->grid_size;
            std::shared_ptr<const term::Terminal_render_snapshot> resized_snapshot;
            const QSizeF resize_candidates[] = {
                QSizeF(800.0, 360.0),
                QSizeF(900.0, 420.0),
                QSizeF(360.0, 180.0),
            };
            for (QSizeF size : resize_candidates) {
                fixture.surface.setSize(size);
                pump_events(app, 2);
                resized_snapshot =
                    term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
                if (resized_snapshot != nullptr &&
                    resized_snapshot->grid_size.columns > 4 &&
                    (resized_snapshot->grid_size.rows    != initial_grid_size.rows ||
                        resized_snapshot->grid_size.columns != initial_grid_size.columns))
                {
                    break;
                }
            }

            const bool usable_resized_snapshot =
                resized_snapshot != nullptr                       &&
                resized_snapshot->grid_size.columns > 4            &&
                (resized_snapshot->grid_size.rows    != initial_grid_size.rows ||
                    resized_snapshot->grid_size.columns != initial_grid_size.columns);
            ok &= check(usable_resized_snapshot,
                "resize-incompatible mid-drag fixture changes published grid size");

            if (usable_resized_snapshot) {
                const QPointF resized_point = point_in_grid_cell(fixture.surface, 0, 4);
                ok &= send_mouse_event(
                    fixture.surface,
                    QEvent::MouseMove,
                    resized_point,
                    Qt::NoButton,
                    Qt::LeftButton,
                    Qt::NoModifier,
                    true,
                    "resize-incompatible mid-drag mismatch move is accepted");

                const std::shared_ptr<const term::Terminal_render_snapshot> mismatch_snapshot =
                    term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
                ok &= check(fixture.surface.selection_state() ==
                    VNM_TerminalSurface::Selection_state::NONE,
                    "resize-incompatible mid-drag clears selection state");
                ok &= check(fixture.surface.selected_text().isEmpty(),
                    "resize-incompatible mid-drag clears stale selected_text payload");
                ok &= check(mismatch_snapshot != nullptr &&
                    mismatch_snapshot->selection_spans.empty(),
                    "resize-incompatible mid-drag emits no stale selection spans");

                ok &= send_mouse_event(
                    fixture.surface,
                    QEvent::MouseButtonRelease,
                    resized_point,
                    Qt::LeftButton,
                    Qt::NoButton,
                    Qt::NoModifier,
                    true,
                    "resize-incompatible mid-drag release is accepted");
                const std::shared_ptr<const term::Terminal_render_snapshot> release_snapshot =
                    term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
                ok &= check(fixture.surface.selection_state() ==
                    VNM_TerminalSurface::Selection_state::NONE,
                    "resize-incompatible mid-drag remains cleared after release");
                ok &= check(fixture.surface.selected_text().isEmpty(),
                    "resize-incompatible mid-drag keeps selected_text empty after release");
                ok &= check(release_snapshot != nullptr &&
                    release_snapshot->selection_spans.empty(),
                    "resize-incompatible mid-drag release emits no stale selection spans");
            }
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "publication-blocked mid-drag surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 3);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "publication-blocked mid-drag press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> hidden_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(hidden_snapshot != nullptr &&
            snapshot_contains_text(*hidden_snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*hidden_snapshot, QStringLiteral("held")),
            "publication-blocked mid-drag keeps held output unpublished");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "publication-blocked mid-drag move is accepted without selecting held output");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "publication-blocked mid-drag release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "publication-blocked mid-drag creates no local selection");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "publication-blocked mid-drag exposes no held selected text");
        ok &= check(final_snapshot != nullptr && final_snapshot->selection_spans.empty(),
            "publication-blocked mid-drag emits no selection spans");
        ok &= check(final_snapshot != nullptr &&
            snapshot_contains_text(*final_snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*final_snapshot, QStringLiteral("held")),
            "publication-blocked mid-drag keeps held output unpublished after release");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> released_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(released_snapshot != nullptr &&
            snapshot_contains_text(*released_snapshot, QStringLiteral("held")) &&
            released_snapshot->selection_spans.empty(),
            "publication-blocked mid-drag publishes held output only after explicit release");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "publication-blocked retained-payload surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 3);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "publication-blocked retained-payload press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "publication-blocked retained-payload initial move is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(fixture.surface.selected_text() == QStringLiteral("visi"),
            "publication-blocked retained-payload captures selected text");
        ok &= check(selected_snapshot != nullptr &&
            selected_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*selected_snapshot, 0, 0, 4),
            "publication-blocked retained-payload initially publishes a selection span");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "publication-blocked retained-payload release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "publication-blocked retained-payload keeps payload active while blocked");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("visi"),
            "publication-blocked retained-payload keeps selected text while blocked");
        const std::shared_ptr<const term::Terminal_render_snapshot> blocked_payload_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(blocked_payload_snapshot != nullptr &&
            snapshot_contains_text(*blocked_payload_snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*blocked_payload_snapshot, QStringLiteral("held")) &&
            blocked_payload_snapshot->selection_spans.empty(),
            "publication-blocked retained-payload clears public spans while held output stays hidden");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> released_payload_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(released_payload_snapshot != nullptr &&
            snapshot_contains_text(*released_payload_snapshot, QStringLiteral("visible")) &&
            snapshot_contains_text(*released_payload_snapshot, QStringLiteral("held")) &&
            released_payload_snapshot->selection_spans.empty(),
            "publication-blocked retained-payload releases without stale selection spans");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("visi"),
            "publication-blocked retained-payload preserves selected text after release");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(4);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "dirty-scrollback selected-row surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                         &&
            anchor_snapshot->grid_size.columns > 5             &&
            anchor_snapshot->viewport.scrollback_rows == 0     &&
            snapshot_row_text(*anchor_snapshot, 0) == QStringLiteral("alpha");
        ok &= check(usable_anchor_snapshot,
            "dirty-scrollback selected-row fixture starts with selected row on primary screen");

        if (usable_anchor_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "dirty-scrollback selected-row press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "dirty-scrollback selected-row move is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "dirty-scrollback selected-row release is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(selected_snapshot != nullptr &&
                selected_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(*selected_snapshot, 0, 0, 5),
                "dirty-scrollback selected-row initially publishes committed selection span");

            QByteArray dirty_then_scroll = QByteArrayLiteral("\x1b[1;1Hbravo\x1b[");
            dirty_then_scroll += QByteArray::number(fixture.surface.rows());
            dirty_then_scroll += QByteArrayLiteral(";1H\r\n");
            backend_ptr->emit_output(std::move(dirty_then_scroll));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);

            const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(tail_snapshot != nullptr &&
                tail_snapshot->viewport.scrollback_rows == 1,
                "dirty-scrollback selected-row output grows scrollback in one result");
            ok &= check(fixture.surface.scroll_to_offset_from_tail(1),
                "dirty-scrollback selected-row scrolls viewport back to mutated scrollback row");

            const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(scrolled_snapshot != nullptr &&
                scrolled_snapshot->viewport.offset_from_tail == 1 &&
                snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("bravo") &&
                scrolled_snapshot->selection_spans.empty(),
                "dirty-scrollback selected-row does not preserve stale selection styling");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(4);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized dirty-scrollback selected-row surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> anchor_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const bool usable_anchor_snapshot =
            anchor_snapshot != nullptr                         &&
            anchor_snapshot->grid_size.columns > 5             &&
            anchor_snapshot->viewport.scrollback_rows == 0     &&
            fixture.surface.rows() > 1                         &&
            snapshot_row_text(*anchor_snapshot, 0) == QStringLiteral("alpha");
        ok &= check(usable_anchor_snapshot,
            "synchronized dirty-scrollback fixture starts with selected row on primary screen");

        if (usable_anchor_snapshot) {
            const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "synchronized dirty-scrollback selected-row press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "synchronized dirty-scrollback selected-row move is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "synchronized dirty-scrollback selected-row release is accepted");

            const std::shared_ptr<const term::Terminal_render_snapshot> selected_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            const std::uint64_t selected_sequence =
                selected_snapshot != nullptr ? selected_snapshot->metadata.sequence : 0U;
            ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
                "synchronized dirty-scrollback captures selected-row payload");
            ok &= check(selected_snapshot != nullptr &&
                selected_snapshot->selection_spans.size() == 1U &&
                snapshot_has_selection_span(*selected_snapshot, 0, 0, 5),
                "synchronized dirty-scrollback initially publishes committed selection span");

            backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[1;1Hbravo"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> hidden_dirty_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(hidden_dirty_snapshot != nullptr &&
                hidden_dirty_snapshot->metadata.sequence == selected_sequence &&
                snapshot_row_text(*hidden_dirty_snapshot, 0) == QStringLiteral("alpha") &&
                hidden_dirty_snapshot->selection_spans.size() == 1U,
                "synchronized dirty-scrollback keeps selected-row rewrite hidden");

            QByteArray hidden_scroll = QByteArrayLiteral("\x1b[");
            hidden_scroll += QByteArray::number(fixture.surface.rows());
            hidden_scroll += QByteArrayLiteral(";1H\r\n");
            backend_ptr->emit_output(std::move(hidden_scroll));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> hidden_scroll_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(hidden_scroll_snapshot != nullptr &&
                hidden_scroll_snapshot->metadata.sequence == selected_sequence &&
                snapshot_row_text(*hidden_scroll_snapshot, 0) == QStringLiteral("alpha"),
                "synchronized dirty-scrollback keeps hidden scrollback unpublished");

            backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
            term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
                fixture.surface);
            const std::shared_ptr<const term::Terminal_render_snapshot> released_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(released_snapshot != nullptr &&
                released_snapshot->viewport.scrollback_rows == 1,
                "synchronized dirty-scrollback release publishes hidden scrollback growth");
            ok &= check(fixture.surface.selection_state() ==
                VNM_TerminalSurface::Selection_state::ACTIVE,
                "synchronized dirty-scrollback keeps retained payload active");
            ok &= check(fixture.surface.selected_text() == QStringLiteral("alpha"),
                "synchronized dirty-scrollback keeps retained selected-row payload");
            ok &= check(fixture.surface.scroll_to_offset_from_tail(1),
                "synchronized dirty-scrollback scrolls viewport back to rewritten row");

            const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(scrolled_snapshot != nullptr &&
                scrolled_snapshot->viewport.offset_from_tail == 1 &&
                snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("bravo") &&
                scrolled_snapshot->selection_spans.empty(),
                "synchronized dirty-scrollback release does not preserve stale selection styling");
        }
    }

    return ok;
}

bool test_stale_synchronized_output_recovery(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        fixture.surface.set_synchronized_output_stale_timeout_ms(25);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible ")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "stale synchronized-output recovery surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "stale synchronized-output recovery keeps held output hidden initially");
        const std::uint64_t hidden_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        ok &= check(pump_until(app, [&] {
            snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            return
                snapshot != nullptr                                       &&
                snapshot_contains_text(*snapshot, QStringLiteral("held")) &&
                snapshot->metadata.sequence > hidden_sequence;
        }),
            "stale synchronized-output recovery force releases held output after timeout");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_synchronized_output_stale_timeout_ms(25);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible ")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "explicit synchronized-output release surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            !snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "explicit synchronized-output release keeps held output hidden initially");
        const std::uint64_t hidden_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        snapshot = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("held")) &&
            snapshot->metadata.sequence > hidden_sequence,
            "explicit synchronized-output release publishes held output");
        const std::uint64_t released_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        pump_events(app, 8);
        snapshot = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->metadata.sequence == released_sequence &&
            snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "explicit synchronized-output release does not publish again after stale timeout");
    }

    return ok;
}

bool test_synchronized_output_styled_blank_rows_preserve_background(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("visible")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started && backend_ptr != nullptr,
        "synchronized styled blank background surface starts");
    if (!started || backend_ptr == nullptr) {
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
            snapshot_contains_text(*baseline_snapshot, QStringLiteral("visible")),
        "synchronized styled blank background publishes baseline");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    constexpr quint32 k_gray_background = 0xff404040U;
    backend_ptr->emit_output(QByteArrayLiteral(
        "\x1b[?2026h"
        "\x1b[48;2;64;64;64m"
        "\x1b[1;1H\x1b[2K"
        "\x1b[2;1H\x1b[2K"
        "\x1b[3;1H> hi\x1b[K"
        "\x1b[0m"
        "\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);

    ok &= check(pump_until(app, [&] {
            const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            return
                snapshot != nullptr &&
                snapshot->metadata.sequence > baseline_snapshot->metadata.sequence &&
                snapshot_row_text(*snapshot, 2) == QStringLiteral("> hi");
        }),
        "synchronized styled blank background publishes released prompt rows");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr,
        "synchronized styled blank background has released snapshot");
    if (snapshot == nullptr) {
        return ok;
    }

    ok &= check(snapshot_dirty_ranges_contain_row(*snapshot, 0) &&
            snapshot_dirty_ranges_contain_row(*snapshot, 1) &&
            snapshot_dirty_ranges_contain_row(*snapshot, 2),
        "synchronized styled blank background marks every gray row dirty");
    ok &= check(snapshot_row_has_rgb_background(
            *snapshot,
            0,
            0,
            snapshot->grid_size.columns,
            k_gray_background),
        "synchronized styled blank background preserves first blank row background");
    ok &= check(snapshot_row_has_rgb_background(
            *snapshot,
            1,
            0,
            snapshot->grid_size.columns,
            k_gray_background),
        "synchronized styled blank background preserves second blank row background");
    ok &= check(snapshot_row_has_rgb_background(
            *snapshot,
            2,
            0,
            snapshot->grid_size.columns,
            k_gray_background),
        "synchronized styled blank background preserves prompt row background");

    return ok;
}

bool test_stale_synchronized_output_recovery_force_releases_after_budgeted_catchup(
    QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int title_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "budgeted stale recovery surface starts");
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_before_release =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const std::uint64_t publication_generation_before_release =
        snapshot_before_release == nullptr ?
            0U :
            snapshot_before_release->metadata.publication_generation;

    QByteArray output = QByteArrayLiteral("\x1b[?2026h");
    output += QByteArrayLiteral("\x1b]2;surface-stale-partial-title\a");
    for (int i = 0; i < 180; ++i) {
        output += QByteArrayLiteral("\r\nsurface-repaint-fill");
    }
    output += QByteArrayLiteral("\x1b[Hsurface-held");
    for (int i = 0; i < 500; ++i) {
        output += QByteArrayLiteral("\r\nsurface-tail-fill");
    }
    output += QByteArrayLiteral("\r\nsurface-tail-after-timeout");

    backend_ptr->emit_output(output);
    term::VNM_TerminalSurface_render_bridge::handle_synchronized_output_recovery_timeout(
        fixture.surface,
        std::chrono::steady_clock::duration::zero());

    const std::shared_ptr<const term::Terminal_render_snapshot> released_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(released_snapshot != nullptr &&
        snapshot_contains_text(*released_snapshot, QStringLiteral("surface-held")),
        "budgeted stale recovery force-releases held synchronized output after one catch-up slice");
    ok &= check(released_snapshot != nullptr &&
        !snapshot_contains_text(*released_snapshot, QStringLiteral("surface-tail-after-timeout")),
        "budgeted stale recovery does not drain the full backend output during timeout");
    ok &= check(released_snapshot != nullptr &&
        released_snapshot->metadata.publication_generation >
            publication_generation_before_release,
        "budgeted stale recovery publishes force-released held output");
    ok &= check_int_equal(title_changed_count, 0,
        "budgeted stale recovery defers partial-drain title notifications");
    ok &= check(term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
        fixture.surface),
        "budgeted stale recovery reposts remaining backend output");

    ok &= check(pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        return snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("surface-tail-after-timeout"));
    }),
        "budgeted stale recovery drains remaining output through posted GUI work");
    ok &= check_int_equal(title_changed_count, 1,
        "budgeted stale recovery delivers deferred title notification after drain boundary");
    ok &= check(fixture.surface.terminal_title() ==
        QStringLiteral("surface-stale-partial-title"),
        "budgeted stale recovery preserves deferred title notification payload");

    return ok;
}

bool test_paste_text_public_method_and_policy(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        ok &= check(!fixture.surface.paste_text(QStringLiteral("no-session")),
            "no-session paste_text returns false");
        ok &= check(error_codes.empty(),
            "no-session paste_text emits no backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "paste policy surface starts");

        const std::size_t initial_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("plain\r\ntext")),
            "application-controlled paste without DECSET returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, initial_write_index) ==
            QByteArrayLiteral("plain\ntext"),
            "application-controlled paste without DECSET writes unframed sanitized text");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2004h"));
        const std::size_t framed_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("mode")),
            "application-controlled paste after pending DECSET returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, framed_write_index) ==
            framed_paste(QByteArrayLiteral("mode")),
            "surface paste_text drains pending DECSET before policy mapping");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t disabled_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("disabled")),
            "disabled surface paste returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, disabled_write_index) ==
            QByteArrayLiteral("disabled"),
            "disabled surface paste stays unframed even in terminal bracketed mode");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::ENABLED);
        const std::size_t enabled_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("enabled")),
            "enabled surface paste returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, enabled_write_index) ==
            framed_paste(QByteArrayLiteral("enabled")),
            "enabled surface paste always frames");

        const std::size_t empty_write_index = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QString(QChar(0x001b))),
            "surface paste_text returns false when sanitization removes the body");
        ok &= check(backend_ptr->writes.size() == empty_write_index,
            "surface paste_text sends no frame for sanitized-empty paste");
        ok &= check(error_codes.empty(),
            "surface paste success path emits no backend_error");

        fixture.surface.set_clipboard_text_reader([]() -> std::optional<QString> {
            return QStringLiteral("reader-paste");
        });
        const std::size_t reader_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_clipboard_text(),
            "surface paste_clipboard_text returns true when the reader supplies text");
        ok &= check(joined_writes_since(backend_ptr->writes, reader_write_index) ==
            framed_paste(QByteArrayLiteral("reader-paste")),
            "surface paste_clipboard_text writes injected clipboard text");

        fixture.surface.set_clipboard_text_reader([]() -> std::optional<QString> {
            return std::nullopt;
        });
        const std::size_t blocked_reader_write_count = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_clipboard_text(),
            "surface paste_clipboard_text returns false when the reader has no text");
        ok &= check(backend_ptr->writes.size() == blocked_reader_write_count,
            "surface paste_clipboard_text writes nothing when the reader has no text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit paste surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);
        ok &= check(error_codes.empty(),
            "post-exit setup emits no backend_error before paste");

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QStringLiteral("after")),
            "post-exit paste_text returns false");
        ok &= check(backend_ptr->writes.size() == write_count,
            "post-exit paste_text does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "post-exit paste_text reports WRITE_FAILED");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->reject_writes = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "write-rejected paste surface starts");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QStringLiteral("rejected")),
            "backend-rejected paste_text returns false");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "backend-rejected paste_text reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected paste_text reports WRITE_FAILED");
    }

    return ok;
}

bool test_right_click_paste_and_mouse_reporting_precedence(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("right-paste"), QClipboard::Clipboard);

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "right-click paste surface starts");

        fixture.surface.set_clipboard_text_reader([]() -> std::optional<QString> {
            return QStringLiteral("reader-right-paste");
        });
        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "right-click paste press is accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "right-click paste writes clipboard text");
        if (backend_ptr->writes.size() > write_count) {
            ok &= check_bytes_equal(
                backend_ptr->writes.back(),
                QByteArrayLiteral("reader-right-paste"),
                "right-click paste writes injected unframed clipboard text");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "right-click mouse-reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "right-click mouse-reporting press is accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "right-click mouse reporting writes one event");
        if (backend_ptr->writes.size() > write_count) {
            ok &= check_bytes_equal(
                backend_ptr->writes.back(),
                sgr_mouse_report(2, 0, 0, 'M'),
                "right-click mouse reporting takes precedence over paste");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "legacy right-click mouse-reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000h"));
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "legacy mouse-reporting right-click is terminal-owned");
        ok &= check(backend_ptr->writes.size() == write_count,
            "legacy mouse-reporting right-click does not paste clipboard text");
    }

    return ok;
}

bool test_focus_reporting_writes_mode_bytes_and_preserves_ime_cancel(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            error_codes.push_back(code);
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "focus reporting surface starts");

    QQuickItem other_item;
    other_item.setParentItem(fixture.window.contentItem());
    other_item.setFocus(true);

    const auto focus_surface = [&]() {
        fixture.surface.forceActiveFocus();
        return pump_until(app, [&] { return fixture.surface.hasActiveFocus(); });
    };
    const auto focus_other = [&]() {
        other_item.forceActiveFocus();
        return pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); });
    };

    ok &= check(focus_other(),
        "focus reporting fixture starts from inactive surface focus");

    const std::size_t disabled_write_count = backend_ptr->writes.size();
    ok &= check(focus_surface(),
        "disabled focus reporting surface receives focus");
    ok &= check(backend_ptr->writes.size() == disabled_write_count,
        "disabled focus-in reporting writes no backend bytes");
    ok &= check(focus_other(),
        "disabled focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == disabled_write_count,
        "disabled focus-out reporting writes no backend bytes");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
    const std::size_t focus_in_index = backend_ptr->writes.size();
    ok &= check(focus_surface(),
        "enabled focus reporting surface receives focus");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        focus_in_index,
        { QByteArrayLiteral("\x1b[I") },
        "enabled surface focus-in writes CSI I");

    backend_ptr->emit_output(numbered_scroll_lines(80));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "focus reporting scrollback fixture detaches viewport");
    std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "focus reporting fixture starts from detached surface viewport");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        4,
        "focus reporting IME preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active,
        "focus reporting IME preedit starts active");

    const std::size_t focus_out_index = backend_ptr->writes.size();
    const std::shared_ptr<const term::Terminal_render_snapshot> before_focus_out_snapshot =
        detached_snapshot;
    ok &= check(focus_other(),
        "enabled focus reporting surface loses focus");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        focus_out_index,
        { QByteArrayLiteral("\x1b[O") },
        "enabled surface focus-out writes CSI O");
    ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "focus-out reporting still cancels IME preedit");
    detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot == before_focus_out_snapshot,
        "surface focus-out report does not publish a replacement snapshot");
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "surface focus-out report preserves detached viewport");

    const std::size_t detached_focus_in_index = backend_ptr->writes.size();
    const std::shared_ptr<const term::Terminal_render_snapshot> before_focus_in_snapshot =
        detached_snapshot;
    ok &= check(focus_surface(),
        "enabled focus reporting surface regains focus while viewport is detached");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        detached_focus_in_index,
        { QByteArrayLiteral("\x1b[I") },
        "enabled detached surface focus-in writes CSI I");
    detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot == before_focus_in_snapshot,
        "surface focus-in report does not publish a replacement snapshot");
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "surface focus-in report preserves detached viewport");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004l"));
    const std::size_t reset_write_count = backend_ptr->writes.size();
    ok &= check(focus_other(),
        "reset focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == reset_write_count,
        "surface focus-out after DECRST writes no backend bytes");
    ok &= check(focus_surface(),
        "reset focus reporting surface receives focus");
    ok &= check(backend_ptr->writes.size() == reset_write_count,
        "surface focus-in after DECRST writes no backend bytes");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    pump_events(app);
    const std::size_t exited_write_count = backend_ptr->writes.size();
    ok &= check(focus_other(),
        "post-exit focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == exited_write_count,
        "post-exit focus reporting writes no backend bytes");
    ok &= check(error_codes.empty(),
        "focus reporting success and no-op paths emit no backend_error");

    {
        Surface_fixture rejected_fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> rejected_error_codes;
        QObject::connect(
            &rejected_fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &rejected_fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                rejected_error_codes.push_back(code);
            });

        auto rejected_backend = std::make_unique<Scripted_backend>();
        rejected_backend->reject_writes = true;
        bool rejected_started = false;
        Scripted_backend* rejected_backend_ptr = start_surface_with_backend(
            rejected_fixture.surface,
            std::move(rejected_backend),
            { QStringLiteral("scripted-terminal") },
            &rejected_started);
        ok &= check(rejected_started, "rejected focus reporting surface starts");

        QQuickItem rejected_other_item;
        rejected_other_item.setParentItem(rejected_fixture.window.contentItem());
        rejected_other_item.setFocus(true);

        rejected_backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
        const std::size_t rejected_focus_index = rejected_backend_ptr->writes.size();
        rejected_fixture.surface.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return rejected_fixture.surface.hasActiveFocus(); }),
            "backend-rejected focus reporting surface receives focus");
        ok &= check_write_chunks_equal(
            rejected_backend_ptr->writes,
            rejected_focus_index,
            { QByteArrayLiteral("\x1b[I") },
            "backend-rejected focus reporting reaches backend write");
        ok &= check(rejected_error_codes.size() == 1U &&
            rejected_error_codes.front() ==
                VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected focus reporting reports one WRITE_FAILED backend_error");
    }

    return ok;
}

bool test_ime_preedit_updates_overlay_without_snapshot_churn(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME preedit surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\r\norder-before-ime"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_before =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("compose"),
        3,
        "IME preedit event is accepted");

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("compose") &&
        ime_preedit.cursor_position == 3,
        "IME preedit appears in surface overlay state");
    ok &= check(snapshot == snapshot_before,
        "IME preedit update does not replace the latest render snapshot");
    ok &= check(stats_after.update_requests > stats_before.update_requests,
        "IME preedit update requests a render update");
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("order-before-ime")),
        "terminal output snapshot remains available after IME preedit");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("compose")),
        "IME preedit does not mutate screen model cells");
    ok &= check(backend_ptr->writes.size() == write_count,
        "IME preedit update produces no backend writes");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("replacement"),
        5,
        "IME replacement preedit event is accepted");
    const term::Ime_preedit_state replacement_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(replacement_preedit.active &&
        replacement_preedit.text == QStringLiteral("replacement") &&
        replacement_preedit.cursor_position == 5,
        "IME preedit replacement updates text and cursor");
    ok &= check(term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface) ==
        snapshot_before,
        "IME preedit replacement still keeps the latest render snapshot");
    ok &= check(backend_ptr->writes.size() == write_count,
        "IME preedit replacement produces no backend writes");

    backend_ptr->emit_output(QByteArrayLiteral("\r\nmodel-after-preedit"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_with_preedit =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot_with_preedit != nullptr &&
        snapshot_with_preedit != snapshot_before &&
        snapshot_contains_text(*snapshot_with_preedit, QStringLiteral("model-after-preedit")),
        "terminal output still publishes a model snapshot while IME preedit is active");
    ok &= check(snapshot_with_preedit != nullptr &&
        snapshot_with_preedit->ime_preedit.active,
        "model snapshots may carry current IME preedit for compatibility");

    const term::Terminal_surface_render_invalidation_stats_t cancel_stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= send_empty_ime_event(
        fixture.surface,
        "IME preedit cancel event is accepted");
    const term::Ime_preedit_state canceled_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t cancel_stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(!canceled_preedit.active && canceled_preedit.text.isEmpty(),
        "IME cancel clears authoritative overlay state");
    ok &= check(term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface) ==
        snapshot_with_preedit,
        "IME cancel does not replace a model snapshot that carried preedit compatibility state");
    ok &= check(cancel_stats_after.update_requests > cancel_stats_before.update_requests,
        "IME cancel requests a render update");

    return ok;
}

bool test_ime_commit_writes_utf8_and_clears_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME commit surface starts");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        2,
        "IME preedit before commit is accepted");

    const std::size_t write_count     = backend_ptr->writes.size();
    const QByteArray  expected_commit = bytes_from_hex("e7958c");
    ok &= send_ime_commit(
        fixture.surface,
        QString::fromUtf8(expected_commit),
        "IME commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME commit writes one backend chunk");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            expected_commit,
            "IME commit writes UTF-8 bytes");
    }

    const term::Ime_preedit_state commit_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(!commit_preedit.active && commit_preedit.text.isEmpty(),
        "IME commit clears preedit overlay state");

    const std::size_t empty_commit_write_count = backend_ptr->writes.size();
    ok &= send_empty_ime_commit(fixture.surface, "empty IME commit event is accepted");
    ok &= check(backend_ptr->writes.size() == empty_commit_write_count,
        "empty IME commit produces no backend writes");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("to-cancel"),
        4,
        "IME preedit before empty event is accepted");
    const std::size_t empty_event_write_count = backend_ptr->writes.size();
    ok &= send_empty_ime_event(fixture.surface, "empty IME event is accepted");
    const term::Ime_preedit_state empty_event_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(backend_ptr->writes.size() == empty_event_write_count,
        "empty IME event produces no backend writes");
    ok &= check(!empty_event_preedit.active &&
        empty_event_preedit.text.isEmpty(),
        "empty IME event cancels active preedit");
    ok &= check(backend_error_count == 0,
        "IME commit success path emits no backend_error");

    return ok;
}

bool test_ime_no_session_overlay_and_later_start_clear(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    const term::Terminal_surface_render_invalidation_stats_t stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("local"),
        3,
        "no-session IME preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("local") &&
        ime_preedit.cursor_position == 3,
        "no-session IME preedit updates local overlay");
    ok &= check(stats_after.update_requests > stats_before.update_requests,
        "no-session IME preedit requests a render update");

    ok          &= send_ime_commit(
        fixture.surface,
        QStringLiteral("ignored"),
        "no-session IME commit event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session IME commit clears local overlay");

    ok          &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("empty-commit"),
        6,
        "no-session empty-commit preedit event is accepted");
    ok          &= send_empty_ime_commit(
        fixture.surface,
        "no-session empty IME commit event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session empty commit clears local overlay");

    ok          &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("plain-empty"),
        5,
        "no-session plain-empty preedit event is accepted");
    ok          &= send_empty_ime_event(
        fixture.surface,
        "no-session plain empty IME event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session plain empty event clears local overlay");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("stray"),
        2,
        "no-session stray preedit event is accepted");

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "no-session IME later start succeeds");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "later start clears no-session IME overlay");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("stray")),
        "later start has no stray no-session preedit in model snapshot");
    ok &= check(backend_error_count == 0,
        "no-session IME events emit no backend_error");

    return ok;
}

bool test_ime_startup_commit_clears_preedit_without_backend_error(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    backend->on_start             = [&] {
        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("startup"),
            4,
            "startup IME preedit event is accepted");
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("ignored"),
            "startup IME commit event is accepted");
    };
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "startup IME surface starts");

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(backend_ptr->writes.empty(),
        "startup IME commit writes no backend bytes");
    ok &= check(backend_error_count == 0,
        "startup IME commit reports no backend_error");
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "startup IME commit does not ghost an old preedit");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("startup")),
        "startup IME preedit does not leak into model snapshot");

    return ok;
}

bool test_ime_multi_codepoint_commit_writes_utf8(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME multi-codepoint commit surface starts");

    const QByteArray  expected_commit = bytes_from_hex("41e7958cf09f9880");
    const std::size_t write_count     = backend_ptr->writes.size();
    ok &= send_ime_commit(
        fixture.surface,
        QString::fromUtf8(expected_commit),
        "IME multi-codepoint commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME multi-codepoint commit writes one backend chunk");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            expected_commit,
            "IME multi-codepoint commit writes exact UTF-8");
    }

    return ok;
}

bool test_ime_combined_commit_and_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME combined commit/preedit surface starts");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("previous"),
        1,
        "IME combined previous preedit is accepted");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_event(
        fixture.surface,
        QStringLiteral("go"),
        QStringLiteral("next"),
        2,
        0,
        0,
        "IME combined commit/preedit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME combined event writes commit bytes");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            QByteArrayLiteral("go"),
            "IME combined event writes exact commit bytes");
    }

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("next") &&
        ime_preedit.cursor_position == 2,
        "IME combined event leaves replacement preedit active");

    return ok;
}

bool test_ime_replacement_range_is_stream_commit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME replacement-range surface starts");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_event(
        fixture.surface,
        QStringLiteral("range"),
        {},
        0,
        -2,
        3,
        "IME replacement-range commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME replacement-range commit writes one backend chunk");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        write_count,
        { QByteArrayLiteral("range") },
        "IME replacement range is treated as stream commit text");

    return ok;
}

bool test_ime_commit_failure_preserves_preedit(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("prompt")};
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "IME post-exit failure surface starts");

        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("pending"),
            3,
            "IME post-exit failure preedit is accepted");
        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("x"),
            "IME post-exit commit event is accepted");
        const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
            fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count,
            "IME post-exit commit does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "IME post-exit commit reports WRITE_FAILED");
        ok &= check(ime_preedit.active &&
            ime_preedit.text == QStringLiteral("pending"),
            "IME post-exit commit preserves active preedit");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("prompt")};
        backend->reject_writes        = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "IME write-rejection failure surface starts");

        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("pending"),
            3,
            "IME write-rejection failure preedit is accepted");
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("x"),
            "IME write-rejection commit event is accepted");
        const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
            fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "IME write-rejection commit reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "IME write-rejection commit reports WRITE_FAILED");
        ok &= check(ime_preedit.active &&
            ime_preedit.text == QStringLiteral("pending"),
            "IME write-rejection commit preserves active preedit");
    }

    return ok;
}

bool test_ime_synchronized_output_overlay(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("A")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME synchronized-output surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hB"));
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("sync"),
        2,
        "IME synchronized-output preedit event is accepted");

    std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("sync"),
        "IME preedit is visible during synchronized output");
    ok &= check(snapshot != nullptr &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("A"),
        "IME preedit during synchronized output does not expose held output");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026lC"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    snapshot     = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(snapshot != nullptr &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("ABC") &&
        ime_preedit.active &&
        ime_preedit.text == QStringLiteral("sync"),
        "synchronized output release preserves active IME overlay");

    ok          &= send_empty_ime_event(
        fixture.surface,
        "IME synchronized-output empty event is accepted");
    snapshot     = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(snapshot != nullptr &&
        !ime_preedit.active &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("ABC"),
        "IME cancel after synchronized output release clears overlay");

    return ok;
}

bool test_ime_focus_loss_cancels_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME focus-loss surface starts");

    fixture.surface.forceActiveFocus();
    ok &= check(pump_until(app, [&] { return fixture.surface.hasActiveFocus(); }),
        "IME focus-loss surface receives active focus");
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        4,
        "IME focus-loss preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active,
        "IME focus-loss test starts with active preedit");

    QQuickItem other_item;
    other_item.setParentItem(fixture.window.contentItem());
    other_item.setFocus(true);
    other_item.forceActiveFocus();
    ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
        "IME focus-loss surface loses active focus");

    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(!ime_preedit.active &&
        ime_preedit.text.isEmpty(),
        "IME focus loss cancels active preedit");

    return ok;
}

bool test_ime_cursor_rectangle_query_tracks_snapshot_cursor(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("A")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME cursor-rectangle surface starts");
    ok &= check(fixture.surface.inputMethodQuery(Qt::ImEnabled).toBool(),
        "IME cursor rectangle surface reports input method enabled");

    const QRectF first_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!first_rect.isEmpty(),
        "IME cursor rectangle query returns a non-empty rect");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[3;6H"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);

    const QRectF moved_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!moved_rect.isEmpty(),
        "IME moved cursor rectangle query returns a non-empty rect");
    ok &= check(!nearly_equal(moved_rect.left(), first_rect.left()) ||
        !nearly_equal(moved_rect.top(), first_rect.top()),
        "IME cursor rectangle changes after cursor movement");

    const qreal cell_width  = moved_rect.width();
    const qreal cell_height = moved_rect.height();
    ok &= check_rect_near(
        moved_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle matches CUP 3;6 cell geometry");

    ok &= send_ime_preedit(
        fixture.surface,
        QString::fromUtf8("A\xe7\x95\x8c" "B"),
        2,
        "IME cursor rectangle preedit offset event is accepted");
    const QRectF preedit_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check_rect_near(
        preedit_rect,
        QRectF(8.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle includes active preedit display width");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("abc"),
        -20,
        "IME negative cursor preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF negative_clamped_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(ime_preedit.cursor_position == 0,
        "IME negative preedit cursor position clamps to start");
    ok &= check_rect_near(
        negative_clamped_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME negative preedit cursor clamp keeps rectangle at terminal cursor");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("abc"),
        200,
        "IME over-length cursor preedit event is accepted");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF over_length_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(ime_preedit.cursor_position == 3,
        "IME over-length preedit cursor position clamps to text length");
    ok &= check_rect_near(
        over_length_rect,
        QRectF(8.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME over-length preedit cursor clamp places rectangle after text");

    ok &= send_empty_ime_event(
        fixture.surface,
        "IME cursor rectangle clear event is accepted");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF cleared_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "IME cursor rectangle clear resets overlay state");
    ok &= check_rect_near(
        cleared_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle clear resets to terminal cursor");

    return ok;
}

bool test_keyboard_cursor_modes(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard cursor mode surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Up, Qt::NoModifier,
        {}, bytes_from_hex("1b5b41"),
        "normal Up writes CSI A");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Down, Qt::NoModifier,
        {}, bytes_from_hex("1b5b42"),
        "normal Down writes CSI B");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Right, Qt::NoModifier,
        {}, bytes_from_hex("1b5b43"),
        "normal Right writes CSI C");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::NoModifier,
        {}, bytes_from_hex("1b5b44"),
        "normal Left writes CSI D");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::NoModifier,
        {}, bytes_from_hex("1b4f44"),
        "application cursor Left drains pending mode output and writes SS3 D");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b313b3244"),
        "modified Left ignores application cursor mode");
    ok &= check(backend_error_count == 0,
        "keyboard cursor success path emits no backend_error");

    return ok;
}

bool test_keyboard_navigation_keys(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard navigation surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Home, Qt::NoModifier,
        {}, bytes_from_hex("1b5b48"),
        "Home writes CSI H");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_End, Qt::NoModifier,
        {}, bytes_from_hex("1b5b46"),
        "End writes CSI F");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Insert, Qt::NoModifier,
        {}, bytes_from_hex("1b5b327e"),
        "Insert writes CSI 2 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Delete, Qt::NoModifier,
        {}, bytes_from_hex("1b5b337e"),
        "Delete writes CSI 3 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_PageUp, Qt::NoModifier,
        {}, bytes_from_hex("1b5b357e"),
        "PageUp writes CSI 5 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_PageDown, Qt::NoModifier,
        {}, bytes_from_hex("1b5b367e"),
        "PageDown writes CSI 6 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backtab, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b5a"),
        "Shift+Tab writes CSI Z");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Tab, Qt::ShiftModifier,
        QStringLiteral("\t"), bytes_from_hex("1b5b5a"),
        "Shift+Tab through Key_Tab writes CSI Z");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Home,
        Qt::ShiftModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b313b3648"),
        "modified Home writes CSI 1;6 H");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_End, Qt::AltModifier,
        {}, bytes_from_hex("1b5b313b3346"),
        "modified End writes CSI 1;3 F");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Delete,
        Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b333b387e"),
        "modified Delete writes CSI 3;8 tilde");
    ok &= check(backend_error_count == 0,
        "keyboard navigation success path emits no backend_error");

    return ok;
}

bool test_keyboard_function_keys(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard function surface starts");

    const struct {
        int         key;
        const char* expected_hex;
        const char* message;
    } unmodified_cases[] = {
        { Qt::Key_F1,  "1b4f50",     "F1 writes SS3 P"         },
        { Qt::Key_F2,  "1b4f51",     "F2 writes SS3 Q"         },
        { Qt::Key_F3,  "1b4f52",     "F3 writes SS3 R"         },
        { Qt::Key_F4,  "1b4f53",     "F4 writes SS3 S"         },
        { Qt::Key_F5,  "1b5b31357e", "F5 writes CSI 15 tilde"  },
        { Qt::Key_F6,  "1b5b31377e", "F6 writes CSI 17 tilde"  },
        { Qt::Key_F7,  "1b5b31387e", "F7 writes CSI 18 tilde"  },
        { Qt::Key_F8,  "1b5b31397e", "F8 writes CSI 19 tilde"  },
        { Qt::Key_F9,  "1b5b32307e", "F9 writes CSI 20 tilde"  },
        { Qt::Key_F10, "1b5b32317e", "F10 writes CSI 21 tilde" },
        { Qt::Key_F11, "1b5b32337e", "F11 writes CSI 23 tilde" },
        { Qt::Key_F12, "1b5b32347e", "F12 writes CSI 24 tilde" },
    };
    for (const auto& key_case : unmodified_cases) {
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, key_case.key, Qt::NoModifier,
            {}, bytes_from_hex(key_case.expected_hex),
            key_case.message);
    }

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F1, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b313b3250"),
        "modified F1 writes CSI 1;2 P");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F5,
        Qt::ControlModifier | Qt::AltModifier,
        {}, bytes_from_hex("1b5b31353b377e"),
        "modified F5 writes CSI 15;7 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F12,
        Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b32343b387e"),
        "modified F12 writes CSI 24;8 tilde");
    ok &= check(backend_error_count == 0,
        "keyboard function success path emits no backend_error");

    return ok;
}

bool test_keyboard_keypad_modes(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard keypad surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "normal keypad digit writes text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Plus, Qt::KeypadModifier,
        QStringLiteral("+"), bytes_from_hex("2b"),
        "normal keypad plus writes text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Enter, Qt::KeypadModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "normal keypad Enter writes carriage return");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b="));

    const struct {
        int         key;
        QString     text;
        const char* expected_hex;
        const char* message;
    } application_cases[] = {
        { Qt::Key_0,        QStringLiteral("0"),  "1b4f70", "application keypad 0"        },
        { Qt::Key_9,        QStringLiteral("9"),  "1b4f79", "application keypad 9"        },
        { Qt::Key_Period,   QStringLiteral("."),  "1b4f6e", "application keypad decimal"  },
        { Qt::Key_Minus,    QStringLiteral("-"),  "1b4f6d", "application keypad minus"    },
        { Qt::Key_Comma,    QStringLiteral(","),  "1b4f6c", "application keypad comma"    },
        { Qt::Key_Plus,     QStringLiteral("+"),  "1b4f6b", "application keypad plus"     },
        { Qt::Key_Asterisk, QStringLiteral("*"),  "1b4f6a", "application keypad multiply" },
        { Qt::Key_Slash,    QStringLiteral("/"),  "1b4f6f", "application keypad divide"   },
        { Qt::Key_Enter,    QStringLiteral("\r"), "1b4f4d", "application keypad enter"    },
        { Qt::Key_Equal,    QStringLiteral("="),  "1b4f58", "application keypad equal"    },
    };
    for (const auto& key_case : application_cases) {
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, key_case.key, Qt::KeypadModifier,
            key_case.text, bytes_from_hex(key_case.expected_hex),
            key_case.message);
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b>"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "ESC > resets keypad digit to normal text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Plus, Qt::KeypadModifier,
        QStringLiteral("+"), bytes_from_hex("2b"),
        "ESC > resets keypad operator to normal text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Enter, Qt::KeypadModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "ESC > resets keypad Enter to carriage return");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?66h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("1b4f75"),
        "DECNKM set makes keypad digit application encoded");
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?66l"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "DECNKM reset restores keypad digit text");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?66h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("1b4f75"),
        "DECNKM under synchronized output still applies immediately to input");

    ok &= check(backend_error_count == 0,
        "keyboard keypad success path emits no backend_error");

    return ok;
}

bool test_keyboard_focus_routed_delivery(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "focus-routed keyboard surface starts");

    ok &= send_window_key_and_expect_write(
        fixture.surface,
        fixture.window,
        *backend_ptr,
        Qt::Key_Left,
        Qt::NoModifier,
        {},
        bytes_from_hex("1b5b44"),
        "focus-routed window special key event writes through active terminal item");
    ok &= check(backend_error_count == 0,
        "focus-routed keyboard success path emits no backend_error");

    return ok;
}

bool test_keyboard_no_session_and_post_exit_semantics(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);
        int backend_error_count = 0;
        observe_backend_errors(fixture.surface, backend_error_count);

        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "no-session mapped key event is accepted");
        ok &= check(backend_error_count == 0,
            "no-session mapped key event emits no backend_error");

        QKeyEvent ignored_event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
        QCoreApplication::sendEvent(&fixture.surface, &ignored_event);
        ok &= check(!ignored_event.isAccepted(),
            "no-session unmapped key event is ignored");
        ok &= check(backend_error_count == 0,
            "no-session unmapped key event emits no backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit keyboard surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);

        const std::size_t write_count = backend_ptr->writes.size();
        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "post-exit mapped key event remains accepted");
        ok &= check(backend_ptr->writes.size() == write_count,
            "post-exit mapped key event does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "post-exit mapped key event reports one WRITE_FAILED backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->reject_writes = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "write-rejection keyboard surface starts");

        const std::size_t write_count = backend_ptr->writes.size();
        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "backend-rejected mapped key event remains accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "backend-rejected mapped key event reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected mapped key event reports one WRITE_FAILED backend_error");
    }

    return ok;
}

bool test_keyboard_unhandled_key_skips_backend_drain(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "unhandled-key drain surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("ignored-key-output"));

    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
    QCoreApplication::sendEvent(&fixture.surface, &event);
    ok &= check(!event.isAccepted(),
        "unhandled key event is ignored while backend output is pending");

    const std::shared_ptr<const term::Terminal_render_snapshot> pre_pump_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_pump_snapshot == nullptr ||
        !snapshot_contains_text(*pre_pump_snapshot, QStringLiteral("ignored-key-output")),
        "unhandled key event does not drain pending backend output");

    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> post_pump_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_pump_snapshot != nullptr &&
        snapshot_contains_text(*post_pump_snapshot, QStringLiteral("ignored-key-output")),
        "pending backend output still drains through the GUI event path");

    return ok;
}

bool test_worker_callback_drains_on_gui_thread(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int  activity_count         = 0;
    bool activity_on_gui_thread = false;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_activity,
        &fixture.surface,
        [&] {
            ++activity_count;
            activity_on_gui_thread =
                QThread::currentThread() == fixture.surface.thread();
        },
        Qt::DirectConnection);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "worker-callback surface starts");

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("worker-output"));
    backend_ptr->join_worker();
    ok &= check(activity_count == 0,
        "worker callback is not drained inline on the worker thread");
    const std::shared_ptr<const term::Terminal_render_snapshot> pre_gui_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_gui_snapshot == nullptr ||
        !snapshot_contains_text(*pre_gui_snapshot, QStringLiteral("worker")),
        "worker callback output does not mutate the snapshot before GUI events");

    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(activity_count == 1, "worker callback drains after GUI events");
    ok &= check(activity_on_gui_thread, "worker callback notification emits on GUI thread");
    ok &= check(snapshot != nullptr && snapshot_contains_text(*snapshot, QStringLiteral("worker")),
        "worker callback output reaches render snapshot");

    return ok;
}

bool test_hover_move_does_not_drain_queued_backend_output(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_row_timestamp_tooltip_enabled(false);
    pump_events(app);

    int activity_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_activity,
        &fixture.surface,
        [&] {
            ++activity_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[?1003;1006hhover-baseline"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "hover queued-drain surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("hover-baseline")),
        "hover queued-drain publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }
    const int baseline_activity_count = activity_count;
    const std::size_t baseline_write_count = backend_ptr->writes.size();

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("\r\nhover-worker-output"));
    backend_ptr->join_worker();
    ok &= check(activity_count == baseline_activity_count,
        "hover queued-drain does not emit activity on the worker callback thread");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            fixture.surface),
        "hover queued-drain does not queue posted drain work before hover");
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            fixture.surface) > 0U,
        "hover queued-drain has pending backend work before hover");

    ok &= send_hover_move(
        fixture.surface,
        QPointF(1.0, 1.0),
        Qt::NoModifier,
        true,
        "hover queued-drain hover move is accepted while mouse reporting is published");
    ok &= check(backend_ptr->writes.size() == baseline_write_count,
        "hover queued-drain hover move writes no stale mouse report while callbacks are pending");
    const std::shared_ptr<const term::Terminal_render_snapshot> hover_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(hover_snapshot != nullptr &&
        hover_snapshot->metadata.sequence == baseline_snapshot->metadata.sequence &&
        !snapshot_contains_text(*hover_snapshot, QStringLiteral("hover-worker-output")),
        "hover queued-drain hover move does not publish queued backend output");
    ok &= check(activity_count == baseline_activity_count,
        "hover queued-drain hover move does not drain backend callbacks");

    ok &= check(pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        return snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("hover-worker-output"));
    }),
        "hover queued-drain event loop publishes queued backend output without hover");

    return ok;
}

bool test_heap_surface_destroy_closes_queued_worker_callback(QGuiApplication& app)
{
    bool ok = true;

    auto window = std::make_unique<QQuickWindow>();
    window->resize(640, 320);

    auto surface = std::make_unique<VNM_TerminalSurface>();
    QPointer<VNM_TerminalSurface> surface_guard(surface.get());
    surface->setParentItem(window->contentItem());
    surface->setSize(QSizeF(520.0, 240.0));
    surface->set_font_family(QStringLiteral("monospace"));
    surface->set_font_size(12.0);
    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(*surface);
    window->show();
    pump_events(app);

    int output_activity_count = 0;
    int exited_count          = 0;
    int backend_error_count   = 0;
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::output_activity,
        &app,
        [&] {
            ++output_activity_count;
        });
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::process_exited,
        &app,
        [&](VNM_TerminalSurface::Exit_reason, int) {
            ++exited_count;
        });
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::backend_error,
        &app,
        [&](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++backend_error_count;
        });

    auto state   = std::make_shared<Scripted_backend_lifecycle_state>();
    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("heap-lifecycle-output")};
    backend->lifecycle_state      = state;
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        *surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "heap-destroy lifecycle surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("heap-lifecycle")),
        "heap-destroy lifecycle has a live session snapshot before destruction");
    if (snapshot != nullptr) {
        ok &= check(capture_surface_sequence(
            app,
            *window,
            *surface,
            snapshot->metadata.sequence),
            "heap-destroy lifecycle captures session resources before destruction");
    }
    ok &= check(pump_until(app, [&] {
        return has_live_render_tree(lifecycle_recorder->snapshot());
    }),
        "heap-destroy lifecycle starts with live QSG render resources");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(*surface),
        "heap-destroy lifecycle starts worker callback check with no queued GUI drain");

    const int pre_worker_output_activity_count = output_activity_count;
    backend_ptr->emit_output_from_worker(QByteArrayLiteral("queued-before-delete"));
    backend_ptr->join_worker();
    ok &= check(state->worker_callback_attempts.load() == 1 &&
        state->worker_callback_completions.load() == 1,
        "heap-destroy lifecycle queues one worker callback before deletion");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(*surface),
        "heap-destroy lifecycle does not queue posted drain work before deletion");
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            *surface) > 0U,
        "heap-destroy lifecycle has pending backend work before deletion");
    ok &= check(output_activity_count == pre_worker_output_activity_count,
        "heap-destroy lifecycle does not drain worker output before deletion");

    const int pre_destroy_signal_count =
        output_activity_count + exited_count + backend_error_count;
    surface.reset();
    ok &= check(surface_guard.isNull(), "heap-destroy lifecycle deletes the surface object");
    ok &= check(state->terminate_count.load() == 1,
        "heap-destroy lifecycle terminates the running backend during destruction");
    ok &= check(state->destructed_count.load() == 1,
        "heap-destroy lifecycle destroys the backend during surface destruction");

    ok &= check(pump_until(app, [&] {
        return has_no_live_render_resources(lifecycle_recorder->snapshot());
    }),
        "heap-destroy lifecycle releases QSG resources after surface destruction");
    ok &= check(state->terminate_count.load() == 1 &&
        state->destructed_count.load() == 1,
        "heap-destroy lifecycle cleanup is complete before post-delete event pumping");
    ok &= check(output_activity_count + exited_count + backend_error_count ==
        pre_destroy_signal_count,
        "heap-destroy lifecycle emits no public signal during or after surface deletion");

    return ok;
}

bool test_after_frame_callback_update_latch_falls_back_on_window_destroy(
    QGuiApplication& app)
{
    bool ok = true;

    auto window = std::make_unique<QQuickWindow>();
    window->resize(640, 320);

    VNM_TerminalSurface* surface = new VNM_TerminalSurface();
    QPointer<VNM_TerminalSurface> surface_guard(surface);
    surface->setParentItem(window->contentItem());
    surface->setSize(QSizeF(520.0, 240.0));
    surface->set_font_family(QStringLiteral("monospace"));
    surface->set_font_size(12.0);
    window->show();
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("after-frame-detach-baseline")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        *surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "after-frame detach fallback starts");
    if (!started || backend_ptr == nullptr) {
        delete surface;
        return ok;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(
            *baseline_snapshot,
            QStringLiteral("after-frame-detach-baseline")),
        "after-frame detach fallback publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        delete surface;
        return ok;
    }

    ok &= check(capture_surface_sequence(
        app,
        *window,
        *surface,
        baseline_snapshot->metadata.sequence),
        "after-frame detach fallback captures baseline");

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            *surface,
            std::chrono::steady_clock::duration::zero());
    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_cursor_stable_stop_extension_for_benchmark(
            *surface,
            std::chrono::steady_clock::duration::zero());

    const QString tail_text = QStringLiteral("after-frame-detach-tail");
    backend_ptr->emit_output(epoch_catchup_budget_probe_payload(tail_text.toUtf8()));

    QCoreApplication::sendPostedEvents(surface, QEvent::MetaCall);
    const term::Terminal_surface_render_invalidation_stats_t scheduled_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(*surface);
    ok &= check(scheduled_stats.pending_update,
        "after-frame detach fallback starts with frame work pending");

    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(*surface);
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            *surface) > 0U,
        "after-frame detach fallback leaves callback work behind the frame wait");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
            *surface),
        "after-frame detach fallback does not use posted drains while frame is viable");
    const std::shared_ptr<const term::Terminal_render_snapshot> before_detach_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(before_detach_snapshot != nullptr &&
        !snapshot_contains_text(*before_detach_snapshot, tail_text),
        "after-frame detach fallback has not published the delayed tail before detach");

    const term::Terminal_surface_backend_drain_stats_t stats_before_detach =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(*surface);
    window.reset();
    ok &= check(pump_until(app, [&] {
        return surface_guard.isNull() || surface_guard->window() == nullptr;
    }),
        "after-frame detach fallback reaches a detached surface");
    ok &= check(!surface_guard.isNull(),
        "after-frame detach fallback keeps the surface alive after window destruction");
    if (surface_guard.isNull()) {
        return ok;
    }

    ok &= check(pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(
                *surface_guard.data());
        return snapshot != nullptr &&
            snapshot_contains_text(*snapshot, tail_text);
    }, 40),
        "after-frame detach fallback drains pending callback work after window destruction");

    const term::Terminal_surface_backend_drain_stats_t stats_after_detach =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(
            *surface_guard.data());
    ok &= check(
        stats_after_detach.posted_drain_calls >
            stats_before_detach.posted_drain_calls,
        "after-frame detach fallback uses a posted drain after frame delivery is impossible");
    ok &= check(term::VNM_TerminalSurface_render_bridge::pending_backend_callback_count(
            *surface_guard.data()) == 0U,
        "after-frame detach fallback leaves no pending backend callbacks");

    delete surface_guard.data();
    return ok;
}

bool test_window_destroy_keeps_surface_session_until_surface_destroy(QGuiApplication& app)
{
    bool ok = true;

    auto window = std::make_unique<QQuickWindow>();
    window->resize(640, 320);

    VNM_TerminalSurface* surface = new VNM_TerminalSurface();
    QPointer<VNM_TerminalSurface> surface_guard(surface);
    surface->setParentItem(window->contentItem());
    surface->setSize(QSizeF(520.0, 240.0));
    surface->set_font_family(QStringLiteral("monospace"));
    surface->set_font_size(12.0);

    int output_activity_count = 0;
    QObject::connect(
        surface,
        &VNM_TerminalSurface::output_activity,
        surface,
        [&] {
            ++output_activity_count;
        });

    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(*surface);
    auto state   = std::make_shared<Scripted_backend_lifecycle_state>();
    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("window-lifecycle-output")};
    backend->lifecycle_state      = state;
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        *surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "window-destroy lifecycle surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("window-lifecycle")),
        "window-destroy lifecycle has a live session snapshot before rendering");

    window->show();
    if (snapshot != nullptr) {
        ok &= check(capture_surface_sequence(
            app,
            *window,
            *surface,
            snapshot->metadata.sequence),
            "window-destroy lifecycle captures session resources before window destruction");
    }

    const term::terminal_renderer_lifecycle_stats_t setup_stats =
        lifecycle_recorder->snapshot();
    ok &= check(has_live_render_tree(setup_stats),
        "window-destroy lifecycle starts with live QSG render resources");

    window.reset();
    ok &= check(pump_until(app, [&] {
        return surface_guard.isNull() || surface_guard->window() == nullptr;
    }),
        "window-destroy lifecycle reaches a post-window-destruction state");
    ok &= check(!surface_guard.isNull(),
        "window-destroy lifecycle keeps the surface alive after window destruction");
    if (surface_guard.isNull()) {
        return ok;
    }
    ok &= check(surface_guard->window() == nullptr,
        "window-destroy lifecycle detaches the surface from the destroyed window");

    ok &= check(pump_until(app, [&] {
        return has_no_live_render_resources(lifecycle_recorder->snapshot());
    }),
        "window-destroy lifecycle releases QSG resources after window destruction");
    const term::terminal_renderer_lifecycle_stats_t window_destroy_stats =
        lifecycle_recorder->snapshot();
    const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(*surface_guard.data());
    ok &= check(window_destroy_stats.item_scene_detaches >
        setup_stats.item_scene_detaches,
        "window-destroy lifecycle records the item scene detach");
    ok &= check(!invalidation_stats.pending_update,
        "window-destroy lifecycle clears pending render invalidation state");
    ok &= check(has_no_live_render_resources(window_destroy_stats),
        "window-destroy lifecycle leaves no live QSG resources after convergence");

    ok &= check(state->destructed_count.load() == 0,
        "window-destroy lifecycle keeps the backend owned while the surface lives");
    ok &= check(surface_guard->process_state() == VNM_TerminalSurface::Process_state::RUNNING &&
        surface_guard->backend_ready(),
        "window-destroy lifecycle keeps the session active until surface destruction");

    const int post_detach_activity_count = output_activity_count;
    backend_ptr->emit_output(QByteArrayLiteral("after-window-destroy"));
    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> post_detach_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface_guard.data());
    ok &= check(output_activity_count == post_detach_activity_count + 1,
        "window-destroy lifecycle still emits output activity after window destruction");
    ok &= check(post_detach_snapshot != nullptr &&
        snapshot_contains_text(*post_detach_snapshot, QStringLiteral("after-window-destroy")),
        "window-destroy lifecycle still updates the session snapshot after window destruction");

    delete surface;
    surface  = nullptr;
    ok      &= check(surface_guard.isNull(), "window-destroy lifecycle deletes the surface explicitly");
    ok      &= check(state->terminate_count.load() == 1,
        "window-destroy lifecycle terminates the backend when the surface is destroyed");
    ok      &= check(state->destructed_count.load() == 1,
        "window-destroy lifecycle destroys the backend with the surface");

    return ok;
}

bool test_geometry_change_resizes_session(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "geometry-resize surface starts");
    ok &= check(backend_ptr->resize_requests.empty(),
        "start does not issue a separate resize request");

    const std::size_t initial_resize_count = backend_ptr->resize_requests.size();
    const QSizeF candidates[] = {
        QSizeF(800.0, 360.0),
        QSizeF(900.0, 420.0),
        QSizeF(360.0, 180.0),
    };
    for (QSizeF size : candidates) {
        fixture.surface.setSize(size);
        pump_events(app, 2);
        if (backend_ptr->resize_requests.size() > initial_resize_count) {
            break;
        }
    }

    ok &= check(backend_ptr->resize_requests.size() > initial_resize_count,
        "geometry change sends a resize to the backend");
    if (backend_ptr->resize_requests.size() > initial_resize_count) {
        const term::terminal_grid_size_t grid_size =
            backend_ptr->resize_requests.back().grid_size;
        ok &= check(grid_size.rows == fixture.surface.rows() &&
            grid_size.columns == fixture.surface.columns(),
            "surface rows and columns mirror resized session grid");
        ok &= check(term::is_valid_grid_size(grid_size),
            "geometry resize sends a valid backend grid");
    }

    return ok;
}

bool test_backend_exit_updates_surface(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int exited_count = 0;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int exit_code = -1;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int code) {
            ++exited_count;
            exit_reason = reason;
            exit_code = code;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "exit surface starts");

    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 7});
    pump_events(app);

    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::EXITED,
        "backend exit updates surface process state");
    ok &= check(!fixture.surface.backend_ready(),
        "backend exit clears surface backend readiness");
    ok &= check(exited_count == 1 &&
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        exit_code == 7,
        "backend exit emits public process_exited signal");

    return ok;
}

bool test_public_lifecycle_methods(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        ok &= check(!fixture.surface.interrupt_process(),
            "no-session interrupt returns false");
        ok &= check(!fixture.surface.terminate_process(),
            "no-session terminate returns false");
        ok &= check(error_codes.size() == 2U &&
            error_codes[0] ==
                VNM_TerminalSurface::Backend_error_code::INTERRUPT_FAILED &&
            error_codes[1] ==
                VNM_TerminalSurface::Backend_error_code::TERMINATE_FAILED,
            "no-session lifecycle calls emit typed public backend errors");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        int exited_count = 0;
        VNM_TerminalSurface::Exit_reason exit_reason =
            VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
        int exit_code = -1;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::process_exited,
            &fixture.surface,
            [&](VNM_TerminalSurface::Exit_reason reason, int code) {
                ++exited_count;
                exit_reason = reason;
                exit_code = code;
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);

        ok &= check(started, "interrupt lifecycle surface starts");
        ok &= check(fixture.surface.interrupt_process(),
            "public interrupt succeeds through scripted backend");
        pump_events(app);
        ok &= check(!backend_ptr->running,
            "public interrupt stops scripted backend");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::EXITED &&
            !fixture.surface.backend_ready(),
            "public interrupt syncs exited surface properties");
        ok &= check(exited_count == 1 &&
            exit_reason == VNM_TerminalSurface::Exit_reason::INTERRUPTED &&
            exit_code == 130,
            "public interrupt emits process_exited signal");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        int exited_count = 0;
        VNM_TerminalSurface::Exit_reason exit_reason =
            VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
        int exit_code = -1;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::process_exited,
            &fixture.surface,
            [&](VNM_TerminalSurface::Exit_reason reason, int code) {
                ++exited_count;
                exit_reason = reason;
                exit_code = code;
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);

        ok &= check(started, "terminate lifecycle surface starts");
        ok &= check(fixture.surface.terminate_process(),
            "public terminate succeeds through scripted backend");
        pump_events(app);
        ok &= check(!backend_ptr->running,
            "public terminate stops scripted backend");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::EXITED &&
            !fixture.surface.backend_ready(),
            "public terminate syncs exited surface properties");
        ok &= check(exited_count == 1 &&
            exit_reason == VNM_TerminalSurface::Exit_reason::TERMINATED &&
            exit_code == 0,
            "public terminate emits process_exited signal");
    }

    return ok;
}

bool test_terminal_icon_name_notifications(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int title_changed_count     = 0;
    int icon_name_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_icon_name_changed,
        &fixture.surface,
        [&] {
            ++icon_name_changed_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "terminal-icon-name surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]1;icon-only\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 1,
        "OSC 1 emits surface icon name notification");
    ok &= check(title_changed_count == 0,
        "OSC 1 does not emit surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("icon-only"),
        "OSC 1 updates surface terminal icon name");
    ok &= check(fixture.surface.terminal_title().isEmpty(),
        "OSC 1 preserves empty surface terminal title");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;combined\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 2,
        "OSC 0 emits surface icon name notification");
    ok &= check(title_changed_count == 1,
        "OSC 0 emits surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("combined"),
        "OSC 0 updates surface terminal icon name");
    ok &= check(fixture.surface.terminal_title() == QStringLiteral("combined"),
        "OSC 0 updates surface terminal title");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]2;title-only\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 2,
        "OSC 2 does not emit surface icon name notification");
    ok &= check(title_changed_count == 2,
        "OSC 2 emits surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("combined"),
        "OSC 2 preserves surface terminal icon name");
    ok &= check(fixture.surface.terminal_title() == QStringLiteral("title-only"),
        "OSC 2 updates surface terminal title");

    return ok;
}

bool test_audible_bell_policy_requests_platform_bell(QGuiApplication& app)
{
    bool ok = true;

    auto run_case = [&](
                        VNM_TerminalSurface::Bell_policy audible_policy,
                        VNM_TerminalSurface::Bell_policy visual_policy,
                        int                              expected_audible_bell_count,
                        int                              expected_bell_signal_count,
                        const char*                      start_message,
                        const char*                      audible_message,
                        const char*                      signal_message) {
        Surface_fixture fixture;
        fixture.surface.set_audible_bell_policy(audible_policy);
        fixture.surface.set_visual_bell_policy(visual_policy);
        pump_events(app);

        int audible_bell_count = 0;
        int bell_signal_count  = 0;
        term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
            fixture.surface,
            [&] {
                ++audible_bell_count;
            });
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::bell_requested,
            &fixture.surface,
            [&] {
                ++bell_signal_count;
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, start_message);

        backend_ptr->emit_output(QByteArrayLiteral("\a"));
        pump_events(app);
        ok &= check(
            audible_bell_count == expected_audible_bell_count,
            audible_message);
        ok &= check(
            bell_signal_count == expected_bell_signal_count,
            signal_message);
    };

    run_case(
        VNM_TerminalSurface::Bell_policy::ENABLED,
        VNM_TerminalSurface::Bell_policy::ENABLED,
        1,
        1,
        "audible-bell enabled surface starts",
        "enabled audible bell invokes platform bell handler",
        "enabled audible bell emits surface bell_requested signal");
    run_case(
        VNM_TerminalSurface::Bell_policy::DISABLED,
        VNM_TerminalSurface::Bell_policy::ENABLED,
        0,
        1,
        "audible-bell disabled surface starts",
        "disabled audible bell does not invoke platform bell handler",
        "visual-only bell still emits surface bell_requested signal");
    run_case(
        VNM_TerminalSurface::Bell_policy::DISABLED,
        VNM_TerminalSurface::Bell_policy::DISABLED,
        0,
        0,
        "audible-bell fully disabled surface starts",
        "fully disabled bell does not invoke platform bell handler",
        "fully disabled bell emits no surface bell_requested signal");

    Surface_fixture fixture;
    fixture.surface.set_audible_bell_policy(VNM_TerminalSurface::Bell_policy::DISABLED);
    fixture.surface.set_visual_bell_policy(VNM_TerminalSurface::Bell_policy::DISABLED);
    pump_events(app);

    int audible_bell_count = 0;
    int bell_signal_count  = 0;
    term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
        fixture.surface,
        [&] {
            ++audible_bell_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::bell_requested,
        &fixture.surface,
        [&] {
            ++bell_signal_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "live bell-policy toggle surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\a"));
    pump_events(app);
    ok &= check(audible_bell_count == 0,
        "initially disabled live bell policy suppresses platform bell handler");
    ok &= check(bell_signal_count == 0,
        "initially disabled live bell policy suppresses surface bell signal");

    fixture.surface.set_audible_bell_policy(VNM_TerminalSurface::Bell_policy::ENABLED);
    backend_ptr->emit_output(QByteArrayLiteral("\a"));
    pump_events(app);
    ok &= check(audible_bell_count == 1,
        "live audible bell enable invokes platform bell handler");
    ok &= check(bell_signal_count == 1,
        "live audible bell enable emits surface bell signal");

    fixture.surface.set_audible_bell_policy(VNM_TerminalSurface::Bell_policy::DISABLED);
    backend_ptr->emit_output(QByteArrayLiteral("\a"));
    pump_events(app);
    ok &= check(audible_bell_count == 1,
        "live audible bell disable suppresses platform bell handler");
    ok &= check(bell_signal_count == 1,
        "live audible bell disable suppresses surface bell signal");

    fixture.surface.set_visual_bell_policy(VNM_TerminalSurface::Bell_policy::ENABLED);
    backend_ptr->emit_output(QByteArrayLiteral("\a"));
    pump_events(app);
    ok &= check(audible_bell_count == 1,
        "live visual-only bell policy keeps platform bell handler suppressed");
    ok &= check(bell_signal_count == 2,
        "live visual-only bell policy emits surface bell signal");

    {
        Surface_fixture delayed_fixture;
        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::DISABLED);
        delayed_fixture.surface.set_visual_bell_policy(
            VNM_TerminalSurface::Bell_policy::ENABLED);
        pump_events(app);

        int delayed_audible_count = 0;
        int delayed_signal_count  = 0;
        term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
            delayed_fixture.surface,
            [&] {
                ++delayed_audible_count;
            });
        QObject::connect(
            &delayed_fixture.surface,
            &VNM_TerminalSurface::bell_requested,
            &delayed_fixture.surface,
            [&] {
                ++delayed_signal_count;
            });

        auto delayed_backend = std::make_unique<Scripted_backend>();
        bool delayed_started = false;
        Scripted_backend* delayed_backend_ptr = start_surface_with_backend(
            delayed_fixture.surface,
            std::move(delayed_backend),
            { QStringLiteral("scripted-terminal") },
            &delayed_started);
        ok &= check(delayed_started, "delayed visual-only bell surface starts");

        delayed_backend_ptr->emit_output(QByteArrayLiteral("\a"));
        term::VNM_TerminalSurface_render_bridge::
            process_backend_callbacks_without_notification_delivery_for_testing(
                delayed_fixture.surface);
        ok &= check(delayed_audible_count == 0,
            "delayed visual-only bell is not audible before notification delivery");
        ok &= check(delayed_signal_count == 0,
            "delayed visual-only bell signal is pending before notification delivery");

        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::ENABLED);
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            delayed_fixture.surface);
        ok &= check(delayed_audible_count == 0,
            "delayed visual-only bell does not become audible after live policy enable");
        ok &= check(delayed_signal_count == 1,
            "delayed visual-only bell still emits pending surface bell signal");
    }

    {
        Surface_fixture delayed_fixture;
        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::ENABLED);
        delayed_fixture.surface.set_visual_bell_policy(
            VNM_TerminalSurface::Bell_policy::DISABLED);
        pump_events(app);

        int delayed_audible_count = 0;
        int delayed_signal_count  = 0;
        term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
            delayed_fixture.surface,
            [&] {
                ++delayed_audible_count;
            });
        QObject::connect(
            &delayed_fixture.surface,
            &VNM_TerminalSurface::bell_requested,
            &delayed_fixture.surface,
            [&] {
                ++delayed_signal_count;
            });

        auto delayed_backend = std::make_unique<Scripted_backend>();
        bool delayed_started = false;
        Scripted_backend* delayed_backend_ptr = start_surface_with_backend(
            delayed_fixture.surface,
            std::move(delayed_backend),
            { QStringLiteral("scripted-terminal") },
            &delayed_started);
        ok &= check(delayed_started, "delayed audible-only bell surface starts");

        delayed_backend_ptr->emit_output(QByteArrayLiteral("\a"));
        term::VNM_TerminalSurface_render_bridge::
            process_backend_callbacks_without_notification_delivery_for_testing(
                delayed_fixture.surface);
        ok &= check(delayed_audible_count == 0,
            "delayed audible-only bell is pending before notification delivery");
        ok &= check(delayed_signal_count == 0,
            "delayed audible-only bell signal is pending before notification delivery");

        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::DISABLED);
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            delayed_fixture.surface);
        ok &= check(delayed_audible_count == 1,
            "delayed audible-only bell remains audible after live policy disable");
        ok &= check(delayed_signal_count == 1,
            "delayed audible-only bell emits pending surface bell signal");
    }

    {
        Surface_fixture delayed_fixture;
        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::ENABLED);
        delayed_fixture.surface.set_visual_bell_policy(
            VNM_TerminalSurface::Bell_policy::DISABLED);
        pump_events(app);

        int delayed_audible_count = 0;
        int delayed_signal_count  = 0;
        term::VNM_TerminalSurface_render_bridge::set_audible_bell_handler_for_testing(
            delayed_fixture.surface,
            [&] {
                ++delayed_audible_count;
            });
        QObject::connect(
            &delayed_fixture.surface,
            &VNM_TerminalSurface::bell_requested,
            &delayed_fixture.surface,
            [&] {
                ++delayed_signal_count;
            });

        auto delayed_backend = std::make_unique<Scripted_backend>();
        bool delayed_started = false;
        Scripted_backend* delayed_backend_ptr = start_surface_with_backend(
            delayed_fixture.surface,
            std::move(delayed_backend),
            { QStringLiteral("scripted-terminal") },
            &delayed_started);
        ok &= check(delayed_started, "delayed mixed-intent bell surface starts");

        delayed_backend_ptr->emit_output(QByteArrayLiteral("\a"));
        term::VNM_TerminalSurface_render_bridge::
            process_backend_callbacks_without_notification_delivery_for_testing(
                delayed_fixture.surface);
        delayed_fixture.surface.set_audible_bell_policy(
            VNM_TerminalSurface::Bell_policy::DISABLED);
        delayed_fixture.surface.set_visual_bell_policy(
            VNM_TerminalSurface::Bell_policy::ENABLED);
        delayed_backend_ptr->emit_output(QByteArrayLiteral("\a"));
        term::VNM_TerminalSurface_render_bridge::
            process_backend_callbacks_without_notification_delivery_for_testing(
                delayed_fixture.surface);
        ok &= check(delayed_audible_count == 0,
            "delayed mixed-intent bell is pending before notification delivery");
        ok &= check(delayed_signal_count == 0,
            "delayed mixed-intent bell signal is pending before notification delivery");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            delayed_fixture.surface);
        ok &= check(delayed_audible_count == 1,
            "delayed mixed-intent bell preserves coalesced audible intent");
        ok &= check(delayed_signal_count == 1,
            "delayed mixed-intent bell emits one coalesced surface bell signal");
    }

    return ok;
}

bool test_notification_burst_uses_durable_channel(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int title_changed_count = 0;
    int exited_count        = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason, int) {
            ++exited_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "notification-burst surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("notification-burst-pending-frame"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(pending_stats.pending_update,
        "notification-burst fixture starts with already-pending render work");
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    constexpr int k_title_notification_count = 1105;
    QByteArray burst;
    for (int i = 0; i < k_title_notification_count; ++i) {
        burst.append(QByteArrayLiteral("\x1b]2;"));
        burst.append(QStringLiteral("burst-title-%1").arg(i).toUtf8());
        burst.append('\a');
    }

    backend_ptr->emit_output(std::move(burst));
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 5});
    pump_events(app);

    ok &= check_int_equal(title_changed_count, 1,
        "durable notification drain coalesces title burst for GUI delivery");
    ok &= check(fixture.surface.terminal_title() ==
        QStringLiteral("burst-title-%1").arg(k_title_notification_count - 1),
        "title burst leaves the final terminal title visible");
    ok &= check(exited_count == 1,
        "critical process_exited notification is delivered after a notification burst");

    return ok;
}

bool test_surface_overflow_reports_error_and_exit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int overflow_error_count = 0;
    int exited_count         = 0;

    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            if (code == VNM_TerminalSurface::Backend_error_code::OUTPUT_OVERFLOW) {
                ++overflow_error_count;
            }
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int) {
            ++exited_count;
            exit_reason = reason;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface overflow test starts");

    backend_ptr->emit_output(QByteArray(3 * 1024 * 1024, 'x'));
    pump_events(app);

    ok &= check(overflow_error_count == 1,
        "surface overflow emits one public overflow backend_error");
    ok &= check(exited_count == 1 &&
        exit_reason == VNM_TerminalSurface::Exit_reason::TERMINATED,
        "surface overflow termination emits public process_exited");
    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::EXITED,
        "surface overflow reaches exited process state");
    ok &= check(!fixture.surface.backend_ready(),
        "surface overflow clears backend readiness");

    return ok;
}

bool test_invalid_argv_reports_backend_error(QGuiApplication& app)
{
    bool ok = true;

    const std::vector<QStringList> invalid_argv_cases = {
        QStringList{},
        QStringList{QStringLiteral("   ")},
    };
    for (const QStringList& argv : invalid_argv_cases) {
        {
            Surface_fixture fixture;
            pump_events(app);

            int error_count = 0;
            std::optional<VNM_TerminalSurface::Backend_error_code> error_code;
            QObject::connect(
                &fixture.surface,
                &VNM_TerminalSurface::backend_error,
                &fixture.surface,
                [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                    ++error_count;
                    error_code = code;
                });

            ok &= check(!fixture.surface.start_process(argv),
                "public invalid argv start returns false");
            ok &= check(error_count == 1 &&
                error_code ==
                    VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG,
                "public invalid argv emits typed backend_error");
            ok &= check(fixture.surface.process_state() ==
                VNM_TerminalSurface::Process_state::FAILED,
                "public invalid argv marks surface process state failed");
        }

        Surface_fixture fixture;
        pump_events(app);

        int error_count = 0;
        std::optional<VNM_TerminalSurface::Backend_error_code> error_code;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                ++error_count;
                error_code = code;
            });

        std::vector<term::Terminal_launch_config> observed_start_configs;
        int  observed_start_attempts = 0;
        auto backend                 = std::make_unique<Scripted_backend>();
        backend->start_config_observer  = &observed_start_configs;
        backend->start_attempt_observer = &observed_start_attempts;
        bool started = false;
        (void)start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            argv,
            &started);

        ok &= check(!started, "invalid argv start returns false");
        ok &= check(observed_start_attempts == 0,
            "invalid argv does not attempt backend start");
        ok &= check(observed_start_configs.empty(),
            "invalid argv does not invoke backend start");
        ok &= check(error_count == 1 &&
            error_code ==
                VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG,
            "invalid argv emits public backend_error");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::FAILED,
            "invalid argv marks surface process state failed");
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList arguments = app.arguments();

    if (arguments.contains(QStringLiteral("--pending-mouse-input-preservation"))) {
        const bool ok = test_pending_mouse_report_preserves_following_key_input(app);
        return ok ? 0 : 1;
    }

    bool ok = true;
    ok &= test_start_maps_output_to_snapshot(app);
    ok &= test_surface_polish_refreshes_metrics_after_window_dpr_change(app);
    ok &= test_surface_session_snapshot_burst_coalesces_to_latest_render(app);
    ok &= test_surface_polish_drains_queued_backend_output_before_render_capture(app);
    ok &= test_surface_no_echo_input_keeps_cursor_visible(app);
    ok &= test_surface_unrendered_publication_input_keeps_cursor_visible(app);
    ok &= test_surface_undrawn_publication_keeps_atlas_completion_pending(app);
    ok &= test_surface_undrawn_publication_restores_cursor_after_render(app);
    ok &= test_surface_frame_boundary_output_publishes_followup_dirty_rows(app);
    ok &= test_surface_backend_drain_metrics_split_stages(app);
    ok &= test_surface_posted_backend_drain_uses_frame_pending_small_budget(app);
    ok &= test_surface_frame_catchup_budget_env_config();
    ok &= test_surface_frame_catchup_cursor_stable_stop_extension_env_config();
    ok &= test_surface_epoch_catchup_uses_frame_budget_override(app);
    ok &= test_surface_cursor_stable_extension_disabled_preserves_incomplete_boundary(app);
    ok &= test_surface_synchronized_release_stable_with_extension_disabled_counts_cursor_stable(app);
    ok &= test_surface_cursor_withhold_arms_unsafe_publication_until_settlement(app);
    ok &= test_surface_cursor_withhold_clears_after_nonpublishing_settlement(app);
    ok &= test_surface_cursor_withhold_clears_active_callback_before_follow_up(app);
    ok &= test_surface_cursor_withhold_clears_incomplete_output_after_exit(app);
    ok &= test_surface_cursor_withhold_clears_same_drain_output_exit(app);
    ok &= test_surface_cursor_withhold_clears_synchronized_hold_after_exit(app);
    ok &= test_surface_cursor_withhold_suppresses_rendered_cursor(app);
    ok &= test_surface_wheel_input_keeps_no_drain_write_output_withheld(app);
    ok &= test_surface_cursor_withhold_keeps_hidden_backend_progress_withheld(app);
    ok &= test_surface_cursor_withhold_keeps_pending_callback_withheld(app);
    ok &= test_surface_cursor_withhold_keeps_write_output_withheld(app);
    ok &= test_surface_cursor_withhold_keeps_prescan_pending_output_withheld(app);
    ok &= test_surface_synchronized_hold_stop_preserves_drain_stats(app);
    ok &= test_surface_epoch_catchup_pending_mouse_retry_stays_on_frame_path(app);
    ok &= test_surface_posted_backend_drain_uses_full_budget_without_frame_work(app);
    ok &= test_surface_posted_backend_drain_reconciles_completed_atlas_before_budget(app);
    ok &= test_surface_reported_atlas_completion_advances_rendered_publication(app);
    ok &= test_surface_stale_atlas_completion_does_not_advance_rendered_publication(app);
    ok &= test_surface_reset_session_clears_pending_atlas_completion(app);
    ok &= test_surface_qsg_capture_without_draw_preserves_dirty_rows(app);
    ok &= test_surface_session_single_drain_coalesces_dirty_rows(app);
    ok &= test_osc52_clipboard_write_signal_and_deny(app);
    ok &= test_osc52_clipboard_wrong_duplicate_and_replacement(app);
    ok &= test_osc52_clipboard_late_exit_restart_and_targets(app);
    ok &= test_osc52_clipboard_allow_writes_clipboard(app);
    ok &= test_keyboard_printable_controls_and_prompt_path(app);
    ok &= test_copy_shortcut_policy(app);
    ok &= test_synchronized_output_scroll_policy_property(app);
    ok &= test_scroll_diagnostic_enum_name_table(app);
    ok &= test_no_payload_copy_fallback_states(app);
    ok &= test_control_wheel_font_zoom(app);
    ok &= test_plain_wheel_scrolls_primary_scrollback(app);
    ok &= test_public_viewport_scroll_api(app);
    ok &= test_immediate_public_projection_public_scroll_api(app);
    ok &= test_public_viewport_scroll_source_labels(app);
    ok &= test_immediate_public_projection_page_keys(app);
    ok &= test_public_scroll_api_records_deferred_after_projection_invalidation(app);
    ok &= test_app_route_boundaries_record_deferred_after_projection_invalidation(app);
    ok &= test_plain_wheel_scrolls_scroll_region_primary_scrollback(app);
    ok &= test_plain_wheel_scrolls_csi_scroll_up_primary_scrollback(app);
    ok &= test_page_keys_scroll_primary_scrollback(app);
    ok &= test_page_keys_fall_through_on_alternate_screen(app);
    ok &= test_plain_wheel_boundaries_and_alternate_input(app);
    ok &= test_local_first_wheel_scroll_keeps_callbacks_queued_without_backend_drain(app);
    ok &= test_wheel_input_stops_after_post_barrier_callbacks_become_pending(app);
    ok &= test_wheel_mouse_reporting_stops_after_post_barrier_callbacks_become_pending(app);
    ok &= test_mouse_press_ignores_hidden_pending_mouse_enable(app);
    ok &= test_mouse_release_pending_report_uses_published_modes(app);
    ok &= test_mouse_press_release_pending_callbacks_clear_grab(app);
    ok &= test_pending_mouse_report_preserves_following_key_input(app);
    ok &= test_mouse_passive_motion_preserves_detached_viewport(app);
    ok &= test_local_first_wheel_trace_records_ingress_before_route(app);
    ok &= test_local_first_wheel_scroll_applies_during_synchronized_output_block(app);
    ok &= test_mid_hold_policy_flip_keeps_text_area_wheel_boundary_input(app);
    ok &= test_transcript_timing_diagnostics_records_hot_paths();
    ok &= test_mouse_reporting_surface_events(app);
    ok &= test_row_timestamp_tooltip_signal_contract(app);
    ok &= test_selection_drag_and_selected_text(app);
    ok &= test_selection_visual_detach_after_row_mutation(app);
    ok &= test_selection_drag_remaps_live_scrollback_viewport_spans(app);
    ok &= test_selection_drag_survives_unrelated_row_backend_output(app);
    ok &= test_selection_drag_survives_worker_unrelated_row_backend_output(app);
    ok &= test_selection_drag_survives_at_tail_streaming_output(app);
    ok &= test_selection_drag_stationary_pointer_at_tail_streaming_contract(app);
    ok &= test_selection_drag_survives_stable_row_content_drift(app);
    ok &= test_selection_drag_preserves_payload_after_mid_drag_drift(app);
    ok &= test_selection_drag_rejects_snapshot_change(app);
    ok &= test_stale_synchronized_output_recovery(app);
    ok &= test_synchronized_output_styled_blank_rows_preserve_background(app);
    ok &= test_stale_synchronized_output_recovery_force_releases_after_budgeted_catchup(app);
    ok &= test_paste_text_public_method_and_policy(app);
    ok &= test_right_click_paste_and_mouse_reporting_precedence(app);
    ok &= test_focus_reporting_writes_mode_bytes_and_preserves_ime_cancel(app);
    ok &= test_ime_preedit_updates_overlay_without_snapshot_churn(app);
    ok &= test_ime_commit_writes_utf8_and_clears_preedit(app);
    ok &= test_ime_no_session_overlay_and_later_start_clear(app);
    ok &= test_ime_startup_commit_clears_preedit_without_backend_error(app);
    ok &= test_ime_multi_codepoint_commit_writes_utf8(app);
    ok &= test_ime_combined_commit_and_preedit(app);
    ok &= test_ime_replacement_range_is_stream_commit(app);
    ok &= test_ime_commit_failure_preserves_preedit(app);
    ok &= test_ime_synchronized_output_overlay(app);
    ok &= test_ime_focus_loss_cancels_preedit(app);
    ok &= test_ime_cursor_rectangle_query_tracks_snapshot_cursor(app);
    ok &= test_keyboard_cursor_modes(app);
    ok &= test_keyboard_navigation_keys(app);
    ok &= test_keyboard_function_keys(app);
    ok &= test_keyboard_keypad_modes(app);
    ok &= test_keyboard_focus_routed_delivery(app);
    ok &= test_keyboard_no_session_and_post_exit_semantics(app);
    ok &= test_keyboard_unhandled_key_skips_backend_drain(app);
    ok &= test_worker_callback_drains_on_gui_thread(app);
    ok &= test_hover_move_does_not_drain_queued_backend_output(app);
    ok &= test_heap_surface_destroy_closes_queued_worker_callback(app);
    ok &= test_after_frame_callback_update_latch_falls_back_on_window_destroy(app);
    ok &= test_window_destroy_keeps_surface_session_until_surface_destroy(app);
    ok &= test_geometry_change_resizes_session(app);
    ok &= test_backend_exit_updates_surface(app);
    ok &= test_public_lifecycle_methods(app);
    ok &= test_terminal_icon_name_notifications(app);
    ok &= test_audible_bell_policy_requests_platform_bell(app);
    ok &= test_notification_burst_uses_durable_channel(app);
    ok &= test_surface_overflow_reports_error_and_exit(app);
    ok &= test_invalid_argv_reports_backend_error(app);
    return ok ? 0 : 1;
}
