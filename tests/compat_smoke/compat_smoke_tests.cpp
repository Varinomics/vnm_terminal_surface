#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool argument_equals(const char* argument, const char* expected)
{
    return std::string_view(argument) == expected;
}

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

template <typename Predicate>
bool pump_until(QGuiApplication& app, Predicate predicate, int timeout_ms = 10000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        app.processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) {
            return true;
        }

        QThread::msleep(10);
    }

    app.processEvents(QEventLoop::AllEvents, 50);
    return predicate();
}

struct surface_grid_size_t
{
    int rows       = 0;
    int columns    = 0;
};

surface_grid_size_t current_surface_grid(const VNM_TerminalSurface& surface)
{
    return {surface.rows(), surface.columns()};
}

bool surface_grid_size_is_positive(surface_grid_size_t grid)
{
    return grid.rows > 0 && grid.columns > 0;
}

bool same_surface_grid_size(surface_grid_size_t lhs, surface_grid_size_t rhs)
{
    return lhs.rows == rhs.rows && lhs.columns == rhs.columns;
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

std::vector<QString> snapshot_rows(const term::Terminal_render_snapshot& snapshot)
{
    std::vector<QString> rows;
    rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        rows.push_back(snapshot_row_text(snapshot, row));
    }
    return rows;
}

bool snapshot_contains_text(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (const QString& row : snapshot_rows(snapshot)) {
        if (row.contains(text)) {
            return true;
        }
    }

    return false;
}

bool snapshot_contains_row(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (const QString& row : snapshot_rows(snapshot)) {
        if (row == text) {
            return true;
        }
    }

    return false;
}

std::shared_ptr<const term::Terminal_render_snapshot> current_snapshot(
    VNM_TerminalSurface& surface)
{
    return term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
}

bool wait_for_snapshot_text(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const QString&         text)
{
    return pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        return snapshot != nullptr && snapshot_contains_text(*snapshot, text);
    });
}

bool wait_for_snapshot_row(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const QString&         text)
{
    return pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        return snapshot != nullptr && snapshot_contains_row(*snapshot, text);
    });
}

bool wait_for_surface_resize(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    surface_grid_size_t    initial_grid,
    surface_grid_size_t&   resized_grid)
{
    return pump_until(app, [&] {
        const surface_grid_size_t grid = current_surface_grid(surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (!surface.backend_geometry_in_sync()        ||
            !surface_grid_size_is_positive(grid)       ||
            same_surface_grid_size(grid, initial_grid) ||
            snapshot                    == nullptr     ||
            snapshot->grid_size.rows    != grid.rows   ||
            snapshot->grid_size.columns != grid.columns)
        {
            return false;
        }

        resized_grid = grid;
        return true;
    });
}

bool wait_for_surface_initial_sync(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    surface_grid_size_t&   grid)
{
    return pump_until(app, [&] {
        const surface_grid_size_t current_grid = current_surface_grid(surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (!surface.backend_geometry_in_sync()              ||
            !surface_grid_size_is_positive(current_grid)     ||
            snapshot                    == nullptr           ||
            snapshot->grid_size.rows    != current_grid.rows ||
            snapshot->grid_size.columns != current_grid.columns)
        {
            return false;
        }

        grid = current_grid;
        return true;
    });
}

QString stream_line_for_count(int count)
{
    return QStringLiteral("stream-row-%1").arg(count, 3, 10, QChar(u'0'));
}

QString size_line_for_grid(
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t&
                           contract,
    surface_grid_size_t    grid)
{
    return QStringLiteral("%1%2x%3")
        .arg(QString::fromLatin1(
            contract.size_prefix.data(),
            static_cast<qsizetype>(contract.size_prefix.size())))
        .arg(grid.rows)
        .arg(grid.columns);
}

bool wait_for_size_report(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t&
                           contract,
    surface_grid_size_t    grid,
    int                    timeout_ms = 10000)
{
    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    const QString expected_size_row = prompt + size_line_for_grid(contract, grid);
    const QString size_command = QString::fromLatin1(
        contract.size_command.data(),
        static_cast<qsizetype>(contract.size_command.size()));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto next_query = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_query) {
            if (!surface.paste_text(size_command + QLatin1Char('\n'))) {
                return false;
            }
            next_query = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(250);
        }

        app.processEvents(QEventLoop::AllEvents, 50);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (snapshot != nullptr && snapshot_contains_row(*snapshot, expected_size_row)) {
            return true;
        }

        QThread::msleep(10);
    }

    app.processEvents(QEventLoop::AllEvents, 50);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(surface);
    return snapshot != nullptr && snapshot_contains_row(*snapshot, expected_size_row);
}

bool is_live_surface_process_state(VNM_TerminalSurface::Process_state state)
{
    return
        state == VNM_TerminalSurface::Process_state::STARTING ||
        state == VNM_TerminalSurface::Process_state::RUNNING;
}

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(720, 360);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(620.0, 260.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        window.show();
    }
};

