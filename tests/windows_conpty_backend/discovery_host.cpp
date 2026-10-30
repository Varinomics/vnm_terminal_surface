#include "discovery_host.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"

#include <QByteArray>
#include <QDir>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace term = vnm_terminal::internal;

namespace {

struct Host_observation
{
    std::atomic<DWORD>         root_pid{0};
    std::atomic<std::uint64_t> output_bytes{0};
    std::mutex                mutex;
    bool                      root_exited = false;
    term::Terminal_backend_exit exit;
    QJsonArray                backend_errors;
};

struct Host_requests
{
    std::uint64_t cpu_count      = 0;
    std::uint64_t snapshot_count = 0;
    std::optional<vnm_terminal::process_cpu_time_t> cpu;
    std::optional<vnm_terminal::Process_snapshot>   snapshot;
};

void report_native_error(const char* operation, DWORD error)
{
    std::cerr << "discovery host: " << operation << " failed, Win32 error " << error << '\n';
}

void record_backend_error(Host_observation& observation, const term::Terminal_backend_error& error)
{
    const std::lock_guard lock(observation.mutex);
    observation.backend_errors.append(QJsonObject{
        {QStringLiteral("code"), static_cast<int>(error.code)},
        {QStringLiteral("message"), error.message},
    });
}

bool write_sample(
    const QString&                      path,
    const term::Windows_conpty_backend&  backend,
    Host_observation&                   observation,
    const Host_requests&                requests,
    bool                                started)
{
    // In particular, neither CPU nor process snapshot queries belong here.
    const auto state = backend.discovery_state_for_testing();
    QJsonObject sample{
        {QStringLiteral("host_pid"), static_cast<qint64>(GetCurrentProcessId())},
        {QStringLiteral("root_pid"), static_cast<qint64>(observation.root_pid.load())},
        {QStringLiteral("started"), started},
        {QStringLiteral("system_snapshot_count"), static_cast<qint64>(state.system_snapshot_count)},
        {QStringLiteral("autonomous_discovery_wake_count"),
            static_cast<qint64>(state.autonomous_discovery_wake_count)},
        {QStringLiteral("discovery_pass_count"), static_cast<qint64>(state.discovery_pass_count)},
        {QStringLiteral("retained_process_count"), static_cast<qint64>(state.retained_process_count)},
        {QStringLiteral("output_bytes"), static_cast<qint64>(observation.output_bytes.load())},
        {QStringLiteral("cpu_request_count"), static_cast<qint64>(requests.cpu_count)},
        {QStringLiteral("snapshot_request_count"), static_cast<qint64>(requests.snapshot_count)},
        {QStringLiteral("cpu_available"), requests.cpu.has_value()},
        {QStringLiteral("snapshot_available"), requests.snapshot.has_value()},
    };
    {
        const std::lock_guard lock(observation.mutex);
        sample.insert(QStringLiteral("root_exited"), observation.root_exited);
        sample.insert(QStringLiteral("root_exit_code"), observation.exit.exit_code);
        sample.insert(QStringLiteral("root_exit_reason"), static_cast<int>(observation.exit.reason));
        sample.insert(QStringLiteral("backend_errors"), observation.backend_errors);
    }
    if (requests.cpu) {
        sample.insert(QStringLiteral("cpu_time_usec"), static_cast<qint64>(requests.cpu->cpu_time_usec));
        sample.insert(QStringLiteral("cpu_scope"), static_cast<int>(requests.cpu->scope));
    }
    if (requests.snapshot) {
        QJsonArray processes;
        for (const auto& process : requests.snapshot->processes) {
            processes.append(QJsonObject{
                {QStringLiteral("process_id"), static_cast<qint64>(process.process_id)},
                {QStringLiteral("birth_token"), static_cast<qint64>(process.birth_token)},
                {QStringLiteral("name"), QString::fromStdString(process.name)},
            });
        }
        sample.insert(QStringLiteral("snapshot_processes"), processes);
    }

    QSaveFile file(path);
    const auto bytes = QJsonDocument(sample).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        std::cerr << "discovery host: sample write failed: " << file.errorString().toStdString() << '\n';
        return false;
    }
    return true;
}

