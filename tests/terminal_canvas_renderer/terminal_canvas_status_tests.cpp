#include "helpers/test_check.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_canvas_render_bridge.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

namespace renderer = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;

std::shared_ptr<vnm_terminal::Terminal_canvas_frame> make_frame(std::uint64_t sequence)
{
    auto frame = std::make_shared<vnm_terminal::Terminal_canvas_frame>();
    frame->rows                   = 2;
    frame->columns                = 12;
    frame->cell_width             = 10.0;
    frame->cell_height            = 20.0;
    frame->content_width          = 120.0;
    frame->content_height         = 40.0;
    frame->sequence               = sequence;
    frame->publication_generation = sequence;
    frame->styles.push_back({0xffffffffU, 0xff101010U, 0U});
    frame->cells.push_back({0, 0, 1, 0U, QStringLiteral("A")});
    return frame;
}

renderer::Captured_atlas_frame capture(const VNM_TerminalCanvas& canvas, std::uint64_t ownership)
{
    renderer::Captured_atlas_frame frame;
    frame.snapshot                = renderer::VNM_TerminalCanvas_render_bridge::render_snapshot(canvas);
    frame.capture_sequence        = canvas.frame_generation();
    frame.canvas_frame_generation = canvas.frame_generation();
    frame.ownership_generation    = ownership;
    frame.publication_generation  = canvas.canvas_frame()->publication_generation;
    return frame;
}

void prepare(
    renderer::Qsg_atlas_recorder& recorder,
    const renderer::Captured_atlas_frame& frame,
    bool committed = true,
    const renderer::Glyph_atlas_cache_stats& cache = {},
    const renderer::Qsg_atlas_frame_build_summary& build = {})
{
    recorder.record_prepare(
        frame, 150U, true, true, true, false, false, false, false,
        0, 0U, 0U, cache, build, {}, {}, {}, committed);
}

void deliver(QObject& receiver)
{
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
}

bool test_idle_canvas_has_no_periodic_status_work()
{
    VNM_TerminalCanvas empty;
    VNM_TerminalCanvas visible;
    VNM_TerminalCanvas hidden;
    if (!check(visible.set_canvas_frame(make_frame(101U)) && hidden.set_canvas_frame(make_frame(102U)),
            "idle canvases accept non-blinking frames"))
    {
        return false;
    }
    hidden.setVisible(false);
    QObject observer;
    unsigned timeouts = 0U;
    for (auto* canvas : {&empty, &visible, &hidden}) {
        for (auto* timer : canvas->findChildren<QTimer*>()) {
            QObject::connect(timer, &QTimer::timeout, &observer, [&] { ++timeouts; });
        }
    }
    QEventLoop loop;
    QTimer::singleShot(350, &loop, &QEventLoop::quit);
    loop.exec();
    return check(timeouts == 0U,
        "unchanged no-frame, visible and hidden canvases perform no periodic status work");
}

bool test_burst_coalescing_and_rearm()
{
    renderer::Qsg_atlas_recorder recorder;
    QObject receiver;
    std::atomic_uint wakes = 0U;
    std::vector<std::uint64_t> deliveries;
    bool correct_thread = true;
    bool produce_during_delivery = false;
    renderer::Captured_atlas_frame frame;
    frame.ownership_generation = 3U;
    QObject::connect(&recorder, &renderer::Qsg_atlas_recorder::render_status_changed, &receiver,
        [&] {
            // A synchronous diagnostic reader also proves that producer
            // notification does not execute under the recorder mutex.
            (void)recorder.snapshot();
            ++wakes;
        }, Qt::DirectConnection);
    QObject::connect(&recorder, &renderer::Qsg_atlas_recorder::render_status_changed, &receiver,
        [&] {
            const auto status = recorder.take_render_status();
            deliveries.push_back(status.render_canvas_frame_generation);
            correct_thread &= QThread::currentThread() == receiver.thread();
            if (produce_during_delivery) {
                produce_during_delivery = false;
                std::thread producer([&] {
                    ++frame.capture_sequence;
                    ++frame.canvas_frame_generation;
                    prepare(recorder, frame);
                    recorder.record_render(frame, 180U, {}, true);
                });
                producer.join();
            }
        }, Qt::QueuedConnection);

    std::thread producer([&] {
        for (std::uint64_t index = 1U; index <= 300U; ++index) {
            frame.capture_sequence        = index;
            frame.canvas_frame_generation = index;
            prepare(recorder, frame);
            recorder.record_render(frame, index, {}, true);
        }
    });
    producer.join();
    bool ok = check(wakes == 1U && deliveries.empty(),
        "a blocked GUI receives one queued wake for a complete producer burst");
    (void)recorder.snapshot();
    ok &= check(wakes == 1U, "diagnostic snapshots do not rearm status delivery");
    produce_during_delivery = true;
    deliver(receiver);
    ok &= check(deliveries == std::vector<std::uint64_t>{300U} && wakes == 2U,
        "consumption includes the latest frame and a racing successor rearms one wake");
    deliver(receiver);
    ok &= check(deliveries == std::vector<std::uint64_t>{300U, 301U} && correct_thread,
        "the final successor reaches the consumer's QObject thread");

    renderer::Glyph_atlas_cache_stats cache;
    for (std::uint64_t index = 0U; index < 100U; ++index) {
        cache.lookups = cache.hits = index;
        recorder.record_capture(frame);
        prepare(recorder, frame, true, cache);
        recorder.record_render(frame, index, {}, true);
    }
    deliver(receiver);
    ok &= check(wakes == 2U && deliveries.size() == 2U,
        "diagnostic-only capture, prepare and render updates do not notify the GUI");
    return ok;
}

