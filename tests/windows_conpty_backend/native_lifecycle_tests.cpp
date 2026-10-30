#include "native_lifecycle_tests.h"
#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace term = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;
constexpr auto k_wait = std::chrono::seconds(20);

enum class Admission_case
{
    SUCCESS,
    PREPARE_REFUSED,
    PREPARE_THREW,
    CREATE_FAILED,
    ASSIGNMENT_FAILED,
    ROOT_REFUSED,
    ROOT_THREW,
    RESUME_FAILED,
};

struct Lifecycle_state
{
    ~Lifecycle_state()
    {
        if (root) CloseHandle(root);
    }

    std::mutex mutex;
    std::condition_variable changed;
    Admission_case scenario = Admission_case::SUCCESS;
    HANDLE job = nullptr; // Borrowed only until finish_cleanup returns.
    HANDLE root = nullptr; // Independently duplicated exact occurrence.
    DWORD caller_thread = GetCurrentThreadId();
    unsigned prepared = 0;
    unsigned admitted = 0;
    unsigned begun = 0;
    unsigned finished = 0;
    unsigned destroyed = 0;
    unsigned exits = 0;
    bool valid = true;
    bool hold_admission = false;
    bool hold_cleanup = false;
    std::atomic_uint activity{0};
    std::shared_ptr<term::Windows_conpty_close_control_for_testing> close_control;
};

class Recording_lifecycle final : public vnm_terminal::Windows_terminal_lifecycle
{
public:
    explicit Recording_lifecycle(std::shared_ptr<Lifecycle_state> state)
    : m_state(std::move(state)) {}

    ~Recording_lifecycle() override
    {
        const std::lock_guard lock(m_state->mutex);
        ++m_state->destroyed;
        m_state->changed.notify_all();
    }

    bool prepare(void* job, std::string* error) override
    {
        const std::lock_guard lock(m_state->mutex);
        ++m_state->prepared;
        m_state->caller_thread = GetCurrentThreadId();
        m_state->job = job;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        m_state->valid &= QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0;
        if (m_state->scenario == Admission_case::PREPARE_THREW) {
            throw std::runtime_error("lifecycle preparation fixture");
        }
        if (m_state->scenario == Admission_case::PREPARE_REFUSED) {
            *error = "lifecycle preparation refused";
            return false;
        }
        return true;
    }

    bool root_admitted(void* root, std::string* error) override
    {
        std::unique_lock lock(m_state->mutex);
        ++m_state->admitted;
        BOOL member = FALSE;
        m_state->valid &= IsProcessInJob(root, m_state->job, &member) && member &&
            WaitForSingleObject(root, 0) == WAIT_TIMEOUT;
        m_state->valid &= DuplicateHandle(GetCurrentProcess(), root, GetCurrentProcess(),
            &m_state->root, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0) != FALSE;
        m_state->changed.notify_all();
        m_state->changed.wait(lock, [&] { return !m_state->hold_admission; });
        if (m_state->scenario == Admission_case::ROOT_THREW) {
            throw std::runtime_error("lifecycle admission fixture");
        }
        if (m_state->scenario == Admission_case::ROOT_REFUSED) {
            *error = "lifecycle admission refused";
            return false;
        }
        return true;
    }

    void io_activity() noexcept override { ++m_state->activity; }

    void begin_cleanup() noexcept override
    {
        std::unique_lock lock(m_state->mutex);
        ++m_state->begun;
        m_state->valid &= GetCurrentThreadId() != m_state->caller_thread && m_state->prepared == 1;
        m_state->valid &= m_state->close_control->close_count.load() == 0;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        m_state->valid &= QueryInformationJobObject(m_state->job, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr) != FALSE;
        m_state->changed.notify_all();
        m_state->changed.wait(lock, [&] { return !m_state->hold_cleanup; });
    }