int host(const QDir& directory, QStringList root_argv)
{
    if (!directory.mkpath(QStringLiteral("."))) {
        std::cerr << "discovery host: cannot create control directory\n";
        return 1;
    }

    // Auto-reset requests coalesce. The controller waits for each sample file
    // replacement before issuing another sample. Stop has wait priority.
    constexpr std::array<const wchar_t*, 4> suffixes{L"stop", L"cpu", L"snapshot", L"sample"};
    std::array<HANDLE, suffixes.size()> events{};
    const auto close_events = qScopeGuard([&] {
        for (const auto event : events) {
            if (event) {
                CloseHandle(event);
            }
        }
    });
    for (std::size_t index = 0; index != events.size(); ++index) {
        const auto name = QStringLiteral("Local\\vnm-discovery-%1-%2")
            .arg(GetCurrentProcessId()).arg(QString::fromWCharArray(suffixes[index])).toStdWString();
        events[index] = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
        const DWORD error = GetLastError();
        if (!events[index] || error == ERROR_ALREADY_EXISTS) {
            report_native_error("CreateEventW", error);
            return 1;
        }
    }

    auto observation = std::make_shared<Host_observation>();
    auto backend     = std::make_unique<term::Windows_conpty_backend>();
    if (!backend->set_cpu_observer_for_testing(
            [observation](const char* event, void*, void* root, const auto&) {
                if (std::string_view(event) == "assignment") {
                    observation->root_pid.store(GetProcessId(static_cast<HANDLE>(root)));
                }
            }))
    {
        std::cerr << "discovery host: cannot install root observer\n";
        return 1;
    }

    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [observation](QByteArray bytes) {
        observation->output_bytes.fetch_add(static_cast<std::uint64_t>(bytes.size()), std::memory_order_relaxed);
    };
    callbacks.process_exited = [observation](term::Terminal_backend_exit exit) {
        const std::lock_guard lock(observation->mutex);
        observation->root_exited = true;
        observation->exit        = exit;
    };
    callbacks.error_reported = [observation](const term::Terminal_backend_error& error) {
        record_backend_error(*observation, error);
    };

    if (root_argv.isEmpty()) {
        root_argv = {QStringLiteral("cmd.exe"), QStringLiteral("/d"),
            QStringLiteral("/q"), QStringLiteral("/k")};
    }
    term::Terminal_launch_config config;
    config.argv               = std::move(root_argv);
    config.initial_grid_size  = {24, 80};
    config.identity.term      = QStringLiteral("xterm-256color");
    config.identity.colorterm = QStringLiteral("truecolor");
    const auto result  = backend->start(config, std::move(callbacks));
    const bool started = result.code == term::Terminal_backend_result_code::ACCEPTED;
    if (result.error) {
        record_backend_error(*observation, *result.error);
    }

    Host_requests requests;
    const auto sample_path = directory.filePath(QStringLiteral("%1.json").arg(GetCurrentProcessId()));
    bool ok = write_sample(sample_path, *backend, *observation, requests, started) && started;
    while (ok) {
        // Root exit deliberately does not stop the host: the harness measures
        // retained-state cost after exit using the same process and handles.
        const DWORD event = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
        if (event == WAIT_OBJECT_0) {
            break;
        }
        if (event == WAIT_OBJECT_0 + 3) {
            ok = write_sample(sample_path, *backend, *observation, requests, started);
        }
        else
        if (event == WAIT_OBJECT_0 + 1) {
            ++requests.cpu_count;
            requests.cpu = backend->process_cpu_time();
        }
        else
        if (event == WAIT_OBJECT_0 + 2) {
            ++requests.snapshot_count;
            requests.snapshot = backend->process_snapshot();
        }
        else {
            report_native_error("WaitForMultipleObjects", GetLastError());
            ok = false;
        }
    }

    // Facade destruction revokes callbacks and delegates native cleanup to the
    // reserved owner. The only timed wait in this host is its bounded teardown.
    backend.reset();
    if (!term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + std::chrono::seconds(20)))
    {
        std::cerr << "discovery host: native cleanup did not settle within 20 seconds\n";
        ok = false;
    }
    {
        const std::lock_guard lock(observation->mutex);
        if (!observation->backend_errors.isEmpty()) {
            std::cerr << "discovery host: backend errors: "
                      << QJsonDocument(observation->backend_errors).toJson(QJsonDocument::Compact).constData()
                      << '\n';
            ok = false;
        }
    }
    return ok ? 0 : 1;
}

} // namespace

std::optional<int> run_discovery_host(int argc, char** argv)
{
    if (argc < 2 || std::string_view(argv[1]) != "--discovery-host") {
        return std::nullopt;
    }
    int argument_count  = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) {
        report_native_error("CommandLineToArgvW", GetLastError());
        return 1;
    }
    const auto release_arguments = qScopeGuard([&] { LocalFree(arguments); });
    if (argument_count < 3) {
        std::cerr << "usage: --discovery-host <control-directory> [root argv...]\n";
        return 1;
    }
    QStringList root_argv;
    for (int index = 3; index < argument_count; ++index) {
        root_argv.append(QString::fromWCharArray(arguments[index]));
    }
    return host(QDir(QString::fromWCharArray(arguments[2])), std::move(root_argv));
}
