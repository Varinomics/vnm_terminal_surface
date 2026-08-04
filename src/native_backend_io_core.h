#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <QByteArray>
#include <QString>
#include <QStringView>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace vnm_terminal::internal {

constexpr std::size_t k_native_backend_output_read_chunk_bytes =   16U * 1024U;
constexpr std::size_t k_native_backend_max_queued_write_bytes  = 1024U * 1024U;

struct Native_backend_start_gate
{
    std::mutex& mutex;
    bool&       running;
    bool&       start_attempted;
    bool&       start_in_progress;
};

struct Native_backend_start_precheck
{
    Terminal_backend_result                         result;
    std::optional<Terminal_effective_launch_config> effective_config;
};

// Non-owning view of the caller-admission and worker-identity state a native backend
// keeps. The fields stay members of the backend and stay under the backend's own mutex,
// because backend decisions read them together with backend state under a single lock:
// the POSIX backend claims its deferred master descriptor while holding that lock, and
// both destructors decide between inline and deferred teardown from it. A registry with
// a lock of its own would change what those decisions observe.
struct native_backend_call_state_t
{
    std::mutex&                mutex;
    std::condition_variable&   public_call_cv;
    std::size_t&               public_call_depth;
    std::set<std::thread::id>& worker_thread_ids;
    bool&                      startup_aborted;
};

void enter_native_backend_public_call(
    native_backend_call_state_t        call_state);

void wait_for_native_backend_public_calls(
    native_backend_call_state_t        call_state);

bool native_backend_has_active_public_call(
    native_backend_call_state_t        call_state);

// True when the calling thread is one of this backend's own worker threads. Destruction
// reached from a worker callback must not tear down inline, because shutdown() joins the
// worker threads and would detach the calling thread and free the backend while that
// worker is still on the stack.
bool is_native_backend_worker_thread(
    native_backend_call_state_t        call_state);

// True when backend destruction must hand its teardown to a fresh thread instead of
// running it inline: either the calling thread is one of the backend's own workers,
// so inline teardown would join the thread it runs on, or a public backend call is
// still unwinding, so the impl must not be freed under live stack frames. The
// worker-id set and the public-call depth are read as two separate locked snapshots,
// the same observation shape both destructors have always used.
bool must_defer_native_backend_destruction(
    native_backend_call_state_t        call_state);

// Records the calling worker's id, holds it at `startup_gate` until start() has committed
// its thread members, and reports whether the worker may run. False means start() aborted
// and the worker must return without running any callback-capable work.
//
// The id is recorded BEFORE the gate opens, so a worker is always in the id set by the
// time it can deliver a callback. is_native_backend_worker_thread depends on exactly that
// ordering, and it is the reason workers are admitted through one function rather than
// registering themselves. Recording ids rather than comparing std::thread members also
// keeps destruction independent of unsynchronized reads of those members.
bool admit_native_backend_worker(
    native_backend_call_state_t        call_state,
    std::latch&                        startup_gate);

// Releases a startup gate that start() could not finish arming. Marks the startup as
// aborted under the mutex first, then opens the gate, so every worker released by it sees
// the abort and returns instead of running against half-committed state.
void abort_native_backend_startup_gate(
    native_backend_call_state_t        call_state,
    std::latch&                        startup_gate);

// Leaves a public call and, when it was the outermost one, runs `on_last_public_call`
// under the mutex before waking the waiters. The POSIX backend uses that hook to claim
// the master descriptor it could not close while a call was in flight; claiming it
// without the lock would race a call that entered in between.
template <typename On_last_public_call_fn>
void leave_native_backend_public_call(
    native_backend_call_state_t        call_state,
    On_last_public_call_fn&&           on_last_public_call)
{
    std::lock_guard<std::mutex> lock(call_state.mutex);
    --call_state.public_call_depth;
    if (call_state.public_call_depth == 0U) {
        on_last_public_call();
        call_state.public_call_cv.notify_all();
    }
}

// Default no-op action for the injected wake hooks: does nothing. Windows passes
// it where POSIX wakes its I/O threads, because the ConPTY backend has no I/O
// threads to wake.
struct Native_backend_empty_action
{
    void operator()() const noexcept {}
};

