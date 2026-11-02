#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_canvas_render_bridge.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QColor>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

struct sample_t
{
    rusage process{};
    rusage gui{};
    std::chrono::steady_clock::time_point time;
};

struct timer_observation_t
{
    QTimer* timer = nullptr;
    std::uint64_t timeouts = 0;
    std::uint64_t before = 0;
    bool active_before = false;
};

bool read_sample(sample_t& sample)
{
    if (getrusage(RUSAGE_SELF, &sample.process) != 0 ||
        getrusage(RUSAGE_THREAD, &sample.gui) != 0)
    {
        std::perror("getrusage");
        return false;
    }
    sample.time = std::chrono::steady_clock::now();
    return true;
}

double cpu_ms(const rusage& value)
{
    return (double)(value.ru_utime.tv_sec + value.ru_stime.tv_sec) * 1000.0 +
        (double)(value.ru_utime.tv_usec + value.ru_stime.tv_usec) / 1000.0;
}

void run_event_loop(int duration_ms)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(duration_ms);
    loop.exec();
}

bool frame_is_presented(const VNM_TerminalCanvas& canvas)
{
    return canvas.render_ready() &&
        canvas.rendered_frame_generation() == canvas.frame_generation() &&
        canvas.rendered_frame_sequence() == canvas.frame_sequence() &&
        canvas.rendered_publication_generation() == canvas.canvas_frame()->publication_generation;
}

bool wait_for_frame(VNM_TerminalCanvas& canvas)
{
    if (frame_is_presented(canvas)) {
        return true;
    }
    QEventLoop loop;
    QObject::connect(&canvas, &VNM_TerminalCanvas::render_status_changed, &loop,
        [&] { if (frame_is_presented(canvas)) { loop.quit(); } });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(5000);
    loop.exec();
    return frame_is_presented(canvas);
}

std::shared_ptr<vnm_terminal::Terminal_canvas_frame> make_frame(bool blink, std::uint64_t sequence)
{
    auto frame = std::make_shared<vnm_terminal::Terminal_canvas_frame>();
    frame->rows                   = 6;
    frame->columns                = 24;
    frame->cell_width              = 10.0;
    frame->cell_height             = 20.0;
    frame->content_width           = 240.0;
    frame->content_height          = 120.0;
    frame->sequence               = sequence;
    frame->publication_generation = sequence;
    frame->default_foreground_rgba = 0xffd0d8e0U;
    frame->default_background_rgba = 0xff091018U;
    frame->cursor_rgba             = 0xff80ff80U;
    frame->styles.push_back({frame->default_foreground_rgba, frame->default_background_rgba, 0U});
    frame->cells = {{0, 0, 1, 0U, QStringLiteral("A")}, {1, 0, 1, 0U, QStringLiteral("B")}};
    frame->cursor = {1, 2, vnm_terminal::Terminal_canvas_cursor_shape::BLOCK, true, blink};
    return frame;
}

