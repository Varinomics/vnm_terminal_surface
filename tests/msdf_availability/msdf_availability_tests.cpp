#include "vnm_terminal/internal/msdf_availability_work.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/diagnostics/diagnostic_sink.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringView>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QTimerEvent>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace term = vnm_terminal::internal;
namespace diagnostics = vnm_terminal::diagnostics;

namespace {

using vnm_terminal::test_helpers::check;
using Surface_bridge = term::VNM_TerminalSurface_render_bridge;

class Blocked_probe
{
public:
    explicit Blocked_probe(bool available)
    :
        m_available(available),
        m_started(std::make_shared<std::promise<void>>()),
        m_started_signal(m_started->get_future()),
        m_release_signal(m_release.get_future().share()),
        m_calls(std::make_shared<std::atomic<int>>(0))
    {}

    ~Blocked_probe() { release(); }

    std::function<bool(const QFont&)> handler() const
    {
        return [
                available = m_available,
                started = m_started,
                release = m_release_signal,
                calls = m_calls
            ](const QFont&)
            {
                if (calls->fetch_add(1) == 0) {
                    started->set_value();
                }
                release.wait();
                return available;
            };
    }

    bool wait_until_started() const
    {
        return m_started_signal.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready;
    }

    int calls() const { return m_calls->load(); }

    void release()
    {
        if (!std::exchange(m_released, true)) {
            m_release.set_value();
        }
    }

private:
    bool                                  m_available;
    std::shared_ptr<std::promise<void>>   m_started;
    std::future<void>                     m_started_signal;
    std::promise<void>                    m_release;
    std::shared_future<void>              m_release_signal;
    std::shared_ptr<std::atomic<int>>     m_calls;
    bool                                  m_released = false;
};

class Timer_events final : public QObject
{
public:
    explicit Timer_events(QTimer& timer) { timer.installEventFilter(this); }

    int count() const { return m_count; }

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Timer) {
            ++m_count;
        }
        return false;
    }

private:
    int m_count = 0;
};

struct signal_counts_t
{
    int  checking_changes   = 0;
    int  available_changes  = 0;
    bool owner_thread_only  = true;
};

void observe_signals(VNM_TerminalSurface& surface, signal_counts_t& counts)
{
    QObject::connect(
        &surface, &VNM_TerminalSurface::msdf_text_checking_changed, &surface,
        [&surface, &counts] {
            ++counts.checking_changes;
            counts.owner_thread_only &= QThread::currentThread() == surface.thread();
        });
    QObject::connect(
        &surface, &VNM_TerminalSurface::msdf_text_available_changed, &surface,
        [&surface, &counts] {
            ++counts.available_changes;
            counts.owner_thread_only &= QThread::currentThread() == surface.thread();
        });
}

class Completion_diagnostics
{
public:
    Completion_diagnostics()
    {
        diagnostics::set_sink([
                counts = m_counts
            ](diagnostics::Level, QStringView message)
            {
                if (message.contains(u"MSDF availability dispatch admission failed")) {
                    ++counts->rejected;
                }
                if (message.contains(u"MSDF availability completion timed out")) {
                    ++counts->timed_out;
                }
            });
    }

    ~Completion_diagnostics() { diagnostics::clear_sink(); }

    int rejected() const { return m_counts->rejected.load(); }
    int timed_out() const { return m_counts->timed_out.load(); }

private:
    struct Counts
    {
        std::atomic<int> rejected{0};
        std::atomic<int> timed_out{0};
    };

    std::shared_ptr<Counts> m_counts = std::make_shared<Counts>();
};

class Application_dispatch_rejection
{
public:
    explicit Application_dispatch_rejection(QGuiApplication& app)
    :
        m_app(app),
        m_application_thread(app.thread()),
        m_dispatcher(QAbstractEventDispatcher::instance(app.thread())),
        m_dispatcher_parent(m_dispatcher->parent())
    {
        // Qt parents the dispatcher to the application. Moving that child
        // would also move the queue drained by the Unix event pump, so detach
        // it while rejecting only the application receiver's admission.
        m_dispatcher->setParent(nullptr);
        m_app.moveToThread(nullptr);
    }

    ~Application_dispatch_rejection()
    {
        m_app.moveToThread(m_application_thread);
        m_dispatcher->setParent(m_dispatcher_parent);
    }

private:
    QGuiApplication&                m_app;
    QThread* const                  m_application_thread;
    QAbstractEventDispatcher* const m_dispatcher;
    QObject* const                  m_dispatcher_parent;
};