// Default no-op claim for hooks that run under a lock and return the action to
// run after it is released: claims nothing and defers nothing. Windows passes it
// for both the public-call guard and exit publication: nothing is deferred to the
// outermost public call on that backend (the ConPTY handle is closed through its
// own active-call counter), and exit publication sets no extra stop flags and
// wakes no I/O threads there.
struct Native_backend_empty_claim
{
    Native_backend_empty_action operator()() const noexcept { return {}; }
};

// RAII scope for one public backend call: enters the call on construction and leaves
// it on destruction through the shared enter/leave primitives, so both backends
// bracket their public surface with the same nesting, locking and wakeup shape. When
// the leaving call was the outermost one, the leave first runs the on-last-call
// claim under the backend mutex (before waking the destruction waiters), then runs
// the action the claim returned after the mutex is released. The POSIX backend
// claims its deferred master descriptor in the first step and closes it in the
// second; claiming without the lock would race a call that entered in between, and
// the close keeps the claim-under-the-lock, close-after-unlock shape every deferred
// close in that backend already follows.
template <typename On_last_call_claim_fn = Native_backend_empty_claim>
class Native_backend_public_call_guard
{
public:
    explicit Native_backend_public_call_guard(
        native_backend_call_state_t call_state,
        On_last_call_claim_fn       on_last_call_claim = {})
    :
        m_call_state(call_state),
        m_on_last_call_claim(std::move(on_last_call_claim))
    {
        enter_native_backend_public_call(m_call_state);
    }

    Native_backend_public_call_guard(const Native_backend_public_call_guard&) = delete;
    Native_backend_public_call_guard& operator=(const Native_backend_public_call_guard&) = delete;

    ~Native_backend_public_call_guard()
    {
        std::optional<decltype(m_on_last_call_claim())> deferred_action;
        leave_native_backend_public_call(m_call_state, [&] {
            deferred_action.emplace(m_on_last_call_claim());
        });
        if (deferred_action.has_value()) {
            (*deferred_action)();
        }
    }

private:
    native_backend_call_state_t m_call_state;
    On_last_call_claim_fn       m_on_last_call_claim;
};

// Runs impl->shutdown() and deletes impl on a fresh, non-worker thread, after
// waiting for in-flight public calls to drain, so join_native_backend_threads()
// can join the worker threads and any public backend call already on the stack
// can return before the impl is freed. Takes ownership of impl. When the thread
// cannot be spawned, runs on_spawn_failure instead: the impl is deliberately
// leaked (safe: no use-after-free) while the backend force-terminates its child
// process tree.
template <typename Impl, typename On_spawn_failure_fn>
void defer_native_backend_shutdown_and_delete(
    Impl*                        impl,
    native_backend_call_state_t  call_state,
    On_spawn_failure_fn&&        on_spawn_failure) noexcept
{
    try {
        std::thread([impl, call_state] {
            wait_for_native_backend_public_calls(call_state);
            impl->shutdown();
            delete impl;
        }).detach();
    }
    catch (...) {
        on_spawn_failure();
    }
}

// Spawns one worker thread that is admitted through the shared startup gate
// before it runs `loop`: the worker records its id, waits at the gate until the
// spawner has committed its thread members, and returns without running `loop`
// when the startup was aborted. Both backends ran this trampoline as a member
// function; it lives here so the admit-before-run ordering exists once.
template <typename Loop_fn>
std::thread spawn_native_backend_gated_worker(
    native_backend_call_state_t        call_state,
    std::shared_ptr<std::latch>        startup_gate,
    Loop_fn&&                          loop)
{
    return std::thread(
        [call_state, startup_gate, loop = std::forward<Loop_fn>(loop)]() mutable {
            if (!admit_native_backend_worker(call_state, *startup_gate)) {
                return;
            }
            loop();
        });
}

