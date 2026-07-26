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

#include <functional>

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
    return test_dispatch_rejection_clears_checking_and_allows_retry(app) ? 0 : 1;
}
