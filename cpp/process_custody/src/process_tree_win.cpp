#include <vnm_process_custody/process_tree.h>

#include <vnm_process_custody/process_clock.h>

#include <windows.h>

#include <algorithm>
#include <thread>

namespace vnm::process_custody {

namespace {

// Job accounting has no completion notification without an I/O completion
// port; the settle loop probes it on this tick, as the Git custody does.
constexpr int k_accounting_probe_tick_ms = 20;

#ifdef LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS
Process_exit_observation_for_test g_exit_observation = Process_exit_observation_for_test::NATIVE;
#endif

DWORD wait_for_processes(DWORD count, const HANDLE* handles, DWORD timeout)
{
#ifdef LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS
    if (g_exit_observation == Process_exit_observation_for_test::WAIT_EXPIRED) {
        return WAIT_TIMEOUT;
    }
    if (g_exit_observation == Process_exit_observation_for_test::WAIT_FAILURE) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return WAIT_FAILED;
    }
#endif
    return WaitForMultipleObjects(count, handles, TRUE, timeout);
}

BOOL read_process_exit_code(HANDLE handle, DWORD* code)
{
#ifdef LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS
    if (g_exit_observation == Process_exit_observation_for_test::QUERY_FAILURE) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
#endif
    return GetExitCodeProcess(handle, code);
}

std::string last_error_text(const char* in_call)
{
    return std::string(in_call) + " failed: error " + std::to_string(GetLastError());
}

bool process_has_ended(HANDLE handle)
{
    return wait_for_processes(1, &handle, 0) == WAIT_OBJECT_0;
}

} // namespace

#ifdef LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS
void set_process_exit_observation_for_test(Process_exit_observation_for_test observation)
{
    g_exit_observation = observation;
}
#endif


struct Process_tree_containment::Impl { HANDLE job = nullptr; };
Process_tree_containment::Process_tree_containment() : m_impl(std::make_unique<Impl>()) {}
Process_tree_containment::~Process_tree_containment() { release(); }
bool Process_tree_containment::establish(const std::string&, std::string* error)
{
    if (available()) { if (error) *error = "generation containment already established"; return false; }
    m_impl->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_impl->job || !SetInformationJobObject(m_impl->job,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        if (error) *error = last_error_text("create generation Job");
        release();
        return false;
    }
    return true;
}
bool Process_tree_containment::available() const { return m_impl->job != nullptr; }
void Process_tree_containment::configure_spawn(Spawn_request& request) const
{
    request.generation_job = m_impl->job;
}
bool Process_tree_containment::terminate_and_observe_empty(
    std::chrono::milliseconds budget, std::string* error)
{
    if (!available()) { if (error) *error = "generation Job unavailable"; return false; }
    const Deadline deadline = Deadline::after(budget);
    do {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(m_impl->job, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0)
            return true;
        if (!TerminateJobObject(m_impl->job, ERROR_OPERATION_ABORTED) && error)
            *error = last_error_text("terminate generation Job");
        if (deadline.expired()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (true);
    if (error && error->empty()) *error = "generation Job emptiness unconfirmed";
    return false;
}
void Process_tree_containment::release()
{
    if (m_impl->job) { CloseHandle(m_impl->job); m_impl->job = nullptr; }
}

struct Process_tree_owner::Impl
{
    HANDLE                     job = nullptr;
    std::vector<std::int64_t>  process_ids;
    std::vector<Native_handle> process_handles;
    std::vector<bool>          collected;
    std::string                observation_error;

    bool job_is_empty(std::string* out_error) const
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(
                job,
                JobObjectBasicAccountingInformation,
                &accounting,
                static_cast<DWORD>(sizeof(accounting)),
                nullptr))
        {
            *out_error = last_error_text("QueryInformationJobObject");
            return false;
        }
        return accounting.ActiveProcesses == 0;
    }
};

Process_tree_owner::Process_tree_owner()
:
    m_impl(std::make_unique<Impl>())
{}

Process_tree_owner::~Process_tree_owner()
{
    for (const HANDLE handle : m_impl->process_handles) {
        CloseHandle(handle);
    }
    if (m_impl->job) {
        // Kill-on-close: whatever terminate_and_reap could not confirm ends
        // with this handle.
        CloseHandle(m_impl->job);
    }
}

bool Process_tree_owner::enable_subreaper()
{
    return false;
}

