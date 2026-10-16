#pragma once

#include <vnm_process_custody/process_pipe.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vnm::process_custody {

#if defined(_WIN32) && defined(LOGONOMIC_ENABLE_PROCESS_SPAWN_TEST_HOOKS)
// Creates a real suspended child, but rejects its resume and leaves cleanup
// observation unconfirmed. The fixture retains the original native identity.
void set_spawn_start_unconfirmed_for_test(bool enabled);
#endif

/// One handle the child inherits. child_number 0, 1 and 2 are the child's
/// stdin, stdout and stderr on both platforms. Any other number is, on Linux,
/// the descriptor number the child finds the handle at; on Windows a
/// non-stdio handle is duplicated to a child-visible value before creation;
/// child_number is not used there.
struct Inherited_descriptor
{
    Native_handle parent_handle = k_invalid_handle;
    int           child_number  = -1;
};

struct Inherited_reference
{
    std::size_t inherited_index = 0;
};

struct Spawn_argument
{
    using Value = std::variant<std::string, Inherited_reference>;

    Spawn_argument() = default;
    Spawn_argument(const char* in_text) : value(std::string(in_text)) {}
    Spawn_argument(std::string in_text) : value(std::move(in_text)) {}
    Spawn_argument(Inherited_reference in_reference) : value(in_reference) {}

    Value value;
};

struct Spawn_request
{
    std::string                       executable;        // absolute path, executed directly (no shell)
    std::vector<Spawn_argument>       argv;              // argv[0] included; inherited refs resolve during spawn
    std::vector<std::string>          environment;       // "NAME=VALUE", the complete environment
    std::string                       working_directory; // empty: inherit
    std::vector<Inherited_descriptor> inherited;         // everything else is closed / not inherited
    int                               failed_start_wait_ms = 5000; // preserves the existing failed-resume budget
    bool                              new_session = false;   // Linux: setsid(); no Windows equivalent
#if defined(__linux__) || defined(__APPLE__)
    bool                              controlling_terminal = false; // mapped stdin becomes the controlling PTY
    std::function<bool()>             admit_execution; // parent-only, after pidfd acquisition, before exec
#endif
#if defined(__linux__)
    int                               generation_cgroup = -1; // retained cgroup directory; birth must be atomic
#endif
#if defined(__APPLE__)
    bool                              new_process_group = false; // same retained session, distinct shell group
    bool                              foreground_terminal = false; // mapped stdin; after group creation
    // Local opt-in only, never serialized. One deadline covers the exec error
    // pipe and a separately retained native NOTE_EXEC observation.
    std::optional<std::chrono::steady_clock::time_point> execution_deadline;
#endif
#if defined(_WIN32)
    void*                             generation_job = nullptr; // nested after the process-wide family Job
    void*                             job = nullptr;         // HANDLE the child joins at creation
#endif
};

#if defined(__APPLE__)
struct Darwin_spawn_failure
{
    std::int64_t process_id = 0;
    std::uint64_t birth_token = 0;
    Native_handle process_handle = k_invalid_handle; // caller owns it if the sole wait could not be collected
    bool execution_admitted = false;
    bool wait_collected = false;
    int wait_status = 0;
};
#endif

struct Spawn_result
{
    bool          ok             = false;
    std::int64_t  process_id     = 0;
    // Owned by the caller even when ok is false: a failed Windows resume can
    // leave a created child whose native cleanup has not yet been confirmed.
    Native_handle process_handle = k_invalid_handle;   // Linux: pidfd; Darwin: local kqueue; Windows: process handle.
#if defined(__APPLE__)
    std::uint64_t process_birth_token = 0; // original PROC_PIDTBSDINFO birth time, in microseconds
    bool execution_confirmed = false; // only a registered-before-release NOTE_EXEC proves this
    // Failure alone is not no-birth evidence. This is true only before native
    // birth, or after the still-gated child was positively collected without
    // admitting execution. Admitted/uncertain failures retain their identity
    // and exact collected status (or remaining observation handle) separately.
    bool no_workload_admitted = false;
    std::optional<Darwin_spawn_failure> failed_birth;
#endif
    std::vector<Native_handle> inherited_handles;      // child-visible values, parallel to Spawn_request::inherited
    std::string   error;
};

/// Starts the child described by the request. Linux: fork, setsid when
/// asked, default signal dispositions and an empty signal mask, the
/// descriptor map applied with dup2, every other descriptor closed or marked
/// close-on-exec, chdir, execve; an exec failure is reported through the
/// result, never through a child that lingers. Windows: CreateProcessW with
/// PROC_THREAD_ATTRIBUTE_HANDLE_LIST (and PROC_THREAD_ATTRIBUTE_JOB_LIST when
/// a job is given), CREATE_SUSPENDED, then resumed once creation succeeded.
Spawn_result spawn(const Spawn_request& in_request);

#if defined(__APPLE__)
/// Creates a local NOTE_EXIT kqueue after checking the original birth identity
/// before and after registration. This is observation only, never wait or
/// signal authority. Keep the event unconsumed: poll(POLLIN) stays latched at
/// death, including across PID reuse. Failure/absence cannot prove death.
Native_handle open_process_observer(std::int64_t process_id, std::uint64_t birth_token, std::string* error);

enum class Native_process_observation { LIVE, ENDED, UNCONFIRMED };

struct Native_process_status
{
    Native_process_observation state = Native_process_observation::UNCONFIRMED;
    bool status_valid = false;
    int wait_status = 0;
};

/// Receiver-local NOTE_EXITSTATUS observation of one original occurrence.
/// It grants neither wait nor signal authority. open() is one-shot; observe()
/// consumes the kernel event once and retains its terminal status or error.
/// The caller serializes access. A normal code has meaning only after the
/// caller separately admitted the exact helper's outcome contract.
class Native_process_status_observer
{
public:
    Native_process_status_observer();
    ~Native_process_status_observer();
    Native_process_status_observer(Native_process_status_observer&&) noexcept;
    Native_process_status_observer& operator=(Native_process_status_observer&&) noexcept;
    Native_process_status_observer(const Native_process_status_observer&) = delete;
    Native_process_status_observer& operator=(const Native_process_status_observer&) = delete;

    bool open(std::int64_t process_id, std::uint64_t birth_token, std::string* error);
    Native_handle native_handle() const noexcept;
    Native_process_status observe() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif

Inherited_reference inherited_argument(std::size_t in_inherited_index);

/// This process's stdin, stdout and stderr as inherited descriptors at the
/// same positions, for a child that shares them (the custodian's supervisor
/// keeps the custodian's diagnostics stream).
std::vector<Inherited_descriptor> inherit_standard_streams();

/// Once per process that owns pipes or channels of this library. Linux:
/// SIGPIPE is ignored so a closed peer surfaces as EPIPE / CLOSED (spawn()
/// restores the default disposition in every child). Windows: nothing.
bool prepare_parent_process(std::string* out_error);

} // namespace vnm::process_custody
