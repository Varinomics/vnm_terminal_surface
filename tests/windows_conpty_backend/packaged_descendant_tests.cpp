#include "packaged_descendant_tests.h"
#include "store_powershell_fixture.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"
#include "../helpers/test_check.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;

namespace {

constexpr auto k_timeout = std::chrono::seconds(20);

class Process_handle
{
public:
    explicit Process_handle(HANDLE handle = nullptr)
    :
        m_handle(handle)
    {}

    ~Process_handle()
    {
        if (m_handle) {
            CloseHandle(m_handle);
        }
    }

    Process_handle(const Process_handle&) = delete;
    Process_handle& operator=(const Process_handle&) = delete;
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

struct Membership_observation
{
    std::mutex mutex;
    std::shared_ptr<Process_handle> child;
    std::atomic_bool root_admitted{false};
    std::atomic_bool child_query_succeeded{false};
    std::atomic_bool child_in_root_job{true};
};

bool wait_until(const std::function<bool()>& ready)
{
    const auto deadline = std::chrono::steady_clock::now() + k_timeout;
    do {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool marker(const QDir& directory, const char* name)
{
    QFile file(directory.filePath(QString::fromLatin1(name)));
    return file.open(QIODevice::WriteOnly);
}

bool spawn(const QString& executable, const QString& arguments, DWORD flags)
{
    const auto application = QDir::toNativeSeparators(executable).toStdWString();
    auto command = (QStringLiteral("\"%1\" ").arg(executable) + arguments).toStdWString();
    STARTUPINFOW startup{};
    startup.cb          = sizeof(startup);
    startup.dwFlags     = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
        flags | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process))
    {
        std::cerr << "packaged descendant fixture CreateProcessW failed: " << GetLastError() << '\n';
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int run_parent(const QString& packaged_shell, const QDir& directory)
{
    QString quoted_directory = directory.absolutePath();
    quoted_directory.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString script = QStringLiteral(
        "$d='%1'; [IO.File]::WriteAllText((Join-Path $d 'packaged.pid'), [string]$PID); "
        "$w=[Diagnostics.Stopwatch]::StartNew(); while($w.ElapsedMilliseconds -lt 250) { $x=17*19 }; "
        "[IO.File]::WriteAllText((Join-Path $d 'ready'), 'ready'); "
        "while($w.ElapsedMilliseconds -lt 60000 -and !(Test-Path (Join-Path $d 'child-exit'))) "
        "{ Start-Sleep -Milliseconds 25 }")
            .arg(quoted_directory);
    const QString arguments = QStringLiteral("-NoLogo -NoProfile -NonInteractive -Command \"%1\"").arg(script);
    if (!spawn(packaged_shell, arguments, CREATE_NEW_CONSOLE)) {
        return 1;
    }
    // This unpackaged intermediary stays alive, so S2 can prove the packaged
    // grandchild's creator even when its own console is detached from ConPTY.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline &&
        !QFileInfo::exists(directory.filePath(QStringLiteral("root-exit"))))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

int run_root(const QString& self, const QString& packaged_shell, const QDir& directory)
{
    if (!spawn(self, QStringLiteral("--packaged-descendant-parent \"%1\" \"%2\"")
        .arg(packaged_shell, directory.absolutePath()), 0))
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline &&
        !QFileInfo::exists(directory.filePath(QStringLiteral("root-exit"))))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

std::optional<std::uint64_t> process_cpu(HANDLE process)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return std::nullopt;
    }
    const auto ticks = [](FILETIME time) {
        return ((std::uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 10U;
}

bool test_lifecycle(const QString& self, const QString& shell, const QString& package, std::string_view mode)
{
    // Every fixture relaunch must preserve paths outside the active ANSI code page.
    QTemporaryDir temporary(QDir::temp().filePath(
        QStringLiteral("vnm-packaged-\u03a9\u6f22\u5b57\U0001f680-XXXXXX")));
    if (!check(temporary.isValid(), "packaged descendant directory is available")) {
        return false;
    }
    const QDir directory(temporary.path());
    const auto membership = std::make_shared<Membership_observation>();
    std::atomic_bool exited{false};
    auto backend = std::make_unique<term::Windows_conpty_backend>();
    backend->set_cpu_observer_for_testing([membership](const char* event, void* job, void* root, const auto&) {
        if (std::string_view(event) == "assignment") {
            BOOL member = FALSE;
            membership->root_admitted.store(IsProcessInJob(root, job, &member) && member);
        }
        if (std::string_view(event) == "sample") {
            const std::lock_guard lock(membership->mutex);
            if (membership->child) {
                BOOL member = TRUE;
                membership->child_query_succeeded.store(IsProcessInJob(membership->child->get(), job, &member));
                membership->child_in_root_job.store(member != FALSE);
            }
        }
    });
    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [](QByteArray) {};
    callbacks.process_exited = [&](auto) { exited.store(true, std::memory_order_release); };
    callbacks.error_reported = [](const auto& error) { std::cerr << error.message.toStdString() << '\n'; };
    term::Terminal_launch_config config;
    config.argv = {self, QStringLiteral("--packaged-descendant-root"), shell, temporary.path()};
    config.initial_grid_size = {24, 80};
    const auto started = backend->start(config, std::move(callbacks));
    if (!check(!term::is_backend_rejection(started) && membership->root_admitted.load(),
        "packaged grandchild fixture starts in the terminal Job"))
    {
        return false;
    }
    if (!check(wait_until([&] { return QFileInfo::exists(directory.filePath(QStringLiteral("ready"))); }),
        "detached packaged grandchild burns CPU and reports readiness"))
    {
        marker(directory, "child-exit");
        return false;
    }
    QFile pid_file(directory.filePath(QStringLiteral("packaged.pid")));
    if (!check(pid_file.open(QIODevice::ReadOnly), "packaged PID is readable")) {
        marker(directory, "child-exit");
        return false;
    }
    const DWORD pid = pid_file.readAll().trimmed().toUInt();
    const auto child = std::make_shared<Process_handle>(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid));
    if (!check(child->get() != nullptr, "exact packaged grandchild handle is retained")) {
        marker(directory, "child-exit");
        return false;
    }
    // Cleanup failed assertions too: an undiscovered packaged process is
    // precisely what this test is intended to expose.
    const auto cleanup_child = qScopeGuard([&] {
        if (WaitForSingleObject(child->get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(child->get(), 1);
            WaitForSingleObject(child->get(), 5000);
        }
    });
    UINT32 package_size = 0;
    const LONG package_result = GetPackageFullName(child->get(), &package_size, nullptr);
    std::vector<wchar_t> package_name(package_size);
    bool ok = check(package_result == ERROR_INSUFFICIENT_BUFFER &&
        GetPackageFullName(child->get(), &package_size, package_name.data()) == ERROR_SUCCESS &&
        QString::fromWCharArray(package_name.data()) == package,
        "the fixture is the registered MSIX process instance");
    {
        const std::lock_guard lock(membership->mutex);
        membership->child = child;
    }
    ok &= check(wait_until([&] {
        const auto snapshot = backend->process_snapshot();
        if (!snapshot) {
            return false;
        }
        for (const auto& entry : snapshot->processes) {
            if (entry.process_id == pid && entry.birth_token != 0) {
                return true;
            }
        }
        return false;
    }), "the scanner adopts the detached packaged grandchild into its observed family");
    const auto cpu = process_cpu(child->get());
    const auto sample = backend->process_cpu_time();
    ok &= check(backend->write_state_for_testing().adoption_job_count > 0,
        "live escaped descendants have retained adoption Jobs");
    ok &= check(membership->child_query_succeeded.load() && !membership->child_in_root_job.load(),
        "the packaged grandchild escaped the root Job");
    ok &= check(cpu && sample && sample->cpu_time_usec >= *cpu &&
        sample->scope == vnm_terminal::Process_cpu_scope::OBSERVED_FAMILY,
        "family accounting includes all OS-reported packaged process CPU with observed scope");

    if (mode == "exited-child") {
        ok &= check(marker(directory, "child-exit") &&
            WaitForSingleObject(child->get(), 20000) == WAIT_OBJECT_0, "packaged child exits before root");
        ok &= check(wait_until([&] { return backend->write_state_for_testing().adoption_job_count == 0; }),
            "empty adoption Jobs are released while the root still runs");
        const auto retired = backend->process_cpu_time();
        ok &= check(retired && sample && retired->cpu_time_usec >= sample->cpu_time_usec,
            "retiring an empty adoption Job preserves its cumulative CPU");
    }
    if (mode == "destroy") {
        backend.reset();
    }
    else
    if (mode == "terminate") {
        ok &= check(!term::is_backend_rejection(backend->terminate()), "termination request is accepted");
    }
    else {
        ok &= check(marker(directory, "root-exit"), "root is allowed to exit naturally");
    }
    ok &= check(WaitForSingleObject(child->get(), 20000) == WAIT_OBJECT_0,
        "terminal retirement ends the detached packaged grandchild");
    if (backend) {
        ok &= check(wait_until([&] { return exited.load(std::memory_order_acquire); }),
            "root exit is reported after family settlement");
        const auto final_cpu = backend->process_cpu_time();
        ok &= check(final_cpu && sample && final_cpu->cpu_time_usec >= sample->cpu_time_usec,
            "family CPU remains available after root and descendant exit");
        backend.reset();
    }
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
        std::chrono::steady_clock::now() + k_timeout), "packaged descendant native cleanup settles");
    std::cerr << "packaged descendant lifecycle " << mode << (ok ? " passed\n" : " failed\n");
    return ok;
}

}

std::optional<int> run_packaged_descendant_tests(int argc, char** argv)
{
    if (argc < 2) {
        return std::nullopt;
    }
    const std::string_view mode(argv[1]);
    if (mode != "--packaged-descendant-parent" &&
        mode != "--packaged-descendant-root"   &&
        mode != "--packaged-descendants")
    {
        return std::nullopt;
    }

    // This dispatcher runs before a QCoreApplication exists. Narrow CRT argv
    // has already lost characters that the native command line still retains.
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!check(arguments != nullptr, "packaged fixture native arguments are available")) {
        return 1;
    }
    const auto release_arguments = qScopeGuard([&] { LocalFree(arguments); });
    if (argument_count == 4 && mode == "--packaged-descendant-parent") {
        return run_parent(QString::fromWCharArray(arguments[2]), QDir(QString::fromWCharArray(arguments[3])));
    }
    if (argument_count == 4 && mode == "--packaged-descendant-root") {
        return run_root(QString::fromWCharArray(arguments[0]), QString::fromWCharArray(arguments[2]),
            QDir(QString::fromWCharArray(arguments[3])));
    }
    if (argument_count != 2 || mode != "--packaged-descendants") {
        return std::nullopt;
    }
    QString package;
    QString executable;
    const int discovery = vnm_terminal::test_helpers::find_store_powershell(package, executable);
    if (discovery != 0) {
        return discovery;
    }
    bool ok = true;
    for (const auto mode : {"natural", "terminate", "destroy", "exited-child"}) {
        ok &= test_lifecycle(QString::fromWCharArray(arguments[0]), executable, package, mode);
    }
    return ok ? 0 : 1;
}