    void finish_cleanup() noexcept override
    {
        const std::lock_guard lock(m_state->mutex);
        ++m_state->finished;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        m_state->valid &= GetCurrentThreadId() != m_state->caller_thread && m_state->begun == 1 &&
            QueryInformationJobObject(m_state->job, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0;
        if (m_state->root) {
            m_state->valid &= WaitForSingleObject(m_state->root, 0) == WAIT_OBJECT_0;
        }
        m_state->job = nullptr;
        m_state->changed.notify_all();
    }

private:
    std::shared_ptr<Lifecycle_state> m_state;
};

template<class Predicate>
bool await_state(const std::shared_ptr<Lifecycle_state>& state, Predicate ready)
{
    std::unique_lock lock(state->mutex);
    return state->changed.wait_for(lock, k_wait, ready);
}

term::Terminal_backend_callbacks callbacks(const std::shared_ptr<Lifecycle_state>& state)
{
    term::Terminal_backend_callbacks result;
    result.output_received = [](QByteArray) {};
    result.error_reported = [](term::Terminal_backend_error) {};
    result.process_exited = [state](term::Terminal_backend_exit exit) {
        (void)exit;
        const std::lock_guard lock(state->mutex);
        ++state->exits;
        state->valid &= state->finished == 1;
        state->changed.notify_all();
    };
    return result;
}

QString own_executable()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? QString::fromWCharArray(path.data(), length) : QString();
}

term::Terminal_launch_config launch(const QString& marker, const QString& event)
{
    term::Terminal_launch_config result;
    result.argv = {own_executable(), QStringLiteral("--lifecycle-child"), marker, event};
    result.initial_grid_size = term::terminal_grid_size_t{24, 80};
    result.working_directory = QFileInfo(marker).absolutePath();
    result.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    return result;
}

int child_main()
{
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args || count != 4) {
        if (args) LocalFree(args);
        return 2;
    }
    const HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, args[3]);
    const HANDLE marker = CreateFileW(args[2], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(args);
    if (!stop || marker == INVALID_HANDLE_VALUE) {
        if (stop) CloseHandle(stop);
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
        return 3;
    }
    CloseHandle(marker);
    constexpr char ready[] = "native-lifecycle-ready\r\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ready, sizeof(ready) - 1, &written, nullptr);
    const DWORD result = WaitForSingleObject(stop, INFINITE);
    CloseHandle(stop);
    return result == WAIT_OBJECT_0 ? 0 : 4;
}