bool wait_for_process_exit(
    QGuiApplication&       app,
    bool&                  process_exited,
    int                    timeout_ms = 10000)
{
    return pump_until(app, [&] {
        return process_exited;
    }, timeout_ms);
}

bool cleanup_surface_process(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    bool&                  process_exited,
    std::string_view       label)
{
    pump_events(app);
    if (process_exited ||
        !is_live_surface_process_state(surface.process_state()))
    {
        return true;
    }

    const bool terminate_requested = surface.terminate_process();
    const bool exited              = wait_for_process_exit(app, process_exited, 5000);
    pump_events(app);

    if (!terminate_requested) {
        std::cerr << label << ": failed to request process termination during cleanup\n";
    }

    if (!exited) {
        std::cerr << label << ": process did not exit after cleanup termination\n";
    }

    return terminate_requested && exited;
}

void print_backend_errors(std::string_view label, const QStringList& backend_errors)
{
    for (const QString& backend_error : backend_errors) {
        std::cerr << label << " backend error: "
            << backend_error.toLocal8Bit().constData() << '\n';
    }
}

QString present_environment_probe_line(const QString& name, const QString& value)
{
    return QStringLiteral("%1=present:%2")
        .arg(name, QString::fromLatin1(value.toUtf8().toHex()));
}

QString missing_environment_probe_line(const QString& name)
{
    return QStringLiteral("%1=missing").arg(name);
}

std::vector<vnm_terminal::Terminal_environment_entry> explicit_environment_entries(
    const QProcessEnvironment& environment)
{
    std::vector<vnm_terminal::Terminal_environment_entry> entries;
    const QStringList names = environment.keys();
    entries.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& name : names) {
        entries.push_back({name, environment.value(name)});
    }
    return entries;
}

vnm_terminal::Terminal_process_start_result start_terminal_fixture(
    VNM_TerminalSurface& surface,
    QStringList argv,
    QString working_directory,
    std::vector<vnm_terminal::Terminal_environment_entry> base_environment,
    std::optional<std::vector<vnm_terminal::Terminal_environment_entry>>
        capability_environment = std::nullopt)
{
    return surface.start_terminal({
        std::move(argv),
        std::move(working_directory),
        std::move(base_environment),
        std::move(capability_environment),
    });
}

term::Terminal_launch_config environment_validation_config()
{
    term::Terminal_launch_config config;
    config.argv = {QStringLiteral("environment-validation-fixture")};
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

bool is_invalid_launch_config_result(const term::Terminal_backend_result& result)
{
    return
        result.code == term::Terminal_backend_result_code::REJECTED &&
        result.error.has_value() &&
        result.error->code == term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG;
}

// Several launch-config checks share one error code, so a configuration that
// would also fail an earlier check cannot be pinned by the code alone: the
// assertion would pass on the wrong rule. Name the rule where that matters.
bool is_launch_config_rejection_for(
    const term::Terminal_backend_result&  result,
    const QString&                        reason)
{
    return
        is_invalid_launch_config_result(result) &&
        result.error->message.contains(reason);
}

bool test_launch_environment_validation_contract()
{
    bool ok = true;
    term::Terminal_launch_config config = environment_validation_config();

    ok &= check(
        !term::is_backend_rejection(term::validate_launch_config(config)),
        "launch validation accepts a missing explicit TERM override");
    ok &= check(
        term::build_launch_environment(config, {}).value(QStringLiteral("TERM")) ==
            config.identity.term,
        "launch environment supplies the default TERM when no override is present");

    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("TERM"),
        QStringLiteral("screen-256color"),
    }};
    ok &= check(
        !term::is_backend_rejection(term::validate_launch_config(config)) &&
            term::build_launch_environment(config, {}).value(QStringLiteral("TERM")) ==
                QStringLiteral("screen-256color"),
        "launch environment accepts and applies a non-empty TERM override");