bool Process_tree_owner::establish(std::string* out_error)
{
    m_impl->job = CreateJobObjectW(nullptr, nullptr);
    if (!m_impl->job) {
        *out_error = last_error_text("CreateJobObjectW");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(
            m_impl->job,
            JobObjectExtendedLimitInformation,
            &limits,
            static_cast<DWORD>(sizeof(limits))))
    {
        *out_error = last_error_text("SetInformationJobObject");
        return false;
    }
    return true;
}

void Process_tree_owner::configure_root_spawn(Spawn_request& in_out_request) const
{
    in_out_request.job = m_impl->job;
}

void Process_tree_owner::record_root(const Spawn_result& in_root)
{
    m_impl->process_ids.push_back(in_root.process_id);
    m_impl->process_handles.push_back(in_root.process_handle);
    m_impl->collected.push_back(false);
}

bool Process_tree_owner::record_descendant(std::int64_t in_process_id, Native_handle in_transferred_handle)
{
    const HANDLE handle = in_transferred_handle;
    if (!handle) {
        return false;
    }
    BOOL belongs = FALSE;
    if (in_process_id <= 0 || static_cast<std::int64_t>(GetProcessId(handle)) != in_process_id ||
        !IsProcessInJob(handle, m_impl->job, &belongs) || !belongs ||
        std::find(m_impl->process_ids.begin(), m_impl->process_ids.end(), in_process_id) !=
            m_impl->process_ids.end())
    {
        CloseHandle(handle);
        return false;
    }
    m_impl->process_ids.push_back(in_process_id);
    m_impl->process_handles.push_back(handle);
    m_impl->collected.push_back(false);
    return true;
}