bool pump_until(
    QGuiApplication&           app,
    const std::function<bool()>& predicate,
    int                        timeout_ms = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        app.processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }

    return predicate();
}

void pump_for(QGuiApplication& app, int duration_ms)
{
    QElapsedTimer timer;
    timer.start();
    (void)pump_until(app, [&] { return timer.elapsed() >= duration_ms; }, duration_ms + 1000);
}

bool finish_workers()
{
    return check(QThreadPool::globalInstance()->waitForDone(5000),
        "availability workers finish within the test deadline");
}

bool test_queued_checks_coalesce_and_restart()
{
    bool ok = true;
    auto work = std::make_shared<term::Msdf_availability_work>();
    int checks = 0;
    int last   = 0;

    auto payload = std::make_shared<int>(1);
    const std::weak_ptr<int> obsolete_payload = payload;
    ok &= check(work->submit([payload, &checks] { ++checks; }),
        "the first availability check admits one worker");
    payload.reset();
    for (int selection = 2; selection <= 32; ++selection) {
        ok &= check(!work->submit([
                selection, &checks, &last
            ]()
            {
                ++checks;
                last = selection;
            }),
            "pending font selections reuse the admitted worker");
    }
    ok &= check(obsolete_payload.expired(),
        "replacing a pending check releases its captured state immediately");

    term::Msdf_availability_work::run(work);
    ok &= check(checks == 1 && last == 32,
        "only the final pending font selection is checked");
    ok &= check(work->submit([&checks] { ++checks; }),
        "a completed worker allows a subsequent check to start");
    term::Msdf_availability_work::run(work);
    ok &= check(checks == 2, "a new check runs after the previous drain became idle");
    return ok;
}

bool test_active_check_keeps_only_the_latest_pending_selection()
{
    bool ok = true;
    auto work = std::make_shared<term::Msdf_availability_work>();
    std::promise<void> started;
    std::promise<void> release;
    auto started_signal = started.get_future();
    auto release_signal = release.get_future();
    int checks = 0;
    int last   = 0;
    work->submit([&] {
        ++checks;
        started.set_value();
        release_signal.wait();
    });
    std::thread worker([weak_work = std::weak_ptr(work)] {
        term::Msdf_availability_work::run(weak_work);
    });
    ok &= check(started_signal.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "the first availability check starts on the worker");

    for (int selection = 2; selection <= 32; ++selection) {
        ok &= check(!work->submit([
                selection, &checks, &last
            ]()
            {
                ++checks;
                last = selection;
            }),
            "font changes during a check do not admit another worker");
    }
    release.set_value();
    worker.join();
    ok &= check(checks == 2 && last == 32,
        "the active check is followed by only the latest pending selection");
    return ok;
}

bool test_destroyed_owner_discards_pending_checks()
{
    bool ok = true;
    int checks = 0;
    auto work = std::make_shared<term::Msdf_availability_work>();
    work->submit([&checks] { ++checks; });
    std::weak_ptr<term::Msdf_availability_work> weak_work = work;
    work.reset();
    term::Msdf_availability_work::run(weak_work);
    ok &= check(checks == 0, "destruction before worker admission discards queued checks");

    work = std::make_shared<term::Msdf_availability_work>();
    weak_work = work;
    std::promise<void> started;
    std::promise<void> release;
    auto started_signal = started.get_future();
    auto release_signal = release.get_future();
    work->submit([&] {
        ++checks;
        started.set_value();
        release_signal.wait();
    });
    std::thread worker([weak_work] {
        term::Msdf_availability_work::run(weak_work);
    });
    ok &= check(started_signal.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "the active check starts before its owner is destroyed");
    work->submit([&checks] { ++checks; });
    work.reset();
    ok &= check(weak_work.expired(),
        "an active bake does not retain the owner of pending checks");
    release.set_value();
    worker.join();
    ok &= check(checks == 1,
        "destruction during a bake allows only the already active check to finish");
    return ok;
}