#if defined(_WIN32)
    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("tErM"),
        QStringLiteral("mixed-case-term"),
    }};
    ok &= check(
        !term::is_backend_rejection(term::validate_launch_config(config)) &&
            term::build_launch_environment(config, {}).value(QStringLiteral("TERM")) ==
                QStringLiteral("mixed-case-term"),
        "Windows launch identity applies a non-empty mixed-case TERM override");

    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("tErM"),
        QString(),
    }};
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "Windows launch identity rejects an empty mixed-case TERM override");

    config.environment_edits = {{
        term::Terminal_environment_operation::UNSET,
        QStringLiteral("term"),
        {},
    }};
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "Windows launch identity rejects a lowercase TERM unset");
#else
    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("tErM"),
        QString(),
    }};
    ok &= check(
        !term::is_backend_rejection(term::validate_launch_config(config)) &&
            term::build_launch_environment(config, {}).contains(QStringLiteral("tErM")) &&
            term::build_launch_environment(config, {}).value(QStringLiteral("TERM")) ==
                config.identity.term,
        "POSIX launch identity keeps mixed-case term separate from TERM");

    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("TERM"),
        QString(),
    }};
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "POSIX launch identity rejects an empty exact-case TERM override");

    config.environment_edits = {{
        term::Terminal_environment_operation::UNSET,
        QStringLiteral("TERM"),
        {},
    }};
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "POSIX launch identity rejects an exact-case TERM unset");
#endif

    QString nul_value = QStringLiteral("before");
    nul_value += QChar(u'\0');
    nul_value += QStringLiteral("after");
    config.environment_edits = {{
        term::Terminal_environment_operation::SET,
        QStringLiteral("VNM_NUL_VALUE_PROBE"),
        nul_value,
    }};
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "launch validation rejects an embedded NUL in every SET value");

    config.environment_edits.clear();
    config.identity.term = nul_value;
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "launch validation rejects an embedded NUL in the TERM identity");

    config.identity.term      = QStringLiteral("xterm-256color");
    config.identity.colorterm = nul_value;
    ok &= check(
        is_invalid_launch_config_result(term::validate_launch_config(config)),
        "launch validation rejects an embedded NUL in the COLORTERM identity");

    config = environment_validation_config();
    config.argv = {nul_value};
    ok &= check(
        is_launch_config_rejection_for(
            term::validate_launch_config(config),
            QStringLiteral("argv values must not contain NUL")),
        "launch validation rejects an embedded NUL in argv[0]");

    config = environment_validation_config();
    config.argv.push_back(nul_value);
    ok &= check(
        is_launch_config_rejection_for(
            term::validate_launch_config(config),
            QStringLiteral("argv values must not contain NUL")),
        "launch validation rejects an embedded NUL in a later argument");

    config = environment_validation_config();
    config.working_directory = nul_value;
    ok &= check(
        is_launch_config_rejection_for(
            term::validate_launch_config(config),
            QStringLiteral("working directory must not contain NUL")),
        "launch validation rejects an embedded NUL inside the working directory");

    // A trailing NUL is the quiet case: the truncated path is still a path the
    // platform will happily change to, so nothing downstream reports that the
    // directory the caller asked for was not the directory that was used.
    config = environment_validation_config();
    config.working_directory = QStringLiteral("valid-directory");
    config.working_directory += QChar(u'\0');
    ok &= check(
        is_launch_config_rejection_for(
            term::validate_launch_config(config),
            QStringLiteral("working directory must not contain NUL")),
        "launch validation rejects a trailing NUL in the working directory");

    return ok;
}

bool test_exact_environment_rejection_allows_retry(
    QGuiApplication&            app,
    const QString&              fixture_path,
    std::vector<vnm_terminal::Terminal_environment_entry>
                                  invalid_environment,
    std::string_view            label)
{
    QTemporaryDir marker_dir;
    if (!check(marker_dir.isValid(), "exact-environment rejection marker directory is valid")) {
        return false;
    }

    const QString marker_path = marker_dir.filePath(QStringLiteral("child-started.marker"));
    const QStringList argv = {
        fixture_path,
        QStringLiteral("--write-start-marker"),
        marker_path,
    };

    Surface_fixture fixture;
    pump_events(app);

    std::vector<VNM_TerminalSurface::Backend_error_code> backend_error_codes;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            backend_error_codes.push_back(code);
            backend_errors.push_back(message);
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const auto case_check = [label](bool condition, std::string_view suffix) {
        std::string message(label);
        message += suffix;
        return check(condition, message);
    };

    bool ok = true;
    const vnm_terminal::Terminal_process_start_result invalid_result =
        start_terminal_fixture(
            fixture.surface,
            argv,
            QFileInfo(fixture_path).absolutePath(),
            std::move(invalid_environment));
    const bool invalid_started = invalid_result.accepted;
    pump_events(app);
    ok &= case_check(!invalid_started, " rejects the invalid exact environment");
    ok &= case_check(
        backend_error_codes.size() == 1U &&
            backend_error_codes.front() ==
                VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG,
        " publishes typed INVALID_LAUNCH_CONFIG");
    ok &= case_check(!QFileInfo::exists(marker_path), " does not launch the child");
    ok &= case_check(!process_exited, " publishes no child-exit signal");
    ok &= case_check(
        fixture.surface.process_state() == VNM_TerminalSurface::Process_state::FAILED,
        " publishes failed process state");
    if (invalid_started) {
        (void)wait_for_process_exit(app, process_exited);
        print_backend_errors(label, backend_errors);
        return false;
    }

    const vnm_terminal::Terminal_process_start_result retry_result =
        start_terminal_fixture(
            fixture.surface,
            argv,
            QFileInfo(fixture_path).absolutePath(),
            {});
    const bool retry_started = retry_result.accepted;
    ok &= case_check(retry_started, " allows a same-surface valid retry");
    if (!retry_started) {
        print_backend_errors(label, backend_errors);
        return false;
    }

    ok &= case_check(
        wait_for_process_exit(app, process_exited),
        " retry child exits before timeout");
    pump_events(app);
    ok &= case_check(QFileInfo::exists(marker_path), " retry launches the real child");
    ok &= case_check(
        backend_error_codes.size() == 1U,
        " retry adds no backend error");
    ok &= case_check(
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
            process_exit_code == 0,
        " retry child exits cleanly");

    if (!ok) {
        print_backend_errors(label, backend_errors);
    }
    return ok;
}