bool Process_tree_owner::record_descendant_from_process(std::int64_t in_process_id,
    Native_handle in_source_process, Native_handle in_source_handle)
{
    HANDLE retained = nullptr;
    if (!in_source_process || !in_source_handle ||
        !DuplicateHandle(in_source_process, in_source_handle, GetCurrentProcess(), &retained,
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
    {
        return false;
    }
    return record_descendant(in_process_id, retained);
}

Kill_coordinates Process_tree_owner::coordinates() const
{
    Kill_coordinates coordinates;
    coordinates.process_ids     = m_impl->process_ids;
    coordinates.process_handles = m_impl->process_handles;
    return coordinates;
}

std::vector<Native_handle> Process_tree_owner::exit_wait_handles() const
{
    std::vector<Native_handle> handles;
    for (std::size_t i = 0; i < m_impl->process_handles.size(); ++i) {
        if (!m_impl->collected[i]) {
            handles.push_back(m_impl->process_handles[i]);
        }
    }
    return handles;
}

std::vector<Exit_status> Process_tree_owner::reap_available()
{
    m_impl->observation_error.clear();
    std::vector<Exit_status> exits;
    for (std::size_t i = 0; i < m_impl->process_handles.size(); ++i) {
        if (m_impl->collected[i]) {
            continue;
        }
        const auto handle = m_impl->process_handles[i];
        const DWORD wait = wait_for_processes(1, &handle, 0);
        if (wait != WAIT_OBJECT_0) {
            if (wait == WAIT_FAILED) {
                m_impl->observation_error = last_error_text("Process exit wait");
            }
            continue;
        }
        DWORD code = 0;
        if (!read_process_exit_code(handle, &code)) {
            m_impl->observation_error = last_error_text("GetExitCodeProcess");
            continue;
        }
        Exit_status status;
        status.process_id      = m_impl->process_ids[i];
        status.exit_code       = static_cast<int>(code);
        m_impl->collected[i]   = true;
        exits.push_back(status);
    }
    return exits;
}

bool Process_tree_owner::children_remain() const
{
    for (std::size_t i = 0; i < m_impl->process_handles.size(); ++i) {
        if (!m_impl->collected[i]) {
            return true;
        }
    }
    return false;
}

Reap_outcome Process_tree_owner::terminate_and_reap(
    std::chrono::milliseconds /*in_terminate_to_kill*/,
    std::chrono::milliseconds in_reap_deadline)
{
    const auto started = Deadline::Clock::now();
    Reap_outcome outcome;
    const Deadline deadline = Deadline::after(in_reap_deadline);
    outcome.escalated_to_kill = TerminateJobObject(m_impl->job, 1) != FALSE;
    if (!outcome.escalated_to_kill) {
        outcome.error = last_error_text("TerminateJobObject");
    }
    while (outcome.error.empty()) {
        const std::vector<Exit_status> reaped = reap_available();
        outcome.exits.insert(outcome.exits.end(), reaped.begin(), reaped.end());
        if (!m_impl->observation_error.empty()) {
            outcome.error = m_impl->observation_error;
            break;
        }
        if (m_impl->job_is_empty(&outcome.error)) {
            // Job accounting can reach zero just before the process handles
            // become observable as signalled. Wait for every recorded member
            // before the final collection so their exit facts are not lost.
            const std::vector<Native_handle> remaining = exit_wait_handles();
            if (!remaining.empty()) {
                const int remaining_ms = deadline.remaining_ms();
                const DWORD wait = wait_for_processes(
                    static_cast<DWORD>(remaining.size()),
                    remaining.data(),
                    remaining_ms > 0 ? static_cast<DWORD>(remaining_ms) : 0);
                if (wait != WAIT_OBJECT_0) {
                    outcome.error = wait == WAIT_FAILED
                        ? last_error_text("Process exit wait")
                        : "The recorded process exits were not observed before the reap deadline.";
                }
            }
            const std::vector<Exit_status> final_reaped = reap_available();
            outcome.exits.insert(outcome.exits.end(), final_reaped.begin(), final_reaped.end());
            if (!m_impl->observation_error.empty()) {
                outcome.error = m_impl->observation_error;
            }
            outcome.complete = outcome.error.empty() && !children_remain();
            break;
        }
        if (!outcome.error.empty()) {
            break;
        }
        if (deadline.expired()) {
            outcome.error = "The Job did not become empty before the reap deadline.";
            break;
        }
        Sleep(static_cast<DWORD>(Deadline::earlier_timeout_ms(deadline.remaining_ms(), k_accounting_probe_tick_ms)));
    }

    for (std::size_t i = 0; i < m_impl->process_ids.size(); ++i) {
        if (!m_impl->collected[i]) {
            outcome.remaining_children.push_back(m_impl->process_ids[i]);
        }
    }
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline::Clock::now() - started);
    return outcome;
}

bool peek_process_exit(std::int64_t in_process_id, Native_handle in_handle, Exit_status* out_status)
{
    // The Job is the custodian's; the supervisor only reports its adapter's
    // exit, read from the process handle without disturbing Job accounting.
    if (!in_handle || WaitForSingleObject(in_handle, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (!read_process_exit_code(in_handle, &code)) {
        return false;
    }
    out_status->process_id = in_process_id;
    out_status->exit_code  = static_cast<int>(code);
    return true;
}

bool terminate_coordinates(
    const Kill_coordinates&   in_coordinates,
    std::chrono::milliseconds /*in_terminate_to_kill*/,
    std::chrono::milliseconds in_kill_deadline)
{
    // The custodian's Job has KILL_ON_JOB_CLOSE, so losing the custodian
    // already kills every admitted descendant. Never reopen a recorded PID:
    // by the time fallback runs Windows may have recycled it for an unrelated
    // process. Handles are used only if a future transport safely duplicates
    // the original process objects into the desktop.
    const Deadline deadline = Deadline::after(in_kill_deadline);
    std::vector<HANDLE> handles;
    bool requests_succeeded = true;
    for (const HANDLE handle : in_coordinates.process_handles) {
        if (handle) {
            if (!process_has_ended(handle) && !TerminateProcess(handle, 1)) {
                // Natural exit can win between the poll and termination.
                // A failed request is not a failed cleanup if this exact
                // process is subsequently observed signaled.
                const DWORD wait = wait_for_processes(
                    1, &handle, static_cast<DWORD>(deadline.remaining_ms()));
                requests_succeeded = requests_succeeded && wait == WAIT_OBJECT_0;
            }
            handles.push_back(handle);
        }
    }
    bool all_ended = true;
    if (!handles.empty()) {
        const DWORD timeout = static_cast<DWORD>(deadline.remaining_ms());
        all_ended = wait_for_processes(
            static_cast<DWORD>(handles.size()), handles.data(), timeout) == WAIT_OBJECT_0;
    }
    // The coordinate owner retains and closes these handles.
    return requests_succeeded && all_ended;
}

} // namespace vnm::process_custody
