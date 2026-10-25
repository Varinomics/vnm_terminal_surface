#pragma once

#include <vnm_process_custody/process_pipe.h>
#include <vnm_process_custody/process_spawn.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vnm::process_custody {

enum class Cleanup_scope : std::uint32_t
{
    UNSPECIFIED,
    TRANSITIVE_TREE,
    OWNED_PROCESS_GROUPS,
};

#if defined(_WIN32) && defined(LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS)
enum class Process_exit_observation_for_test
{
    NATIVE,
    WAIT_EXPIRED,
    WAIT_FAILURE,
    QUERY_FAILURE,
};

// Overrides only native observation results, never process ownership or
// termination. The Windows fixture still owns real Job-admitted children.
void set_process_exit_observation_for_test(Process_exit_observation_for_test observation);
#endif

struct Exit_status
{
    std::int64_t process_id = 0;
    int          exit_code  = -1;   // -1 when a signal ended the process
    int          signal     = 0;    // 0 when the process exited on its own (always on Windows)
};

/// What the custodian reports in `started` and what the desktop falls back
/// on when the custodian itself is lost: Linux names the supervisor's
/// session / process group for diagnostics and holds a pidfd per recorded
/// process, the only signal authority after custodian loss, since a saved
/// group number can be reused once the tree has ended; Windows names the
/// processes for diagnostics, but never reopens those recyclable IDs: the
/// custodian's Job kill-on-close is the fallback mechanism unless the
/// original process handles are safely duplicated to the desktop. Handles
/// are owned by whoever produced the struct: the owner for coordinates(), the
/// receiving channel for a desktop that got them over the control channel.
/// Darwin coordinates are diagnostic only: the original sole wait owner is
/// the signal authority and cannot be replaced after its loss.
struct Kill_coordinates
{
    Cleanup_scope             cleanup_scope = Cleanup_scope::UNSPECIFIED;
    std::int64_t               process_group = 0;
    std::vector<std::int64_t>  process_ids;       // in recording order: supervisor, adapter
    std::vector<Native_handle> process_handles;   // Linux pidfds / Windows process handles, same order
};

/// What a teardown left behind. escalated_to_kill records that the kill
/// phase was attempted - after the grace passed, or at once for a zero grace;
/// exits holds every status observed during the teardown (Darwin retains
/// the root zombie until its group settles or bounded cleanup is exhausted);
/// remaining_children names children whose cleanup remains unconfirmed
/// (Linux: a tree process still alive after the kill phase; Windows: an
/// uncollected recorded process; Darwin: the diagnostic original root ID,
/// even when its wait was collected without proving group settlement).
struct Reap_outcome
{
    Cleanup_scope             cleanup_scope        = Cleanup_scope::UNSPECIFIED;
    bool                      complete             = false; // settled within cleanup_scope only
    bool                      terminal_unconfirmed = false; // authority retired without scoped settlement
    bool                      escalated_to_kill     = false;
    std::vector<Exit_status>  exits;
    std::vector<std::int64_t> remaining_children;
    std::chrono::milliseconds elapsed{0};
    std::string               error;
};

/// Owner-retained containment for one generation, independent of the custodian.
/// This is NOT another wait owner and NOT the process-wide Sintra family. Linux
/// uses a delegated cgroup v2 directory and atomic child birth; Windows uses an
/// outer generation Job, leaving the custodian's inner kill-on-close Job intact.
/// Failed observations never establish emptiness. The caller retains this object
/// and continues bounded attempts until native settlement, even after timeout.
/// Darwin has no independent containment and reports it unavailable.
class Process_tree_containment
{
public:
    Process_tree_containment();
    ~Process_tree_containment();
    Process_tree_containment(const Process_tree_containment&) = delete;
    Process_tree_containment& operator=(const Process_tree_containment&) = delete;