bool test_success_uses_completion_events(QGuiApplication& app, bool delayed)
{
    bool ok = true;
    VNM_TerminalSurface surface;
    signal_counts_t counts;
    observe_signals(surface, counts);
    const bool available = !surface.msdf_text_available();
    Blocked_probe probe(available);
    Surface_bridge::set_msdf_availability_probe_for_testing(surface, probe.handler());
    auto& deadline = Surface_bridge::msdf_availability_deadline_timer_for_testing(surface);
    Timer_events timer_events(deadline);

    if (!delayed) {
        probe.release();
    }
    surface.set_font_family(surface.font_family() + QStringLiteral(" completion event"));
    ok &= check(probe.wait_until_started(), "the availability worker starts");
    ok &= check(deadline.isSingleShot() && deadline.timerType() == Qt::PreciseTimer &&
            deadline.interval() == 30000 && deadline.isActive(),
        "a pending availability check arms one precise thirty-second deadline");
    if (delayed) {
        pump_for(app, 60);
        ok &= check(surface.msdf_text_checking() && timer_events.count() == 0,
            "a blocked availability worker causes no recurring timer callbacks");
        probe.release();
    }
    ok &= finish_workers();
    ok &= check(pump_until(app, [&] { return !surface.msdf_text_checking(); }, 5000),
        "the posted result settles checking through event delivery");
    ok &= check(surface.msdf_text_available() == available && !deadline.isActive(),
        "successful completion applies availability and cancels its deadline");
    ok &= check(counts.checking_changes == 2 && counts.available_changes == 1 &&
            counts.owner_thread_only && timer_events.count() == 0,
        "success publishes coherent owner-thread signals without a timer callback");
    return ok;
}

bool test_deadline_and_late_result(QGuiApplication& app)
{
    bool ok = true;
    Completion_diagnostics diagnostic;
    VNM_TerminalSurface surface;
    signal_counts_t counts;
    observe_signals(surface, counts);
    const bool initial_available = surface.msdf_text_available();
    Blocked_probe probe(!initial_available);
    Surface_bridge::set_msdf_availability_probe_for_testing(surface, probe.handler());
    auto& deadline = Surface_bridge::msdf_availability_deadline_timer_for_testing(surface);
    deadline.setInterval(25);
    Timer_events timer_events(deadline);

    surface.set_font_family(surface.font_family() + QStringLiteral(" delayed result"));
    ok &= check(probe.wait_until_started(), "the timed availability worker starts");
    // A delayed GUI loop must expire elapsed time on its first delivery; it
    // must not need a backlog of nominal polling intervals to finish checking.
    QThread::msleep(60);
    ok &= check(pump_until(app, [&] { return !surface.msdf_text_checking(); }, 5000),
        "one deadline delivery ends checking after a delayed GUI loop");
    ok &= check(timer_events.count() == 1 && !deadline.isActive() &&
            surface.msdf_text_available() == initial_available,
        "timeout stops checking without inventing an availability result");
    pump_for(app, 60);
    ok &= check(timer_events.count() == 1 && diagnostic.timed_out() == 1,
        "the expired deadline neither repeats nor logs another timeout");

    probe.release();
    ok &= finish_workers();
    ok &= check(pump_until(app, [&] {
            return surface.msdf_text_available() != initial_available;
        }, 5000),
        "a late result for the same font generation still updates availability");
    ok &= check(!surface.msdf_text_checking() && counts.checking_changes == 2 &&
            counts.available_changes == 1 && counts.owner_thread_only,
        "a late result does not restart or repeat the checking transition");
    return ok;
}