bool test_exact_environment_rejections(QGuiApplication& app, const QString& fixture_path)
{
    std::vector<vnm_terminal::Terminal_environment_entry>
        invalid_pseudo_environment{{
            QStringLiteral("=VNM_INVALID_PSEUDO"),
            QStringLiteral("value"),
        }};

    QString nul_value = QStringLiteral("before");
    nul_value += QChar(u'\0');
    nul_value += QStringLiteral("after");
    std::vector<vnm_terminal::Terminal_environment_entry>
        nul_value_environment{{
            QStringLiteral("VNM_NUL_VALUE_PROBE"),
            nul_value,
        }};

    bool ok = true;
    ok &= test_exact_environment_rejection_allows_retry(
        app,
        fixture_path,
        std::move(invalid_pseudo_environment),
        "invalid pseudo-variable base environment");
    ok &= test_exact_environment_rejection_allows_retry(
        app,
        fixture_path,
        std::move(nul_value_environment),
        "NUL-value exact environment");
    return ok;
}

bool test_capability_environment_rejections(
    QGuiApplication& app,
    const QString&   fixture_path)
{
    struct Rejection_case
    {
        const char* label;
        std::vector<vnm_terminal::Terminal_environment_entry> base;
        std::vector<vnm_terminal::Terminal_environment_entry> contribution;
    };
    const std::vector<Rejection_case> cases{
        {
            "base collision",
            {{QStringLiteral("VNM_SHARED"), QStringLiteral("base")}},
            {{QStringLiteral("VNM_SHARED"), QStringLiteral("capability")}},
        },
        {
            "terminal-owned name",
            {},
            {{QStringLiteral("TERM"), QStringLiteral("capability")}},
        },
        {
            "lookup-sensitive name",
            {},
            {{QStringLiteral("PATH"), QStringLiteral("capability")}},
        },
        {
            "pseudo-variable edit",
            {},
            {{QStringLiteral("=C:"), QStringLiteral("capability")}},
        },
        {
            "equivalent duplicate",
            {},
            {
                {QStringLiteral("VNM_DUPLICATE"), QStringLiteral("first")},
#if defined(_WIN32)
                {QStringLiteral("vnm_duplicate"), QStringLiteral("second")},
#else
                {QStringLiteral("VNM_DUPLICATE"), QStringLiteral("second")},
#endif
            },
        },
    };

    bool ok = true;
    for (const Rejection_case& rejection_case : cases) {
        Surface_fixture fixture;
        pump_events(app);
        int backend_error_count = 0;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code, const QString&) {
                ++backend_error_count;
            });

        const vnm_terminal::Terminal_process_start_result result =
            start_terminal_fixture(
                fixture.surface,
                {fixture_path},
                QFileInfo(fixture_path).absolutePath(),
                rejection_case.base,
                rejection_case.contribution);
        ok &= check(
            !result.accepted &&
                !result.native_dispatch_occurred &&
                result.determinacy ==
                    vnm_terminal::Terminal_process_start_determinacy::DETERMINATE,
            std::string(rejection_case.label) +
                " contribution rejects before native dispatch");
        ok &= check(
            backend_error_count == 1,
            std::string(rejection_case.label) +
                " contribution publishes one typed error");
    }
    return ok;
}

bool test_exact_process_environment(QGuiApplication& app, const QString& fixture_path)
{
    const QString inherited_name = QStringLiteral("VNM_INHERITED_PROBE");
    const QString exact_name     = QStringLiteral("VNM_EXACT_PROBE");
    const QString empty_name     = QStringLiteral("VNM_EMPTY_PROBE");
    const QString unicode_name   = QStringLiteral("VNM_UNICODE_PROBE");
    const QString capability_name = QStringLiteral("VNM_CAPABILITY_PROBE");
    const QString unicode_value  = QString::fromUtf8(
        QByteArray::fromHex("4772c3bcc39f6520e69db1e4baac"));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("TERM"), QStringLiteral("caller-term"));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("caller-colorterm"));
    environment.insert(exact_name, QStringLiteral("snapshot-value"));
    environment.insert(empty_name, QString());
    environment.insert(unicode_name, unicode_value);

    QStringList probe_names = {
        inherited_name,
        exact_name,
        empty_name,
        unicode_name,
        capability_name,
        QStringLiteral("TERM"),
        QStringLiteral("COLORTERM"),
    };
    bool ok = true;