    bool establish(const std::string& nonce, std::string* error);
    bool available() const;
    void configure_spawn(Spawn_request& request) const;
    bool terminate_and_observe_empty(std::chrono::milliseconds budget, std::string* error);
    void release(); // only after native emptiness, or before any child admission

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#if defined(_WIN32)
/// Includes CPU time from active and exited members of the retained Job. The
/// caller keeps the borrowed handle alive; this query does not change custody.
std::optional<std::uint64_t> job_cpu_time_usec(Native_handle job);
#endif

#if defined(__linux__)
/// Reads cumulative CPU time from an existing cgroup v2 directory. The caller
/// retains the directory; this query neither creates nor retires a scope.
std::optional<std::uint64_t> cgroup_cpu_time_usec(int directory);

/// Optional CPU accounting for one workload and its descendants. The caller
/// and helper retain the same delegated scope until existing family cleanup
/// has retired it. cgroup.kill must be available for cleanup after helper loss;
/// the scope owns no process waits. Only acknowledged spawn admission makes
/// its counters attributable. An enabled CPU controller is not required.
class Process_cpu_scope
{
public:
    Process_cpu_scope();
    ~Process_cpu_scope();
    Process_cpu_scope(const Process_cpu_scope&) = delete;
    Process_cpu_scope& operator=(const Process_cpu_scope&) = delete;

