#pragma once

#include <vnm_process_custody/process_pipe.h>

#include <cstdint>
#include <functional>
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
#if defined(__linux__)
    bool                              controlling_terminal = false; // mapped stdin becomes the controlling PTY
    std::function<bool()>             admit_execution; // parent-only, after pidfd acquisition, before exec
    int                               generation_cgroup = -1; // retained cgroup directory; birth must be atomic
#endif
#if defined(_WIN32)
    void*                             generation_job = nullptr; // nested after the process-wide family Job
    void*                             job = nullptr;         // HANDLE the child joins at creation
#endif
};

struct Spawn_result
{
    bool          ok             = false;
    std::int64_t  process_id     = 0;
    // Owned by the caller even when ok is false: a failed Windows resume can
    // leave a created child whose native cleanup has not yet been confirmed.
    Native_handle process_handle = k_invalid_handle;   // Linux: pidfd; Windows: process handle.
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