#if defined(_WIN32)
    const QString uppercase_case_name = QStringLiteral("VNM_CASE_PROBE");
    const QString lowercase_case_name = QStringLiteral("vnm_case_probe");
    const QString magic_name          = QStringLiteral("=C:");
    environment.insert(uppercase_case_name, QStringLiteral("first-value"));
    environment.insert(lowercase_case_name, QStringLiteral("replacement-value"));
    environment.insert(magic_name, QStringLiteral("magic-value"));

    int case_variant_count = 0;
    for (const QString& name : environment.keys()) {
        if (name.compare(uppercase_case_name, Qt::CaseInsensitive) == 0) {
            ++case_variant_count;
        }
    }

    ok &= check(case_variant_count == 1,
        "QProcessEnvironment collapses Windows case-colliding names");
    ok &= check(
        environment.value(uppercase_case_name) == QStringLiteral("replacement-value"),
        "QProcessEnvironment keeps the last value for a Windows case collision");
    if (!ok) {
        return false;
    }

    probe_names.push_back(uppercase_case_name);
    probe_names.push_back(lowercase_case_name);
    probe_names.push_back(magic_name);
#endif

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    QStringList argv = {
        fixture_path,
        QStringLiteral("--echo-environment"),
    };
    argv.append(probe_names);
    const vnm_terminal::Terminal_process_start_result start_result =
        start_terminal_fixture(
            fixture.surface,
            argv,
            QFileInfo(fixture_path).absolutePath(),
            explicit_environment_entries(environment),
            std::vector<vnm_terminal::Terminal_environment_entry>{{
                capability_name,
                QStringLiteral("capability-value"),
            }});
    const bool started = start_result.accepted;
    ok &= check(started, "explicit-environment structured surface launch starts fixture");
    if (!started) {
        print_backend_errors("exact-environment public surface", backend_errors);
        return false;
    }

    ok &= check(
        wait_for_process_exit(app, process_exited),
        "exact-environment fixture exits before timeout");
    pump_events(app);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr,
        "exact-environment fixture publishes a render snapshot");
    if (snapshot != nullptr) {
        ok &= check(
            snapshot_contains_text(*snapshot, missing_environment_probe_line(inherited_name)),
            "exact environment excludes a variable inherited by the host");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(exact_name, QStringLiteral("snapshot-value"))),
            "exact environment delivers an explicit snapshot value");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(empty_name, QString())),
            "exact environment preserves a present empty value");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(unicode_name, unicode_value)),
            "exact environment preserves a Unicode value");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    capability_name,
                    QStringLiteral("capability-value"))),
            "separate capability contribution reaches the child");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    QStringLiteral("TERM"),
                    QStringLiteral("xterm-256color"))) &&
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    QStringLiteral("COLORTERM"),
                    QStringLiteral("truecolor"))),
            "surface owns final TERM and COLORTERM values");

#if defined(_WIN32)
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    uppercase_case_name,
                    QStringLiteral("replacement-value"))),
            "exact environment delivers the resolved Windows case-collision value");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    lowercase_case_name,
                    QStringLiteral("replacement-value"))),
            "Windows child lookup remains case-insensitive after exact launch");
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(
                    magic_name,
                    QStringLiteral("magic-value"))),
            "exact environment preserves a Windows magic environment entry");
#endif
    }

    print_backend_errors("exact-environment public surface", backend_errors);
    ok &= check(
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        process_exit_code == 0,
        "exact-environment fixture exits cleanly");
    ok &= check(backend_error_count == 0,
        "exact-environment public surface launch has no backend errors");
    return ok;
}

bool test_caller_captured_process_environment(
    QGuiApplication& app,
    const QString&   fixture_path)
{
    const QString inherited_name  = QStringLiteral("VNM_INHERITED_PROBE");
    const QString inherited_value = QStringLiteral("host-value");

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const vnm_terminal::Terminal_process_start_result start_result =
        start_terminal_fixture(
            fixture.surface,
            {
                fixture_path,
                QStringLiteral("--echo-environment"),
                inherited_name,
            },
            QFileInfo(fixture_path).absolutePath(),
            explicit_environment_entries(
                QProcessEnvironment::systemEnvironment()));
    const bool started = start_result.accepted;
    bool ok = true;
    ok &= check(started, "caller-captured environment starts the fixture");
    if (!started) {
        print_backend_errors("default public surface environment", backend_errors);
        return false;
    }

    ok &= check(
        wait_for_process_exit(app, process_exited),
        "default-environment fixture exits before timeout");
    pump_events(app);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr,
        "default-environment fixture publishes a render snapshot");
    if (snapshot != nullptr) {
        ok &= check(
            snapshot_contains_text(
                *snapshot,
                present_environment_probe_line(inherited_name, inherited_value)),
            "caller-captured environment reaches the child explicitly");
    }

    print_backend_errors("default public surface environment", backend_errors);
    ok &= check(
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        process_exit_code == 0,
        "default-environment fixture exits cleanly");
    ok &= check(backend_error_count == 0,
        "default public surface environment launch has no backend errors");
    return ok;
}