// Spawns the reader, writer and wait workers through one startup gate and opens
// the gate once all three thread members are committed, so no worker can run
// callback-capable work against half-committed state. When a spawn throws, the
// gate is aborted BEFORE `on_spawn_failure` runs, so an already-spawned worker
// sees the abort and returns instead of running. `on_spawn_failure` is
// deliberately backend-specific: each backend reports the failure with its own
// label, shuts down, and rejects the start attempt; the shared flow does not
// pick a label.
template <typename Read_loop_fn, typename Write_loop_fn, typename Wait_loop_fn, typename On_spawn_failure_fn>
Terminal_backend_result start_native_backend_workers(
    native_backend_call_state_t        call_state,
    std::thread&                       reader_thread,
    std::thread&                       writer_thread,
    std::thread&                       wait_thread,
    Read_loop_fn&&                     read_loop,
    Write_loop_fn&&                    write_loop,
    Wait_loop_fn&&                     wait_loop,
    On_spawn_failure_fn&&              on_spawn_failure)
{
    std::shared_ptr<std::latch> startup_gate;
    try {
        startup_gate = std::make_shared<std::latch>(1);
        reader_thread = spawn_native_backend_gated_worker(
            call_state,
            startup_gate,
            std::forward<Read_loop_fn>(read_loop));
        writer_thread = spawn_native_backend_gated_worker(
            call_state,
            startup_gate,
            std::forward<Write_loop_fn>(write_loop));
        wait_thread = spawn_native_backend_gated_worker(
            call_state,
            startup_gate,
            std::forward<Wait_loop_fn>(wait_loop));
        startup_gate->count_down();
    }
    catch (const std::exception& error) {
        if (startup_gate) {
            abort_native_backend_startup_gate(call_state, *startup_gate);
        }
        return on_spawn_failure(error);
    }

    return backend_accept();
}

// Spawns the termination-escalation worker through one startup gate. Unlike
// start_native_backend_workers, a failed spawn only counts the gate down
// instead of aborting it: the failed attempt produced no worker, so no waiter
// exists that must observe an abort. Both backends had this shape; it is
// preserved verbatim. `run_termination` carries the escalation arguments and
// `on_spawn_failure` the backend-specific fallback - Windows force-terminates
// through the job object, POSIX signals its targets - each with its own
// labelled message and rejection, verbatim.
template <typename Run_termination_fn, typename On_spawn_failure_fn>
Terminal_backend_result start_native_backend_termination_escalation(
    native_backend_call_state_t        call_state,
    std::thread&                       termination_thread,
    Run_termination_fn&&               run_termination,
    On_spawn_failure_fn&&              on_spawn_failure)
{
    std::shared_ptr<std::latch> startup_gate;
    try {
        startup_gate = std::make_shared<std::latch>(1);
        termination_thread = spawn_native_backend_gated_worker(
            call_state,
            startup_gate,
            std::forward<Run_termination_fn>(run_termination));
        startup_gate->count_down();
    }
    catch (const std::exception& error) {
        if (startup_gate) {
            startup_gate->count_down();
        }
        return on_spawn_failure(error);
    }

    return backend_accept();
}

struct native_backend_output_delivery_limits_t
{
    qsizetype                             high_watermark_bytes = 0;
    qsizetype                             delivery_chunk_bytes = 0;
    qsizetype                             read_chunk_bytes     = 1;
};

Native_backend_start_precheck validate_native_backend_start_preconditions(
    const Terminal_launch_config&      config,
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    QStringView                        backend_label);

void clear_native_backend_start_in_progress(
    Native_backend_start_gate          start_gate);

Terminal_backend_result reject_native_backend_start_attempt(
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    Terminal_backend_error_code        code,
    QString                            message);

