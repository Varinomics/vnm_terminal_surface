#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/metrics_json.h"
#include "vnm_terminal/font_metrics.h"
#include "vnm_terminal/terminal_canvas_export.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QProcessEnvironment>
#include <QThread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

int run_terminal_child()
{
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD output_mode = 0;
    if (!GetConsoleMode(output, &output_mode) ||
        !SetConsoleMode(output, output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    {
        return 2;
    }
#endif
    std::printf("native-ready\r\n\x1b]0;native-ready\a");
    std::fflush(stdout);
    char buffer[80];
    while (std::fgets(buffer, sizeof(buffer), stdin)) {
        std::string command(buffer);
        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) {
            command.pop_back();
        }
        if (command == "size") {
            int rows = 0;
            int columns = 0;
#if defined(_WIN32)
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
                return 3;
            }
            rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            columns = info.srWindow.Right - info.srWindow.Left + 1;
#else
            winsize size{};
            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
                return 3;
            }
            rows = size.ws_row;
            columns = size.ws_col;
#endif
            std::printf("\x1b]0;native-size-%d-%d\a", rows, columns);
            std::fflush(stdout);
        }
        else
        if (command == "exit") {
            std::printf("native-exit\r\n");
            std::fflush(stdout);
            return 7;
        }
    }
    return 4;
}

template <typename Predicate>
bool wait_until(QGuiApplication& app, Predicate predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 10000) {
        app.processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

vnm_terminal::Terminal_process_start_request native_request()
{
    vnm_terminal::Terminal_process_start_request request;
    request.argv = {QCoreApplication::applicationFilePath(), QStringLiteral("--terminal-child")};
    request.working_directory = QDir::currentPath();
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& name : environment.keys()) {
        request.base_environment.push_back({name, environment.value(name)});
    }
    return request;
}

bool exported_text_contains(const VNM_TerminalSurface& surface, const QString& expected)
{
    const auto exported = vnm_terminal::export_terminal_canvas_frame(surface);
    if (!exported.frame) {
        return false;
    }
    QString text;
    for (const auto& cell : exported.frame->cells) {
        text += cell.text;
    }
    return text.contains(expected);
}

bool check_default_native_terminal(QGuiApplication& app)
{
    VNM_TerminalSurface surface;
    surface.setWidth(800);
    surface.setHeight(480);
    int exit_count = 0;
    int exit_code = -1;
    QObject::connect(&surface, &VNM_TerminalSurface::process_exited,
        [&exit_count, &exit_code](VNM_TerminalSurface::Exit_reason, int code) {
            ++exit_count;
            exit_code = code;
        });
    const auto started = surface.start_terminal(native_request());
    if (!check(started.accepted && started.native_dispatch_occurred,
            "ordinary start must launch a native terminal without a supplied owner"))
    {
        return false;
    }
    if (!check(wait_until(app, [&] {
            return surface.terminal_title() == QStringLiteral("native-ready") &&
                exported_text_contains(surface, QStringLiteral("native-ready"));
        }), "native child output must reach the public terminal canvas"))
    {
        return false;
    }
    bool ok = true;
#if !defined(_WIN32)
    ok &= check(!surface.process_cpu_time(),
        "ordinary POSIX ownership must report family CPU accounting as unavailable");
#endif
    const int previous_rows = surface.rows();
    const int previous_columns = surface.columns();
    surface.setWidth(960);
    surface.setHeight(576);
    surface.refresh_grid_from_item_geometry();
    ok &= check(wait_until(app, [&] {
        return surface.rows() != previous_rows && surface.columns() != previous_columns &&
            surface.backend_geometry_in_sync();
    }), "native backend must settle the changed grid");
    const QString expected_size = QStringLiteral("native-size-%1-%2")
        .arg(surface.rows()).arg(surface.columns());
    ok &= check(surface.paste_text(QStringLiteral("size\r")), "native input must be accepted");
    ok &= check(wait_until(app, [&] { return surface.terminal_title() == expected_size; }),
        "native child must observe the resized terminal dimensions");
    ok &= check(surface.paste_text(QStringLiteral("exit\r")), "native exit command must be accepted");
    ok &= check(wait_until(app, [&] { return exit_count == 1; }) && exit_code == 7,
        "native command exit must retain its actual status");
    ok &= check(exported_text_contains(surface, QStringLiteral("native-exit")),
        "native exit must follow delivery of final output");
    if (!ok) {
        return false;
    }
    const auto restarted = surface.start_terminal(native_request());
    if (!check(restarted.accepted && wait_until(app, [&] {
            return surface.terminal_title() == QStringLiteral("native-ready");
        }), "ordinary native start must work again after process exit"))
    {
        return false;
    }
    ok &= check(surface.terminate_process(), "closing the live native session must be accepted");
    ok &= check(wait_until(app, [&] { return exit_count == 2; }),
        "closing the live native session must deliver its exit observation");
    return ok;
}