QString graphics_api_name(QQuickWindow& window)
{
    switch (window.rendererInterface()->graphicsApi()) {
        case QSGRendererInterface::OpenGL:   return QStringLiteral("OpenGL");
        case QSGRendererInterface::Vulkan:   return QStringLiteral("Vulkan");
        case QSGRendererInterface::Metal:    return QStringLiteral("Metal");
        case QSGRendererInterface::Software: return QStringLiteral("Software");
        case QSGRendererInterface::Null:     return QStringLiteral("Null");
        case QSGRendererInterface::Unknown:  return QStringLiteral("Unknown");
        default:                            return QStringLiteral("Other");
    }
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const QStringList scenarios = {
        QStringLiteral("empty-window"), QStringLiteral("no-model"),
        QStringLiteral("visible-steady"), QStringLiteral("visible-blink"),
        QStringLiteral("hidden-steady"), QStringLiteral("hidden-blink"),
        QStringLiteral("hidden-parent"), QStringLiteral("hidden-window"),
    };
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Observe terminal canvas idle work without forcing redraws."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("scenario"),
        QStringLiteral("One of: %1.").arg(scenarios.join(QStringLiteral(", "))),
        QStringLiteral("name"), QStringLiteral("visible-steady")});
    parser.addOption({QStringLiteral("duration-ms"), QStringLiteral("Untouched observation interval (500..60000 ms)."),
        QStringLiteral("milliseconds"), QStringLiteral("3000")});
    parser.process(application);
    const QString scenario = parser.value(QStringLiteral("scenario"));
    bool duration_valid = false;
    const int duration_ms = parser.value(QStringLiteral("duration-ms")).toInt(&duration_valid);
    if (!scenarios.contains(scenario) || !duration_valid || duration_ms < 500 || duration_ms > 60000) {
        std::fprintf(stderr, "Invalid scenario or duration. Scenarios: %s\n",
            qPrintable(scenarios.join(QStringLiteral(", "))));
        return 2;
    }

    const bool empty   = scenario == QLatin1String("empty-window");
    const bool has_frame = !empty && scenario != QLatin1String("no-model");
    const bool blink   = scenario.endsWith(QLatin1String("-blink"));
    const bool hidden  = scenario.startsWith(QLatin1String("hidden-"));
    std::atomic<std::uint64_t> render_signals{0};
    std::atomic<std::uint64_t> swap_signals{0};
    QQuickWindow window;
    window.setColor(QColor(9, 16, 24));
    window.resize(400, 240);
    QQuickItem parent(window.contentItem());
    parent.setSize(QSizeF(400, 240));
    std::unique_ptr<VNM_TerminalCanvas> canvas;
    if (!empty) {
        canvas = std::make_unique<VNM_TerminalCanvas>(&parent);
        canvas->setSize(QSizeF(400, 240));
        if (has_frame && !canvas->set_canvas_frame(make_frame(blink, 101U))) {
            std::fprintf(stderr, "Probe frame was rejected.\n");
            return 1;
        }
    }
    QObject observer;
    QObject::connect(&window, &QQuickWindow::afterRendering, &observer,
        [&] { render_signals.fetch_add(1U, std::memory_order_relaxed); }, Qt::DirectConnection);
    QObject::connect(&window, &QQuickWindow::frameSwapped, &observer,
        [&] { swap_signals.fetch_add(1U, std::memory_order_relaxed); }, Qt::DirectConnection);
    std::uint64_t dispatcher_awake = 0;
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::awake,
        &observer, [&] { ++dispatcher_awake; });
    std::vector<timer_observation_t> timers;
    std::uint64_t status_notifications = 0;
    std::uint64_t error_notifications = 0;
    std::uint64_t blink_notifications = 0;
    if (canvas) {
        const auto children = canvas->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
        timers.reserve(children.size());
        for (QTimer* timer : children) {
            timers.push_back({timer});
            const std::size_t index = timers.size() - 1U;
            QObject::connect(timer, &QTimer::timeout, &observer, [&, index] { ++timers[index].timeouts; });
        }
        QObject::connect(canvas.get(), &VNM_TerminalCanvas::render_status_changed, &observer,
            [&] { ++status_notifications; });
        QObject::connect(canvas.get(), &VNM_TerminalCanvas::render_error_changed, &observer,
            [&] { ++error_notifications; });
        QObject::connect(canvas.get(), &VNM_TerminalCanvas::cursor_blink_phase_changed, &observer,
            [&] { ++blink_notifications; });
    }

    window.show();
    if (has_frame && !wait_for_frame(*canvas)) {
        const auto api = window.rendererInterface()->graphicsApi();
        const bool unavailable = QGuiApplication::platformName() == QLatin1String("offscreen") ||
            api == QSGRendererInterface::Software || api == QSGRendererInterface::Null ||
            api == QSGRendererInterface::Unknown;
        std::fprintf(stderr, "Natural first render unavailable: platform=%s api=%s error=%s\n",
            qPrintable(QGuiApplication::platformName()), qPrintable(graphics_api_name(window)),
            qPrintable(canvas->render_error()));
        return unavailable ? 77 : 1;
    }
    if (scenario == QLatin1String("hidden-parent")) {
        parent.setVisible(false);
    }
    else
    if (scenario == QLatin1String("hidden-window")) {
        window.hide();
    }
    else
    if (hidden) {
        canvas->setVisible(false);
    }
    run_event_loop(1000);

    const auto recorder = canvas ? term::VNM_TerminalCanvas_render_bridge::qsg_atlas_recorder(*canvas) : nullptr;
    const auto report_before = recorder ? recorder->snapshot() : term::Qsg_atlas_frame_report{};
    for (auto& timer : timers) {
        timer.before        = timer.timeouts;
        timer.active_before = timer.timer->isActive();
    }
    const auto render_before     = render_signals.load(std::memory_order_relaxed);
    const auto swap_before       = swap_signals.load(std::memory_order_relaxed);
    const auto awake_before      = dispatcher_awake;
    const auto status_before     = status_notifications;
    const auto error_before      = error_notifications;
    const auto blink_before      = blink_notifications;
    sample_t before;
    sample_t after;
    if (!read_sample(before)) {
        return 1;
    }
    // Only this interval's deadline is added; repeated processEvents/grabWindow
    // calls would manufacture the activity this probe is intended to observe.
    run_event_loop(duration_ms);
    if (!read_sample(after)) {
        return 1;
    }
    const auto report_after = recorder ? recorder->snapshot() : term::Qsg_atlas_frame_report{};
    QJsonArray timer_results;
    for (const auto& timer : timers) {
        timer_results.append(QJsonObject{
            {QStringLiteral("interval_ms"), timer.timer->interval()},
            {QStringLiteral("timeouts"), (qint64)(timer.timeouts - timer.before)},
            {QStringLiteral("active_before"), timer.active_before},
            {QStringLiteral("active_after"), timer.timer->isActive()},
        });
    }
    QJsonObject result{
        {QStringLiteral("scenario"), scenario},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("platform"), QGuiApplication::platformName()},
        {QStringLiteral("graphics_api"), graphics_api_name(window)},
        {QStringLiteral("render_loop_request"), QString::fromLocal8Bit(qgetenv("QSG_RENDER_LOOP"))},
        {QStringLiteral("elapsed_ms"), std::chrono::duration<double, std::milli>(after.time - before.time).count()},
        {QStringLiteral("process_cpu_ms"), cpu_ms(after.process) - cpu_ms(before.process)},
        {QStringLiteral("gui_cpu_ms"), cpu_ms(after.gui) - cpu_ms(before.gui)},
        {QStringLiteral("process_voluntary_context_switches"), (qint64)(after.process.ru_nvcsw - before.process.ru_nvcsw)},
        {QStringLiteral("process_involuntary_context_switches"), (qint64)(after.process.ru_nivcsw - before.process.ru_nivcsw)},
        {QStringLiteral("gui_voluntary_context_switches"), (qint64)(after.gui.ru_nvcsw - before.gui.ru_nvcsw)},
        {QStringLiteral("gui_involuntary_context_switches"), (qint64)(after.gui.ru_nivcsw - before.gui.ru_nivcsw)},
        {QStringLiteral("event_dispatcher_awake"), (qint64)(dispatcher_awake - awake_before)},
        {QStringLiteral("kernel_wakeups"), QJsonValue::Null},
        {QStringLiteral("canvas_timers"), timer_results},
        {QStringLiteral("prepare_count"), (qint64)(report_after.prepare_count - report_before.prepare_count)},
        {QStringLiteral("render_count"), (qint64)(report_after.render_count - report_before.render_count)},
        {QStringLiteral("after_rendering_signals"), (qint64)(render_signals.load() - render_before)},
        {QStringLiteral("frame_swapped_signals"), (qint64)(swap_signals.load() - swap_before)},
        {QStringLiteral("status_notifications"), (qint64)(status_notifications - status_before)},
        {QStringLiteral("error_notifications"), (qint64)(error_notifications - error_before)},
        {QStringLiteral("blink_notifications"), (qint64)(blink_notifications - blink_before)},
        {QStringLiteral("canvas_visible"), canvas ? QJsonValue(canvas->isVisible()) : QJsonValue::Null},
        {QStringLiteral("window_visible"), window.isVisible()},
        {QStringLiteral("window_exposed"), window.isExposed()},
        {QStringLiteral("render_ready"), canvas ? QJsonValue(canvas->render_ready()) : QJsonValue::Null},
        {QStringLiteral("render_error"), canvas ? QJsonValue(canvas->render_error()) : QJsonValue::Null},
    };
    bool resumed = true;
    if (hidden) {
        const auto resume_started = std::chrono::steady_clock::now();
        if (!canvas->set_canvas_frame(make_frame(blink, 102U))) {
            std::fprintf(stderr, "Resume probe frame was rejected.\n");
            return 1;
        }
        parent.setVisible(true);
        canvas->setVisible(true);
        window.show();
        resumed = wait_for_frame(*canvas);
        result.insert(QStringLiteral("resume_matches_frame"), resumed);
        result.insert(QStringLiteral("resume_ms"), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - resume_started).count());
    }
    window.hide();
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::printf("%s\n", json.constData());
    return resumed ? 0 : 1;
}