bool test_superseded_completion_and_deadline(QGuiApplication& app, bool reject_dispatch)
{
    bool ok = true;
    Completion_diagnostics diagnostic;
    VNM_TerminalSurface surface;
    signal_counts_t counts;
    observe_signals(surface, counts);
    const bool initial_available = surface.msdf_text_available();
    const QString first_family = surface.font_family() + QStringLiteral(" first selection");
    const QString final_family = surface.font_family() + QStringLiteral(" final selection");
    Blocked_probe first_probe(!initial_available);
    Blocked_probe final_probe(initial_available);
    const auto checked_final_family = std::make_shared<std::atomic<bool>>(false);
    Surface_bridge::set_msdf_availability_probe_for_testing(surface, [
            first_family,
            final_family,
            checked_final_family,
            first = first_probe.handler(),
            last = final_probe.handler()
        ](const QFont& font)
        {
            if (font.family() == first_family) {
                return first(font);
            }
            checked_final_family->store(font.family() == final_family);
            return last(font);
        });
    auto& deadline = Surface_bridge::msdf_availability_deadline_timer_for_testing(surface);
    surface.set_font_family(first_family);
    ok &= check(first_probe.wait_until_started(), "the superseded worker starts");
    const int stale_timer_id = deadline.timerId();
    for (int selection = 0; selection != 8; ++selection) {
        surface.set_font_family(final_family + QString::number(selection));
    }
    surface.set_font_family(final_family);

    std::optional<Application_dispatch_rejection> dispatch_rejection;
    if (reject_dispatch) {
        dispatch_rejection.emplace(app);
        ok &= check(app.thread() == nullptr &&
                QAbstractEventDispatcher::instance()->thread() == QThread::currentThread(),
            "application dispatch rejection preserves the GUI event dispatcher's affinity");
    }
    first_probe.release();
    ok &= check(final_probe.wait_until_started(), "only the latest pending check starts next");
    dispatch_rejection.reset();
    app.processEvents();
    QTimerEvent stale_deadline(stale_timer_id);
    QCoreApplication::sendEvent(&deadline, &stale_deadline);
    ok &= check(surface.msdf_text_checking() && deadline.isActive() &&
            surface.msdf_text_available() == initial_available,
        "stale completion and deadline delivery leave the latest check unchanged");

    final_probe.release();
    ok &= finish_workers();
    ok &= check(pump_until(app, [&] { return !surface.msdf_text_checking(); }, 5000),
        "the latest font selection completes after superseded delivery");
    ok &= check(first_probe.calls() == 1 && final_probe.calls() == 1 &&
            checked_final_family->load(),
        "rapid family changes retain one running and one latest pending probe");
    ok &= check(counts.checking_changes == 2 && counts.available_changes == 0 &&
            counts.owner_thread_only && !deadline.isActive(),
        "superseded results cannot publish availability or settle newer checking");
    ok &= check(diagnostic.rejected() == 0 && diagnostic.timed_out() == 0,
        "stale failure delivery does not diagnose a failure of the current check");
    return ok;
}

bool test_reentrant_selection_during_completion(QGuiApplication& app)
{
    bool ok = true;
    VNM_TerminalSurface surface;
    signal_counts_t counts;
    observe_signals(surface, counts);
    const bool initial_available = surface.msdf_text_available();
    const QString first_family = surface.font_family() + QStringLiteral(" completed selection");
    const QString final_family = surface.font_family() + QStringLiteral(" reentrant selection");
    Blocked_probe final_probe(initial_available);
    Surface_bridge::set_msdf_availability_probe_for_testing(surface, [
            first_family,
            initial_available,
            last = final_probe.handler()
        ](const QFont& font)
        {
            return font.family() == first_family ? !initial_available : last(font);
        });
    bool selected_again = false;
    QObject::connect(
        &surface, &VNM_TerminalSurface::msdf_text_checking_changed, &surface,
        [&] {
            if (!surface.msdf_text_checking() && !selected_again) {
                selected_again = true;
                surface.set_font_family(final_family);
            }
        });

    surface.set_font_family(first_family);
    ok &= check(pump_until(app, [&] { return selected_again; }, 5000),
        "completion notification can synchronously select another font");
    ok &= check(final_probe.wait_until_started(), "the reentrant font check starts");
    ok &= check(surface.msdf_text_checking() &&
            surface.msdf_text_available() == initial_available,
        "the completed generation cannot update availability after a reentrant selection");
    final_probe.release();
    ok &= finish_workers();
    ok &= check(pump_until(app, [&] { return !surface.msdf_text_checking(); }, 5000),
        "the reentrant selection finishes its own checking state");
    ok &= check(counts.checking_changes == 4 && counts.available_changes == 0 &&
            counts.owner_thread_only,
        "reentrant completion preserves each checking transition and result generation");
    return ok;
}

