#pragma once

#include <vnm_process_custody/process_pipe.h>
#include <vnm_process_custody/process_spawn.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vnm::process_custody {

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
struct Kill_coordinates
{
    std::int64_t               process_group = 0;
    std::vector<std::int64_t>  process_ids;       // in recording order: supervisor, adapter
    std::vector<Native_handle> process_handles;   // Linux pidfds / Windows process handles, same order
};

/// What a teardown left behind. escalated_to_kill records that the kill
/// signal was sent - after the grace passed, or at once for a zero grace;
/// exits holds every status reaped during the teardown, in reap order;
/// remaining_children names children whose cleanup remains unconfirmed
/// (Linux: a tree process still alive after the kill phase; Windows: an
/// uncollected recorded process).
struct Reap_outcome
{
    bool                      complete          = false;   // nothing of the tree is left
    bool                      escalated_to_kill = false;
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

/// The custodian's ownership of one generation's process tree. Linux: this
/// process becomes the subreaper of everything below it, the root (the
/// supervisor) is spawned into its own session and process group, and this
/// object is the only place that calls waitid() for that tree. Windows: this
/// object owns the kill-on-close Job the root is created into.
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

    /// Sole wait owner: collects every status available without blocking.
    std::vector<Exit_status> reap_available();

    /// Linux: whether any child (including a reparented orphan) still exists;
    /// Windows: whether a recorded process has not been collected.
    bool children_remain() const;

    /// Terminate, wait in_terminate_to_kill for the tree to vanish, kill,
    /// then reap until no child remains or in_reap_deadline passes after the
    /// kill. A zero in_terminate_to_kill sends no terminate at all: the tree
    /// is killed at once, so every status it produces carries the kill
    /// signal. Linux signals the recorded pidfds, the root's process group
    /// while the root is unreaped, and every child this subreaper acquires
    /// meanwhile, including one that left the root's session; having no
    /// child left is the completion. Windows has no graceful terminate for a
    /// Job, so the kill happens at once and in_terminate_to_kill is not used.
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