Terminal_backend_result reject_native_backend_start_with_report(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

bool native_backend_write_queue_can_accept(
    std::size_t                        queued_write_bytes,
    std::size_t                        incoming_write_bytes);

void add_native_backend_queued_write_bytes(
    std::size_t&                       queued_write_bytes,
    std::size_t                        incoming_write_bytes);

void remove_native_backend_queued_write_bytes(
    std::size_t&                       queued_write_bytes,
    std::size_t                        completed_write_bytes);

native_backend_output_delivery_limits_t derive_native_backend_output_delivery_limits(
    const std::optional<Terminal_backend_output_delivery_limits>&
                                       configured_limits,
    std::optional<std::size_t>         paused_output_high_watermark_ceiling_bytes =
                                           std::nullopt);

void append_native_backend_paused_output(
    QByteArray&                        paused_output,
    QByteArray                         bytes);

// Moves up to `delivery_chunk_bytes` from the head of the paused-output FIFO into
// `delivery_bytes` and marks the delivery in progress, so a second take cannot
// overtake the first. Returns false without touching the FIFO when replay is
// disabled (chunk bytes at or below zero), a delivery is already in progress, or
// the FIFO is empty. The caller must hold the backend mutex guarding all three.
bool take_native_backend_paused_output_for_delivery_locked(
    QByteArray&                        paused_output,
    QByteArray&                        delivery_bytes,
    bool&                              paused_output_delivery_in_progress,
    qsizetype                          delivery_chunk_bytes);

void report_native_backend_error(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

void deliver_native_backend_output(
    const Terminal_backend_callbacks&  callbacks,
    QByteArray                         bytes);

// Snapshots the callbacks under the backend mutex and reports the error through
// the snapshot, so the report never runs while the lock is held. This was both
// backends' report_error member, verbatim.
void report_native_backend_error_with_snapshot(
    std::mutex&                        mutex,
    Terminal_backend_callbacks&        callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

// Snapshots the callbacks under the backend mutex and delivers the bytes
// through the snapshot, so the delivery never runs while the lock is held.
// This was both backends' deliver_output member, verbatim.
void deliver_native_backend_output_with_snapshot(
    std::mutex&                        mutex,
    Terminal_backend_callbacks&        callbacks,
    QByteArray                         bytes);

// Appends `bytes` to the paused-output FIFO, or delivers them when nothing is pending.
//
// The FIFO is never bypassed. `can_buffer_paused_output` is an admission bound the
// caller's reader must keep satisfiable, not an overflow relief valve: the else branch
// emits `bytes` immediately, so taking it while the FIFO is non-empty would put newer
// bytes ahead of older ones and corrupt the VT stream the session parses. Both backends
// uphold that by parking their read loop on the output condition variable once the paused
// buffer reaches the high watermark, which is why the else branch is normally reached only
// with an empty FIFO. Windows also clamps every read to the room left under the watermark,
// because only its predicate weighs the incoming chunk size; the POSIX predicate weighs
// the buffered size alone, so there the clamp only bounds the overshoot. Both backends fix
// these limits in `start()` before the reader thread exists, so a read is always clamped
// against the same numbers the predicate later tests.
//
// The one clause that can refuse with bytes still buffered is the POSIX `m_stopping`
// clause, and both of its shapes are safe. The two shutdown paths clear the callbacks
// under the same lock that sets the flag, so the else branch snapshots an empty
// `output_received` and emits nothing. The exit-report path leaves the callbacks live, but
// it runs only after the reader thread has finished and the pending buffer has been
// drained, so no reader is left to take the else branch against a non-empty FIFO.
//
// A reader that reads more than its own predicate admits reintroduces the reordering.
template <typename Can_buffer_paused_output_fn>
void deliver_or_buffer_native_backend_output(
    std::mutex&                        mutex,
    Terminal_backend_callbacks&        callbacks,
    QByteArray&                        paused_output,
    bool&                              output_paused,
    bool&                              paused_output_delivery_in_progress,
    QByteArray                         bytes,
    Can_buffer_paused_output_fn&&      can_buffer_paused_output)
{
    bool should_deliver = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if ((output_paused ||
             paused_output_delivery_in_progress ||
             !paused_output.isEmpty()) &&
            can_buffer_paused_output())
        {
            append_native_backend_paused_output(paused_output, std::move(bytes));
        }
        else {
            should_deliver = true;
        }
    }

    if (!should_deliver) {
        return;
    }

    Terminal_backend_callbacks callback_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        callback_snapshot = callbacks;
    }

    deliver_native_backend_output(callback_snapshot, std::move(bytes));
}

// Drains the paused-output FIFO after a delivery started by
// take_native_backend_paused_output_for_delivery_locked: keeps delivering chunks
// while output is unpaused (or the caller's stop condition holds) and bytes remain,
// then clears the in-progress flag and wakes the reader. Does nothing when
// `delivery_started` is false.
//
// `stop_requested` is evaluated under the mutex and is deliberately
// backend-specific: Windows stops ignoring the pause once
// `m_stopping || m_shutdown_started`, POSIX once `m_process_stopping || m_stopping`.
// The two stop flag sets are not interchangeable and the shared flow does not pick
// one. `on_drain_finished` runs after the output condition variable is woken;
// POSIX uses it to wake its I/O threads, Windows passes Native_backend_empty_action
// because it has no I/O threads to wake.
template <typename Stop_requested_fn, typename Deliver_output_fn, typename On_drain_finished_fn>
void finish_native_backend_paused_output_delivery(
    std::mutex&                        mutex,
    std::condition_variable&           output_cv,
    QByteArray&                        paused_output,
    bool&                              output_paused,
    bool&                              paused_output_delivery_in_progress,
    qsizetype                          delivery_chunk_bytes,
    bool                               delivery_started,
    Stop_requested_fn&&                stop_requested,
    Deliver_output_fn&&                deliver_output,
    On_drain_finished_fn&&             on_drain_finished)
{
    if (!delivery_started) {
        return;
    }

    for (;;) {
        QByteArray next_paused_output;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if ((!output_paused || stop_requested()) &&
                !paused_output.isEmpty())
            {
                const qsizetype byte_count = std::min(
                    paused_output.size(),
                    delivery_chunk_bytes);
                if (byte_count == paused_output.size()) {
                    next_paused_output = std::move(paused_output);
                    paused_output.clear();
                }
                else {
                    next_paused_output = paused_output.sliced(0, byte_count);
                    paused_output.remove(0, byte_count);
                }
            }
            else {
                paused_output_delivery_in_progress = false;
                break;
            }
        }

        deliver_output(std::move(next_paused_output));
    }

    output_cv.notify_all();
    on_drain_finished();
}