bool test_surface_destruction_with_pending_completion(QGuiApplication& app, bool worker_blocked)
{
    bool ok = true;
    Blocked_probe probe(false);
    auto surface = std::make_unique<VNM_TerminalSurface>();
    const QPointer<VNM_TerminalSurface> guard(surface.get());
    Surface_bridge::set_msdf_availability_probe_for_testing(*surface, probe.handler());
    surface->set_font_family(surface->font_family() + QStringLiteral(" destroyed selection"));
    ok &= check(probe.wait_until_started(), "the worker starts before surface destruction");
    if (worker_blocked) {
        surface->set_font_family(surface->font_family() + QStringLiteral(" discarded pending"));
    }
    else {
        probe.release();
        ok &= finish_workers();
    }
    surface.reset();
    ok &= check(guard.isNull(), "surface destruction does not wait for worker completion");
    probe.release();
    ok &= finish_workers();
    app.processEvents();
    ok &= check(probe.calls() == 1,
        "destroying the surface discards pending checks and safely drains queued completion");
    return ok;
}

bool test_reentrant_font_selection_finishes_checking(QGuiApplication& app)
{
    bool ok = true;
    VNM_TerminalSurface surface;
    const QString final_family = surface.font_family();
    bool changed_from_signal = false;
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::msdf_text_checking_changed,
        &app,
        [&] {
            if (surface.msdf_text_checking() && !changed_from_signal) {
                changed_from_signal = true;
                surface.set_font_family(final_family);
            }
        });

    surface.set_font_family(final_family + QStringLiteral(" superseded selection"));
    ok &= check(changed_from_signal && surface.font_family() == final_family,
        "the checking signal selects a newer font before the first request is queued");
    ok &= check(pump_until(app, [&surface] { return !surface.msdf_text_checking(); }),
        "the reentrant final font selection retains its availability check");
    return ok;
}


bool test_msdf_shared_cpu_bake_reuse(QGuiApplication& app)
{
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
    bool ok = true;
    QThreadPool::globalInstance()->waitForDone();
    ok &= check(term::qsg_atlas_clear_msdf_shared_bakes_for_testing(),
        "MSDF shared-bake test starts without in-flight work");

    if (!check(term::vnm_terminal_default_monospace_font_loaded(),
            "shared-bake validation loads the bundled font fixture"))
    {
        return false;
    }
    const QFont font = term::vnm_terminal_font(
        term::vnm_terminal_default_monospace_font_family(), 18.0);
    const bool first_available =
        term::qsg_atlas_msdf_text_available_for_font(font);
    const auto initial = term::qsg_atlas_msdf_shared_bake_stats();
    if (!check(first_available && initial.completed_atlases == 1U &&
            initial.completed_bytes > 0U,
            "the bundled fixture produces an available nonempty CPU atlas"))
    {
        return false;
    }

    const bool second_available =
        term::qsg_atlas_msdf_text_available_for_font(font);
    const auto repeated = term::qsg_atlas_msdf_shared_bake_stats();
    ok &= check(second_available && repeated.build_calls == 1U &&
            repeated.completed_reuses >= 1U &&
            repeated.completed_atlases == 1U &&
            repeated.completed_bytes <= 32U * 1024U * 1024U,
        "matching availability checks reuse one CPU bake and bounded pixels");

    ok &= check(term::qsg_atlas_clear_msdf_shared_bakes_for_testing(),
        "completed MSDF CPU results can be released for the concurrency test");
    std::promise<void> start;
    const std::shared_future<void> gate = start.get_future().share();
    const auto check_font = [font, gate] {
        gate.wait();
        return term::qsg_atlas_msdf_text_available_for_font(font);
    };
    auto first = std::async(std::launch::async, check_font);
    auto second = std::async(std::launch::async, check_font);
    start.set_value();
    const bool concurrent_first = first.get();
    const bool concurrent_second = second.get();
    const auto concurrent = term::qsg_atlas_msdf_shared_bake_stats();
    ok &= check(concurrent_first && concurrent_second &&
            concurrent.build_calls == 1U &&
            concurrent.inflight_reuses + concurrent.completed_reuses >= 1U,
        "simultaneous availability requests share one actual bake");

    ok &= check(term::qsg_atlas_clear_msdf_shared_bakes_for_testing(),
        "completed CPU results can be released before real surface requests");
    VNM_TerminalSurface first_surface;
    VNM_TerminalSurface second_surface;
    // An empty family selects the same bundled bytes, while changing the
    // constructor's explicit family starts each surface's real async check.
    first_surface.set_font_family(QString());
    second_surface.set_font_family(QString());
    ok &= check(first_surface.msdf_text_checking() && second_surface.msdf_text_checking(),
        "two live surfaces start independent availability checks");
    ok &= check(pump_until(app, [&] {
            return !first_surface.msdf_text_checking() &&
                !second_surface.msdf_text_checking();
        }),
        "both real surface availability checks complete");
    ok &= finish_workers();
    const auto surfaces = term::qsg_atlas_msdf_shared_bake_stats();
    ok &= check(first_surface.msdf_text_available() && second_surface.msdf_text_available() &&
            surfaces.build_calls == 1U &&
            surfaces.inflight_reuses + surfaces.completed_reuses >= 1U,
        "two live surfaces publish availability from one shared CPU bake");
    return ok;
#else
    (void)app;
    return true;
#endif
}