bool test_final_path_and_executable_token_semantics(
    QGuiApplication& app,
    const QString&   fixture_path)
{
    QTemporaryDir directory;
    bool ok = true;
    ok &= check(directory.isValid(), "PATH lookup fixture directory is valid");
    const QString binary_directory = directory.filePath(QStringLiteral("bin"));
    const QString working_directory = directory.filePath(QStringLiteral("working"));
    ok &= check(
        QDir().mkpath(binary_directory) && QDir().mkpath(working_directory),
        "PATH lookup fixture directories are created");
#if defined(_WIN32)
    const QString bare_name = QStringLiteral("vnm-phase2b-path-fixture.exe");
    const QChar path_separator = QLatin1Char(';');
#else
    const QString bare_name = QStringLiteral("vnm-phase2b-path-fixture");
    const QChar path_separator = QLatin1Char(':');
#endif
    const QString copied_fixture = QDir(binary_directory).filePath(bare_name);
    ok &= check(
        QFile::copy(fixture_path, copied_fixture),
        "PATH lookup fixture executable is copied");
    if (!ok) {
        return false;
    }
    QFile::setPermissions(copied_fixture, QFile::permissions(fixture_path));

    const auto run_fixture = [&] (
        QString executable,
        std::vector<vnm_terminal::Terminal_environment_entry> base_environment,
        bool* out_exited) {
        auto fixture = std::make_unique<Surface_fixture>();
        pump_events(app);
        QObject::connect(
            &fixture->surface,
            &VNM_TerminalSurface::process_exited,
            &fixture->surface,
            [out_exited](VNM_TerminalSurface::Exit_reason, int) {
                *out_exited = true;
            });
        const vnm_terminal::Terminal_process_start_result result =
            start_terminal_fixture(
                fixture->surface,
                {std::move(executable)},
                working_directory,
                std::move(base_environment));
        return std::pair{std::move(fixture), result};
    };

    const QString host_path =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
    bool explicit_exited = false;
    auto [explicit_fixture, explicit_result] = run_fixture(
        bare_name,
        {{
            QStringLiteral("PATH"),
            binary_directory + path_separator + host_path,
        }},
        &explicit_exited);
    ok &= check(
        explicit_result.accepted &&
            explicit_result.native_dispatch_occurred &&
            explicit_result.determinacy ==
                vnm_terminal::Terminal_process_start_determinacy::DETERMINATE,
        "bare executable resolves from the explicit final PATH");
    ok &= check(
        wait_for_process_exit(app, explicit_exited),
        "explicit-PATH fixture exits");

    bool absent_exited = false;
    auto [absent_fixture, absent_result] = run_fixture(
        bare_name,
        {},
        &absent_exited);
    ok &= check(
        !absent_result.accepted &&
            !absent_result.native_dispatch_occurred &&
            absent_result.determinacy ==
                vnm_terminal::Terminal_process_start_determinacy::DETERMINATE &&
            !absent_exited,
        "absent PATH does not borrow the host PATH");

    bool empty_exited = false;
    auto [empty_fixture, empty_result] = run_fixture(
        bare_name,
        {{QStringLiteral("PATH"), QString()}},
        &empty_exited);
    ok &= check(
        !empty_result.accepted &&
            !empty_result.native_dispatch_occurred &&
            empty_result.determinacy ==
                vnm_terminal::Terminal_process_start_determinacy::DETERMINATE &&
            !empty_exited,
        "present-empty PATH remains distinct and does not borrow the host PATH");

    bool absolute_exited = false;
    auto [absolute_fixture, absolute_result] = run_fixture(
        copied_fixture,
        explicit_environment_entries(QProcessEnvironment::systemEnvironment()),
        &absolute_exited);
    ok &= check(
        absolute_result.accepted && absolute_result.native_dispatch_occurred,
        "absolute executable token bypasses PATH lookup");
    ok &= check(
        wait_for_process_exit(app, absolute_exited),
        "absolute-token fixture exits");
    return ok;
}

bool test_invalid_second_start_preserves_live_session(
    QGuiApplication& app,
    const QString&   fixture_path)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();

    Surface_fixture fixture;
    pump_events(app);

    std::vector<VNM_TerminalSurface::Backend_error_code> backend_error_codes;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            backend_error_codes.push_back(code);
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    bool ok = true;
    const vnm_terminal::Terminal_process_start_result initial_start =
        start_terminal_fixture(
            fixture.surface,
            {
                fixture_path,
                QStringLiteral("--shell-like-smoke"),
            },
            QFileInfo(fixture_path).absolutePath(),
            explicit_environment_entries(
                QProcessEnvironment::systemEnvironment()));
    ok &= check(
        initial_start.accepted && initial_start.native_dispatch_occurred,
        "live-session admission fixture starts");
    if (!ok) {
        return false;
    }

    QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    while (!prompt.isEmpty() && prompt.back() == QChar(u' ')) {
        prompt.chop(1);
    }
    if (!check(
            wait_for_snapshot_row(app, fixture.surface, prompt),
            "live-session admission fixture reaches its prompt"))
    {
        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "live-session admission fixture");
        return false;
    }

    bool failed_state_published = false;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_state_changed,
        &fixture.surface,
        [&]() {
            failed_state_published = failed_state_published ||
                fixture.surface.process_state() ==
                    VNM_TerminalSurface::Process_state::FAILED;
        });
    const vnm_terminal::Terminal_process_start_result rejected_start =
        start_terminal_fixture(
            fixture.surface,
            {},
            QFileInfo(fixture_path).absolutePath(),
            {});
    pump_events(app);

    ok &= check(
        !rejected_start.accepted &&
            !rejected_start.native_dispatch_occurred &&
            rejected_start.determinacy ==
                vnm_terminal::Terminal_process_start_determinacy::DETERMINATE,
        "invalid second start is rejected before native dispatch");
    ok &= check(
        backend_error_codes.size() == 1U &&
            backend_error_codes.front() ==
                VNM_TerminalSurface::Backend_error_code::START_FAILED,
        "live-session admission rejection publishes START_FAILED");
    ok &= check(
        !failed_state_published &&
            fixture.surface.process_state() ==
                VNM_TerminalSurface::Process_state::RUNNING,
        "invalid second start does not publish FAILED or replace live state");
    ok &= check(
        !process_exited,
        "invalid second start does not terminate the live backend");
    ok &= check(
        fixture.surface.paste_text(QStringLiteral("exit\n")),
        "original backend still accepts input after invalid second start");
    ok &= check(
        wait_for_process_exit(app, process_exited),
        "original backend exits normally after invalid second start");
    pump_events(app);
    ok &= check(
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
            process_exit_code == 0,
        "original backend retains its normal exit path");
    return ok;
}