bool run_case(Admission_case scenario, bool destroy_during_cleanup)
{
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/vnm-lifecycle-\u03bb-XXXXXX"));
    if (!check(directory.isValid(), "native lifecycle Unicode directory exists")) return false;
    const QString marker = directory.filePath(QStringLiteral("root-\u03bb.ready"));
    const QString event_name = QStringLiteral("Local\\vnm-lifecycle-%1-%2")
        .arg(GetCurrentProcessId()).arg(GetTickCount64());
    const auto wide_event = event_name.toStdWString();
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, wide_event.c_str());
    if (!check(stop != nullptr, "native lifecycle child stop event exists")) return false;

    const auto state = std::make_shared<Lifecycle_state>();
    state->scenario = scenario;
    state->hold_cleanup = destroy_during_cleanup;
    state->hold_admission = scenario == Admission_case::SUCCESS;
    auto lifecycle = std::make_shared<Recording_lifecycle>(state);
    const std::weak_ptr<Recording_lifecycle> retained = lifecycle;
    auto backend = std::make_unique<term::Windows_conpty_backend>(lifecycle);
    lifecycle.reset();
    state->close_control = std::make_shared<term::Windows_conpty_close_control_for_testing>();
    state->close_control->allow_close.release();
    state->close_control->allow_observer.release();
    backend->set_close_control_for_testing(state->close_control);
    struct Release
    {
        std::shared_ptr<Lifecycle_state> state;
        HANDLE stop;
        ~Release()
        {
            {
                const std::lock_guard lock(state->mutex);
                state->hold_cleanup = false;
                state->hold_admission = false;
            }
            state->changed.notify_all();
            SetEvent(stop);
            CloseHandle(stop);
        }
    } release{state, stop};

    auto config = launch(marker, event_name);
    if (scenario == Admission_case::CREATE_FAILED) {
        config.argv = {own_executable()};
        config.windows_native_arguments = QString(32768, QChar(u'x'));
    }
    if (scenario == Admission_case::ASSIGNMENT_FAILED || scenario == Admission_case::RESUME_FAILED) {
        backend->set_start_fault_for_testing(scenario == Admission_case::ASSIGNMENT_FAILED
            ? term::Windows_conpty_start_fault_for_testing::JOB_ASSIGNMENT
            : term::Windows_conpty_start_fault_for_testing::THREAD_RESUME_FAILURE);
    }
    const bool success = scenario == Admission_case::SUCCESS;
    term::Terminal_backend_result result;
    bool ok = true;
    if (success) {
        std::thread starter([&] { result = backend->start(config, callbacks(state)); });
        ok &= check(await_state(state, [&] { return state->admitted == 1; }),
            "root admission is held before execution");
        // The native admission gate is still held, so the fixture must not
        // execute even while its caller is independently scheduled.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ok &= check(!QFileInfo::exists(marker), "the root cannot run before admission returns");
        {
            const std::lock_guard lock(state->mutex);
            state->hold_admission = false;
        }
        state->changed.notify_all();
        starter.join();
    }
    else {
        result = backend->start(config, callbacks(state));
    }
    ok &= check((result.code != term::Terminal_backend_result_code::REJECTED) == success,
        "native lifecycle admission has the requested outcome");
    const bool preparation_refused = scenario == Admission_case::PREPARE_REFUSED ||
        scenario == Admission_case::PREPARE_THREW;
    ok &= check(result.native_dispatch_occurred != preparation_refused,
        "preparation failure remains distinct from a native dispatch");
    if (success) {
        const auto deadline = std::chrono::steady_clock::now() + k_wait;
        while ((!QFileInfo::exists(marker) || state->activity.load() == 0) &&
            std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok &= check(QFileInfo::exists(marker) && state->activity > 0,
            "admitted Unicode child runs and native I/O reaches the extension");
        if (destroy_during_cleanup) {
            ok &= check(backend->terminate().code != term::Terminal_backend_result_code::REJECTED,
                "explicit termination admits asynchronous native cleanup");
        }
        else {
            SetEvent(stop);
            ok &= check(await_state(state, [&] { return state->exits == 1; }),
                "natural root exit is published after extension cleanup");
        }
    }
    else {
        ok &= check(!QFileInfo::exists(marker), "a rejected child never runs its entry point");
    }
    if (destroy_during_cleanup) {
        ok &= check(await_state(state, [&] { return state->begun == 1; }),
            "native cleanup enters the held host phase");
    }
    backend.reset();
    if (destroy_during_cleanup) {
        {
            const std::lock_guard lock(state->mutex);
            ok &= check(!retained.expired() && state->finished == 0 && state->destroyed == 0,
                "facade returns while the exact lifecycle remains retained for cleanup");
            state->hold_cleanup = false;
        }
        state->changed.notify_all();
    }
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait), "native lifecycle cleanup owners settle");
    const std::lock_guard lock(state->mutex);
    const bool expected_admission = scenario != Admission_case::PREPARE_REFUSED &&
        scenario != Admission_case::PREPARE_THREW && scenario != Admission_case::CREATE_FAILED &&
        scenario != Admission_case::ASSIGNMENT_FAILED;
    ok &= check(state->valid && state->prepared == 1 && state->admitted == unsigned(expected_admission) &&
            state->begun == 1 && state->finished == 1 && state->destroyed == 1 && retained.expired(),
        "native facts and once-only cleanup survive refusal, exit and facade retirement");
    return ok;
}

} // namespace

std::optional<int> run_native_lifecycle_tests(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == "--lifecycle-child") return child_main();
    if (argc != 2 || std::string_view(argv[1]) != "--native-lifecycle") return std::nullopt;
    bool ok = run_case(Admission_case::SUCCESS, false);
    ok &= run_case(Admission_case::SUCCESS, true);
    for (const auto scenario : {Admission_case::PREPARE_REFUSED, Admission_case::PREPARE_THREW,
             Admission_case::CREATE_FAILED, Admission_case::ASSIGNMENT_FAILED,
             Admission_case::ROOT_REFUSED, Admission_case::ROOT_THREW, Admission_case::RESUME_FAILED})
    {
        ok &= run_case(scenario, true);
    }
    return ok ? 0 : 1;
}