bool test_dispatch_rejection_clears_checking_and_allows_retry(QGuiApplication& app)
{
    bool ok = true;
    Completion_diagnostics diagnostic;
    VNM_TerminalSurface surface;
    signal_counts_t counts;
    observe_signals(surface, counts);
    const bool initial_available = surface.msdf_text_available();
    Surface_bridge::set_msdf_availability_probe_for_testing(
        surface, [initial_available](const QFont&) { return !initial_available; });
    auto& deadline = Surface_bridge::msdf_availability_deadline_timer_for_testing(surface);
    Timer_events timer_events(deadline);

    const QString retry_family = surface.font_family();
    const QString rejected_family =
        retry_family + QStringLiteral(" forced dispatch rejection");
    QThread* const application_thread = app.thread();
    auto* const dispatcher = QAbstractEventDispatcher::instance();
    QObject* const dispatcher_parent = dispatcher->parent();
    {
        Application_dispatch_rejection dispatch_rejection(app);
        ok &= check(app.thread() == nullptr &&
                dispatcher->thread() == application_thread,
            "rejected application dispatch retains a working GUI event dispatcher");

        surface.set_font_family(rejected_family);
        ok &= check(
            surface.msdf_text_checking(),
            "MSDF availability check enters checking state before worker dispatch");
        ok &= finish_workers();
        ok &= check(
            pump_until(app, [&surface] {
                return !surface.msdf_text_checking();
            }, 5000),
            "MSDF availability dispatch rejection clears the checking state");
    }

    ok &= check(app.thread() == application_thread &&
            dispatcher->thread() == application_thread && dispatcher->parent() == dispatcher_parent,
        "dispatch rejection restores application affinity and dispatcher ownership");
    ok &= check(surface.msdf_text_available() == initial_available &&
            counts.available_changes == 0 && !deadline.isActive(),
        "rejected dispatch clears checking without applying the computed availability");
    ok &= check(diagnostic.rejected() == 1 && diagnostic.timed_out() == 0 &&
            timer_events.count() == 0,
        "rejection is diagnosed by completion delivery independently of the deadline");

    Surface_bridge::set_msdf_availability_probe_for_testing(surface, {});
    const bool expected_available =
        term::qsg_atlas_msdf_text_available_for_font(QFont(retry_family));
    surface.set_font_family(retry_family);
    ok &= check(
        surface.msdf_text_checking(),
        "MSDF availability retry re-enters checking state");
    ok &= check(
        pump_until(app, [&surface] {
            return !surface.msdf_text_checking();
        }),
        "MSDF availability retry completes after dispatch affinity is restored");
    ok &= check(
        surface.msdf_text_available() == expected_available,
        "MSDF availability retry applies the worker result");
    ok &= check(
        counts.checking_changes == 4 && counts.owner_thread_only,
        "MSDF availability rejection and retry each publish checking transitions");

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    bool ok = true;
    ok &= test_queued_checks_coalesce_and_restart();
    ok &= test_active_check_keeps_only_the_latest_pending_selection();
    ok &= test_destroyed_owner_discards_pending_checks();
    ok &= test_success_uses_completion_events(app, false);
    ok &= test_success_uses_completion_events(app, true);
    ok &= test_deadline_and_late_result(app);
    ok &= test_superseded_completion_and_deadline(app, false);
    ok &= test_superseded_completion_and_deadline(app, true);
    ok &= test_reentrant_selection_during_completion(app);
    ok &= test_surface_destruction_with_pending_completion(app, false);
    ok &= test_surface_destruction_with_pending_completion(app, true);
    ok &= test_reentrant_font_selection_finishes_checking(app);
    ok &= test_dispatch_rejection_clears_checking_and_allows_retry(app);
    ok &= test_msdf_shared_cpu_bake_reuse(app);
    return ok ? 0 : 1;
}
