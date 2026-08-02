#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <QByteArray>
#include <QString>
#include <QStringView>
#include <condition_variable>
#include <cstddef>
#include <latch>
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

void report_native_backend_error(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

void deliver_native_backend_output(
    const Terminal_backend_callbacks&  callbacks,
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

void report_native_backend_exit(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_exit_reason               reason,
    int                                exit_code);

void join_or_detach_native_backend_thread(
    std::thread&                       thread);

}