// Delivers every remaining paused-output byte before the exit report. Waits on the
// output condition variable until no delivery is in progress and the FIFO can be
// drained - it is empty, output is unpaused, or the caller's stop condition
// holds - then replays it in order through the same take/finish pair every other
// drain uses, so callback re-pause requests cannot suppress the exit report
// indefinitely.
//
// `stop_requested` is the same deliberately backend-specific stop predicate
// finish_native_backend_paused_output_delivery takes, and `on_drain_finished` is
// the same per-backend wake hook; neither is interchangeable between backends.
template <typename Stop_requested_fn, typename Deliver_output_fn, typename On_drain_finished_fn>
void drain_native_backend_paused_output_before_exit_report(
    std::mutex&                        mutex,
    std::condition_variable&           output_cv,
    QByteArray&                        paused_output,
    bool&                              output_paused,
    bool&                              paused_output_delivery_in_progress,
    qsizetype                          delivery_chunk_bytes,
    Stop_requested_fn&&                stop_requested,
    Deliver_output_fn&&                deliver_output,
    On_drain_finished_fn&&             on_drain_finished)
{
    for (;;) {
        QByteArray delivery_bytes;
        bool paused_output_delivery_started = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            output_cv.wait(lock, [&] {
                return
                    !paused_output_delivery_in_progress &&
                    (paused_output.isEmpty() ||
                     !output_paused          ||
                     stop_requested());
            });
            if (paused_output.isEmpty()) {
                return;
            }

            paused_output_delivery_started =
                take_native_backend_paused_output_for_delivery_locked(
                    paused_output,
                    delivery_bytes,
                    paused_output_delivery_in_progress,
                    delivery_chunk_bytes);
        }

        if (!delivery_bytes.isEmpty()) {
            deliver_output(std::move(delivery_bytes));
        }
        finish_native_backend_paused_output_delivery(
            mutex,
            output_cv,
            paused_output,
            output_paused,
            paused_output_delivery_in_progress,
            delivery_chunk_bytes,
            paused_output_delivery_started,
            stop_requested,
            deliver_output,
            on_drain_finished);
    }
}

void report_native_backend_exit(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_exit_reason               reason,
    int                                exit_code);

// Non-owning view of the exit-publication state a native backend keeps. The fields
// stay members of the backend and stay under the backend's own mutex: the once-flag
// check, the stop-flag sets, and - on Windows - the exit-reason resolution all run
// under that single lock, and the callback snapshot is a fresh locked read of the
// same state the shutdown paths clear. A registry with a lock of its own would
// change what those decisions observe.
struct native_backend_exit_publication_state_t
{
    std::mutex&                 mutex;
    std::condition_variable&    output_cv;
    std::condition_variable&    write_cv;
    Terminal_backend_callbacks& callbacks;
    bool&                       exit_reported;
    bool&                       running;
    bool&                       stopping;
    bool&                       output_paused;
};

