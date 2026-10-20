#include "vnm_terminal/internal/msdf_availability_work.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QString>
#include <QThread>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

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

bool test_dispatch_rejection_clears_checking_and_allows_retry(QGuiApplication& app)
{
    bool ok = true;

    VNM_TerminalSurface surface;
    int checking_change_count = 0;
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::msdf_text_checking_changed,
        &app,
        [&checking_change_count] {
            ++checking_change_count;
        });

    const QString retry_family = surface.font_family();
    const QString rejected_family =
        retry_family + QStringLiteral(" forced dispatch rejection");
    QThread* const application_thread = app.thread();
    app.moveToThread(nullptr);
    ok &= check(
        app.thread() == nullptr,
        "MSDF availability test removes the application dispatch affinity");

    surface.set_font_family(rejected_family);
    ok &= check(
        surface.msdf_text_checking(),
        "MSDF availability check enters checking state before worker dispatch");
    ok &= check(
        pump_until(app, [&surface] {
            return !surface.msdf_text_checking();
        }),
        "MSDF availability dispatch rejection clears the checking state");

    app.moveToThread(application_thread);
    ok &= check(
        app.thread() == application_thread,
        "MSDF availability test restores the application dispatch affinity");

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
        checking_change_count == 4,
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
    ok &= test_reentrant_font_selection_finishes_checking(app);
    ok &= test_dispatch_rejection_clears_checking_and_allows_retry(app);
    return ok ? 0 : 1;
}