bool test_shell_like_surface_native_smoke(QGuiApplication& app, const QString& fixture_path)
{
    bool ok = true;
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    ok &= check(contract.stream_count > 0 &&
        contract.stream_max_count > 0 &&
        contract.stream_count <= contract.stream_max_count,
        "shell-like stream count is positive and bounded");
    if (!ok) {
        return false;
    }

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const bool started = start_terminal_fixture(
        fixture.surface,
        {
            fixture_path,
            QStringLiteral("--shell-like-smoke"),
        },
        QFileInfo(fixture_path).absolutePath(),
        explicit_environment_entries(
            QProcessEnvironment::systemEnvironment())).accepted;
    ok &= check(started, "shell-like native surface smoke starts fixture");
    if (!started) {
        return false;
    }

    auto require = [&](bool condition, const std::string& message) -> bool {
        if (check(condition, message)) {
            return true;
        }

        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like native surface smoke");
        return false;
    };

    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    QString visible_prompt = prompt;
    while (!visible_prompt.isEmpty() && visible_prompt.back() == QChar(u' ')) {
        visible_prompt.chop(1);
    }
    if (!require(
            visible_prompt.size() >= 2, "shell-like visible prompt contract is non-empty after trimming"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_row(app, fixture.surface, visible_prompt), "shell-like prompt reaches surface snapshot"))
    {
        return false;
    }

    const QString echo_text = QString::fromLatin1(
        contract.echo_text.data(),
        static_cast<qsizetype>(contract.echo_text.size()));
    if (!require(
            fixture.surface.paste_text(
                QStringLiteral("%1 %2\n")
                    .arg(QString::fromLatin1(
                        contract.echo_command.data(),
                        static_cast<qsizetype>(contract.echo_command.size())))
                    .arg(echo_text)),
            "shell-like echo command writes through public paste_text"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_text(app, fixture.surface, echo_text), "shell-like echo output reaches surface snapshot"))
    {
        return false;
    }

    surface_grid_size_t initial_grid;
    if (!require(
            wait_for_surface_initial_sync(app, fixture.surface, initial_grid),
            "shell-like surface publishes synchronized initial native grid"))
    {
        return false;
    }

    if (!require(
            wait_for_size_report(app, fixture.surface, contract, initial_grid),
            "shell-like size output reports exact initial surface rows and columns"))
    {
        return false;
    }

    fixture.window.resize(900, 500);
    fixture.surface.setSize(QSizeF(820.0, 420.0));

    surface_grid_size_t resized_grid;
    if (!require(
            wait_for_surface_resize(app, fixture.surface, initial_grid, resized_grid),
            "shell-like surface resize changes synchronized native grid"))
    {
        return false;
    }

    if (!require(
            wait_for_size_report(app, fixture.surface, contract, resized_grid),
            "shell-like size output reports exact resized surface rows and columns"))
    {
        return false;
    }

    if (!require(
            contract.stream_count > resized_grid.rows, "shell-like stream count exceeds resized visible rows"))
    {
        return false;
    }

    if (!require(
            fixture.surface.paste_text( QStringLiteral("stream %1\n").arg(contract.stream_count)),
            "shell-like stream command writes through public paste_text"))
    {
        return false;
    }

    const QString final_stream_line = stream_line_for_count(contract.stream_count);
    if (!require(
            wait_for_snapshot_row(app, fixture.surface, final_stream_line),
            "shell-like derived stream final line reaches surface snapshot"))
    {
        return false;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> stream_snapshot =
        current_snapshot(fixture.surface);
    if (!require(
            stream_snapshot != nullptr, "shell-like stream publishes a render snapshot"))
    {
        return false;
    }

    const int minimum_scrollback_rows = std::max(1, contract.stream_count - resized_grid.rows);
    ok &= check(stream_snapshot->grid_size.rows == resized_grid.rows &&
        stream_snapshot->grid_size.columns == resized_grid.columns,
        "shell-like stream snapshot keeps resized surface grid");
    ok &= check(stream_snapshot->viewport.scrollback_rows >= minimum_scrollback_rows,
        "shell-like stream produces scrollback for rows beyond the resized viewport");
    ok &= check(stream_snapshot->viewport.scrollback_rows <=
        contract.stream_count + resized_grid.rows + 8,
        "shell-like stream scrollback stays within expected smoke-test bounds");
    ok &= check(snapshot_contains_row(*stream_snapshot, final_stream_line),
        "shell-like stream final row remains exact in current snapshot");
    if (!ok) {
        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like native surface smoke");
        return false;
    }

    if (!require(
            fixture.surface.paste_text(QStringLiteral("exit\n")),
            "shell-like exit command writes through public paste_text"))
    {
        return false;
    }

    if (!require(
            wait_for_process_exit(app, process_exited), "shell-like fixture exits before timeout"))
    {
        return false;
    }
    pump_events(app);

    print_backend_errors("shell-like native surface", backend_errors);

    ok &= check(exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        process_exit_code == 0,
        "shell-like fixture exits cleanly");
    ok &= check(backend_error_count == 0,
        "shell-like native surface smoke has no backend errors");
    ok &= check(fixture.surface.process_state() ==
        VNM_TerminalSurface::Process_state::EXITED,
        "shell-like surface publishes exited process state");

    return ok;
}

bool test_shell_like_surface_interrupt_smoke(QGuiApplication& app, const QString& fixture_path)
{
    bool ok = true;
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const bool started = start_terminal_fixture(
        fixture.surface,
        {
            fixture_path,
            QStringLiteral("--shell-like-smoke"),
        },
        QFileInfo(fixture_path).absolutePath(),
        explicit_environment_entries(
            QProcessEnvironment::systemEnvironment())).accepted;
    ok &= check(started, "shell-like interrupt surface smoke starts fixture");
    if (!started) {
        return false;
    }

    auto require = [&](bool condition, const std::string& message) -> bool {
        if (check(condition, message)) {
            return true;
        }

        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like interrupt surface smoke");
        return false;
    };

    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    QString visible_prompt = prompt;
    while (!visible_prompt.isEmpty() && visible_prompt.back() == QChar(u' ')) {
        visible_prompt.chop(1);
    }
    if (!require(
            visible_prompt.size() >= 2, "shell-like interrupt visible prompt contract is non-empty after trimming"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_row(app, fixture.surface, visible_prompt),
            "shell-like interrupt prompt reaches surface snapshot"))
    {
        return false;
    }

    if (!require(
            fixture.surface.paste_text(
                QStringLiteral("%1\n")
                    .arg(QString::fromLatin1(
                        contract.wait_command.data(),
                        static_cast<qsizetype>(contract.wait_command.size())))),
            "shell-like wait command writes through public paste_text"))
    {
        return false;
    }

    const QString wait_output = QString::fromLatin1(
        contract.wait_output.data(),
        static_cast<qsizetype>(contract.wait_output.size()));
    if (!require(
            wait_for_snapshot_row(app, fixture.surface, prompt + wait_output),
            "shell-like wait command reaches blocking fixture path"))
    {
        return false;
    }

    if (!require(
            fixture.surface.interrupt_process(), "shell-like wait path accepts public interrupt"))
    {
        return false;
    }

    if (!require(
            wait_for_process_exit(app, process_exited), "shell-like interrupted fixture exits before timeout"))
    {
        return false;
    }
    pump_events(app);

    print_backend_errors("shell-like interrupt surface", backend_errors);

    ok &= check(exit_reason == VNM_TerminalSurface::Exit_reason::INTERRUPTED &&
        process_exit_code == 130,
        "shell-like wait path exits through interrupt");
    ok &= check(backend_error_count == 0,
        "shell-like interrupt smoke has no backend errors");
    ok &= check(fixture.surface.process_state() ==
        VNM_TerminalSurface::Process_state::EXITED,
        "shell-like interrupted surface publishes exited process state");

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    if (argc != 2 || argument_equals(argv[1], "--help")) {
        std::cerr << "usage: compat_smoke_tests <fixture-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);
    bool ok = true;
    ok &= test_launch_environment_validation_contract();
    ok &= test_exact_environment_rejections(app, fixture_path);
    ok &= test_capability_environment_rejections(app, fixture_path);
    ok &= test_exact_process_environment(app, fixture_path);
    ok &= test_caller_captured_process_environment(app, fixture_path);
    ok &= test_final_path_and_executable_token_semantics(app, fixture_path);
    ok &= test_invalid_second_start_preserves_live_session(app, fixture_path);
    ok &= test_shell_like_surface_native_smoke(app, fixture_path);
    ok &= test_shell_like_surface_interrupt_smoke(app, fixture_path);
    return ok ? 0 : 1;
}