bool test_canvas_failure_recovery_and_ownership()
{
    VNM_TerminalCanvas canvas;
    const auto recorder = std::const_pointer_cast<renderer::Qsg_atlas_recorder>(
        renderer::VNM_TerminalCanvas_render_bridge::qsg_atlas_recorder(canvas));
    if (!check(canvas.set_canvas_frame(make_frame(201U)), "the canvas accepts its first frame")) {
        return false;
    }
    auto frame = capture(canvas, 1U);
    std::thread failed_prepare([&] { prepare(*recorder, frame, false); });
    failed_prepare.join();
    deliver(canvas);
    bool ok = check(!canvas.render_ready() && !canvas.render_error().isEmpty(),
        "prepare failure is published before any successful render");

    std::thread recovered([&] {
        prepare(*recorder, frame);
        recorder->record_render(frame, 200U, {}, true);
    });
    recovered.join();
    deliver(canvas);
    ok &= check(canvas.render_ready() && canvas.render_error().isEmpty() &&
            canvas.rendered_frame_generation() == canvas.frame_generation() &&
            canvas.rendered_frame_sequence() == 201U && canvas.rendered_publication_generation() == 201U,
        "successful recovery publishes the presented identity consumed by Companion");

    ++frame.capture_sequence;
    prepare(*recorder, frame, false);
    ok &= check(canvas.set_canvas_frame({}) && canvas.set_canvas_frame(make_frame(202U)),
        "clearing and replacing a canvas retires its previous ownership");
    recorder->record_render(frame, 200U, {}, true);
    deliver(canvas);
    ok &= check(!canvas.render_ready() && canvas.render_error().isEmpty() &&
            canvas.rendered_frame_generation() == 0U,
        "a pending retired-generation notification cannot revive readiness or errors");
    frame = capture(canvas, 2U);
    prepare(*recorder, frame);
    recorder->record_render(frame, 200U, {}, true);
    deliver(canvas);
    ok &= check(canvas.render_ready() && canvas.rendered_frame_sequence() == 202U &&
            canvas.rendered_publication_generation() == 202U,
        "the replacement ownership can publish after a stale notification was consumed");
    return ok;
}

bool test_pending_notification_survives_receiver_retirement()
{
    auto canvas = std::make_unique<VNM_TerminalCanvas>();
    if (!check(canvas->set_canvas_frame(make_frame(301U)), "the retiring canvas accepts a frame")) {
        return false;
    }
    const auto recorder = std::const_pointer_cast<renderer::Qsg_atlas_recorder>(
        renderer::VNM_TerminalCanvas_render_bridge::qsg_atlas_recorder(*canvas));
    const auto frame = capture(*canvas, 1U);
    unsigned delivered = 0U;
    QObject::connect(recorder.get(), &renderer::Qsg_atlas_recorder::render_status_changed, canvas.get(),
        [&] { ++delivered; }, Qt::QueuedConnection);
    prepare(*recorder, frame);
    canvas.reset();
    std::thread retained_render_node([&] { recorder->record_render(frame, 200U, {}, true); });
    retained_render_node.join();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    VNM_TerminalCanvas replacement;
    return check(delivered == 0U && !replacement.render_ready() && replacement.render_error().isEmpty(),
        "a recorder retained by a render node cannot call or update a destroyed canvas");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    bool ok = true;
    ok &= test_idle_canvas_has_no_periodic_status_work();
    ok &= test_burst_coalescing_and_rearm();
    ok &= test_canvas_failure_recovery_and_ownership();
    ok &= test_pending_notification_survives_receiver_retirement();
    return ok ? 0 : 1;
}