// Publishes the child exit exactly once. Under the backend mutex: returns without
// reporting when the exit was already published; otherwise resolves the reported
// reason through `resolve_reason_locked`, sets the once-flag and the stop flags,
// and runs `on_exit_flags_claim`. After the mutex is released: wakes the output
// and write waiters, runs the action the claim returned, snapshots the callbacks
// under a fresh lock, and reports the exit through them.
//
// `resolve_reason_locked` runs under the same lock as the once-flag check and is
// deliberately backend-specific. Windows resolves the reason there: an explicit
// override wins, then a pending interrupt delivery whose exit code matches. POSIX
// pre-resolves its reason from the wait status BEFORE entering the once-section
// and passes the resolved reason through, so its hook is the identity. The two
// placements are load-bearing and not interchangeable, and the shared flow does
// not pick one.
//
// `on_exit_flags_claim` runs under the same lock after the shared stop flags are
// set and returns the action to run after the waiters are woken - the claim-under-
// the-lock, act-after-unlock shape the public-call guard uses. POSIX claims its
// extra stop flag there and wakes its I/O threads from the returned action;
// Windows passes Native_backend_empty_claim because it has neither.
template <typename Resolve_reason_fn, typename On_exit_flags_claim_fn>
void report_native_backend_exit_once(
    native_backend_exit_publication_state_t exit_publication,
    Terminal_exit_reason                    default_reason,
    int                                     exit_code,
    Resolve_reason_fn&&                     resolve_reason_locked,
    On_exit_flags_claim_fn&&                on_exit_flags_claim)
{
    Terminal_exit_reason reason = default_reason;
    std::optional<decltype(on_exit_flags_claim())> wake_after_publication;
    {
        std::lock_guard<std::mutex> lock(exit_publication.mutex);
        if (exit_publication.exit_reported) {
            return;
        }

        reason = resolve_reason_locked(reason);

        exit_publication.exit_reported = true;
        exit_publication.running       = false;
        exit_publication.stopping      = true;
        exit_publication.output_paused = false;
        wake_after_publication.emplace(on_exit_flags_claim());
    }

    exit_publication.output_cv.notify_all();
    exit_publication.write_cv.notify_all();
    if (wake_after_publication.has_value()) {
        (*wake_after_publication)();
    }

    Terminal_backend_callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(exit_publication.mutex);
        callbacks = exit_publication.callbacks;
    }
    report_native_backend_exit(callbacks, reason, exit_code);
}

// Sets the reader-finished flag under the backend mutex and wakes the reader
// waiters after the lock is released, so a waiter cannot wake into a not-yet-
// flagged state. This was both backends' mark_reader_finished member, verbatim.
void mark_native_backend_reader_finished(
    std::mutex&                        mutex,
    std::condition_variable&           reader_cv,
    bool&                              reader_finished);

// Blocks until the reader thread has flagged itself finished. This was both
// backends' wait_for_reader_finished member, verbatim.
void wait_for_native_backend_reader_finished(
    std::mutex&                        mutex,
    std::condition_variable&           reader_cv,
    bool&                              reader_finished);

// Bounded form of the reader-finished wait; only the ConPTY backend bounds its
// wait (its reader close grace), so POSIX has no caller for this one.
bool native_backend_reader_finished_within(
    std::mutex&                        mutex,
    std::condition_variable&           reader_cv,
    bool&                              reader_finished,
    std::chrono::milliseconds          timeout);

// Locked read of the backend's stopping flag. This was both backends'
// stopping() accessor, verbatim.
bool is_native_backend_stopping(
    std::mutex&                        mutex,
    bool&                              stopping);

// Joins (or detaches, when called from the thread itself) the four worker
// threads every native backend spawns. This was both backends' join_threads
// member, verbatim.
void join_native_backend_threads(
    std::thread&                       reader_thread,
    std::thread&                       writer_thread,
    std::thread&                       wait_thread,
    std::thread&                       termination_thread);

void join_or_detach_native_backend_thread(
    std::thread&                       thread);

}