    bool establish(const std::string& nonce, std::string* error);
    bool adopt(int directory, const std::string& name, std::string* error);
    void configure_spawn(Spawn_request& request) const;
    std::optional<std::uint64_t> cpu_time_usec() const;
    bool release(); // failed removal retains identity; shared retirement is idempotent
    bool terminate_and_release(std::chrono::milliseconds budget, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif

#if defined(__APPLE__)
/// Best-effort CPU observations of one root and its discoverable descendants.
/// The root uses its original PROC_PIDTBSDINFO birth time in microseconds.
/// New members establish a baseline; subsequent per-process deltas accrue to
/// a monotonic counter that retains credited CPU when members disappear.
/// Short-lived children and CPU outside the observation window can be missed.
/// Surviving observed descendants remain tracked after reparenting. This owns
/// no process control or wait authority. Callers serialize all access.
class Process_cpu_tracker
{
public:
    Process_cpu_tracker();
    ~Process_cpu_tracker();
    Process_cpu_tracker(const Process_cpu_tracker&) = delete;
    Process_cpu_tracker& operator=(const Process_cpu_tracker&) = delete;

    void set_root(std::int64_t process_id, std::uint64_t birth_token) noexcept;
    // Discovery or counter failures are unavailable, never an idle sample.
    std::optional<std::uint64_t> cpu_time_usec();
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif

/// The custodian's ownership of one generation's process tree. Linux: this
/// process becomes the subreaper of everything below it, the root (the
/// supervisor) is spawned into its own session and process group, and this
/// object is the only place that calls waitid() for that tree. Windows: this
/// object owns the kill-on-close Job the root is created into.
/// Darwin retains the direct root and its original process group for COMMAND,
/// or the controlling PTY session for PTY. Both report OWNED_PROCESS_GROUPS,
/// never transitive detached-descendant cleanup.
class Process_tree_owner
{
public:
    Process_tree_owner();
    ~Process_tree_owner();

    Process_tree_owner(const Process_tree_owner&)            = delete;
    Process_tree_owner& operator=(const Process_tree_owner&) = delete;

    /// Linux: prctl(PR_SET_CHILD_SUBREAPER, 1). False elsewhere.
    bool enable_subreaper();

    /// Linux: enable_subreaper() plus the SIGCHLD signalfd behind
    /// exit_wait_handles(); Windows: creates the Job with
    /// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
    /// and no breakaway.
    bool establish(std::string* out_error);

#if defined(__APPLE__)
    /// The persistent helper retains both descriptors until cleanup ends.
    /// Establishes its controlling session before the single root admission;
    /// the root receives a separate foreground group in that same session.
    bool establish_pty(Native_handle master, Native_handle slave, std::string* out_error);

    /// Takes the failed spawn's remaining observer/wait obligation, or its
    /// exact already-collected receipt. Failure never means no workload.
    void record_failed_root(const Darwin_spawn_failure& failure);
#endif

    /// Makes a spawn request produce the tree's root: Linux sets new_session,
    /// Windows assigns the Job.
    void configure_root_spawn(Spawn_request& in_out_request) const;

    /// Takes ownership of the root's process handle; Linux records its pid as
    /// the process group.
    void record_root(const Spawn_result& in_root);

    /// Records a descendant reported by the root (the adapter). A transferred
    /// handle is consumed. Windows requires the exact handle and verifies its
    /// identity and membership in this Job; it never reopens a numeric PID.
    /// Linux may open an untransferred pidfd while its subreaper retains the
    /// original occurrence. Observation failure is refusal, not exit proof.
    bool record_descendant(std::int64_t in_process_id, Native_handle in_transferred_handle);

#if defined(_WIN32)
    /// Duplicate an adapter handle from the retained source process occurrence.
    /// The source keeps its handle table entry owned until the status receipt.
    bool record_descendant_from_process(std::int64_t in_process_id,
        Native_handle in_source_process, Native_handle in_source_handle);
#endif

    Kill_coordinates coordinates() const;

    /// Handles a wait loop includes to learn that a descendant may have
    /// exited: Linux the SIGCHLD signalfd, Windows the recorded process handles.
    std::vector<Native_handle> exit_wait_handles() const;

    /// Sole wait owner: reports available statuses without blocking. Darwin
    /// retains the reported root zombie until terminate_and_reap settles its
    /// original group or retires exhausted cleanup; callers must not reap it.
    std::vector<Exit_status> reap_available();

    /// Linux: whether any child (including a reparented orphan) still exists;
    /// Windows: whether a recorded process has not been collected.
    bool children_remain() const;

    /// Terminate, wait in_terminate_to_kill for the tree to vanish, kill,
    /// then reap until no child remains or in_reap_deadline passes after the
    /// kill. A zero in_terminate_to_kill skips graceful termination and
    /// attempts forced termination immediately. Linux signals the recorded
    /// pidfds, the root's process group
    /// while the root is unreaped, and every child this subreaper acquires
    /// meanwhile, including one that left the root's session; having no
    /// child left is the completion. Windows has no graceful terminate for a
    /// Job, so the kill happens at once and in_terminate_to_kill is not used.
    /// Darwin may return terminal_unconfirmed after an authorized kill attempt and
    /// the bounded observation period, but only after observing and collecting
    /// the exact root's exit. That outcome stays unconfirmed and retires every
    /// subsequent signal to its original group; a live root remains pending.
    /// Signal failures remain errors and never establish scoped settlement.
    Reap_outcome terminate_and_reap(
        std::chrono::milliseconds in_terminate_to_kill,
        std::chrono::milliseconds in_reap_deadline);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Peeks whether a process has exited, without reaping it, so the tree's sole
/// reaper (the custodian) still collects the status. True with out_status
/// filled once it has exited, false while it is still running. The supervisor
/// uses this on its adapter child to report ADAPTER_EXITED over the link while
/// leaving the reap to the custodian. Linux: waitid(WNOWAIT) on the pid;
/// Windows: WaitForSingleObject / GetExitCodeProcess on the process handle.
bool peek_process_exit(std::int64_t in_process_id, Native_handle in_handle, Exit_status* out_status);

/// The desktop's fallback after custodian loss: sends terminate through the
/// recorded coordinates, waits in_terminate_to_kill, sends kill, and waits
/// up to in_kill_deadline more for the recorded processes to end; a zero
/// in_terminate_to_kill kills at once, as terminate_and_reap does. Never
/// waits any of those processes; their statuses belong to the operating
/// system's reaper. Linux signals only the recorded pidfds and returns true
/// once each has ended, false when none was recorded; that covers the
/// recorded processes, not descendants the lost custodian would have
/// acquired. Windows returns true once every safely duplicated process
/// handle has ended, or immediately when Job kill-on-close is the only safe
/// authority and no such handle was transferred.
bool terminate_coordinates(
    const Kill_coordinates&   in_coordinates,
    std::chrono::milliseconds in_terminate_to_kill,
    std::chrono::milliseconds in_kill_deadline);

} // namespace vnm::process_custody