vnm_terminal::Terminal_process_start_result invoke_standalone_boundary(
    VNM_TerminalSurface& surface,
    QString executable,
    QString working_directory,
    std::vector<vnm_terminal::Terminal_environment_entry> base_environment)
{
    return surface.start_terminal({
        {std::move(executable)},
        std::move(working_directory),
        std::move(base_environment),
        std::nullopt,
    });
}

vnm_terminal::Terminal_process_start_result invoke_worker_boundary(
    VNM_TerminalSurface& surface,
    QString executable,
    QString working_directory,
    std::vector<vnm_terminal::Terminal_environment_entry> base_environment,
    std::vector<vnm_terminal::Terminal_environment_entry> contribution)
{
    return surface.start_terminal({
        {std::move(executable)},
        std::move(working_directory),
        std::move(base_environment),
        std::move(contribution),
    });
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--terminal-child") {
        return run_terminal_child();
    }
    QGuiApplication app(argc, argv);
    const bool native_ok = check_default_native_terminal(app);

    // Prove the installed public diagnostics header is includable and that its
    // builders link from the packaged library, without constructing a surface.
    void (*append_atlas)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_atlas_metrics_json;
    void (*append_render_invalidation)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_render_invalidation_metrics_json;
    void (*append_backend_drain)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_backend_drain_metrics_json;

    // Prove the installed public font/metrics header is includable and that its
    // functions link from the packaged library.
    QString (*default_family)() = &vnm_terminal::default_monospace_font_family;
    vnm_terminal::Cell_metrics (*metrics_for_font)(
        const QString&,
        qreal,
        qreal,
        vnm_terminal::Font_advance_policy,
        qreal) = &vnm_terminal::cell_metrics_for_font;
    bool (*metrics_valid)(const vnm_terminal::Cell_metrics&) =
        &vnm_terminal::cell_metrics_valid;
    const auto standalone_boundary = &invoke_standalone_boundary;
    const auto worker_boundary = &invoke_worker_boundary;
    vnm_terminal::Terminal_canvas_export_result (*export_canvas)(
        const VNM_TerminalSurface&) =
            &vnm_terminal::export_terminal_canvas_frame;
    bool (VNM_TerminalCanvas::*set_canvas_frame)(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>) =
            &VNM_TerminalCanvas::set_canvas_frame;
    bool (VNM_TerminalCanvas::*content_extent_available)() const =
        &VNM_TerminalCanvas::content_extent_available;
    const vnm_terminal::terminal_canvas_content_extent_t content_extent;

    return (native_ok && append_atlas != nullptr &&
            append_render_invalidation != nullptr &&
            append_backend_drain != nullptr &&
            default_family != nullptr && metrics_for_font != nullptr &&
            metrics_valid != nullptr && standalone_boundary != nullptr &&
            worker_boundary != nullptr && export_canvas != nullptr &&
            set_canvas_frame != nullptr && content_extent_available != nullptr &&
            content_extent.record_version ==
                vnm_terminal::k_terminal_canvas_content_extent_version)
        ? 0
        : 1;
}
