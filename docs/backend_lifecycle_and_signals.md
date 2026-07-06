# Backend Lifecycle And Signals

This document is the per-platform contract for the native terminal backends:
the Windows ConPTY backend in `src/windows_conpty_backend.cpp` and the POSIX
forkpty backend in `src/posix_pty_backend.cpp`. Both implement the
`Terminal_backend` interface in
`include/vnm_terminal/internal/backend_contract.h` and share start validation,
launch environment construction, write-queue accounting, callback delivery, and
thread-join helpers in `src/native_backend_io_core.*`. Backend-specific
pause/backpressure paths are documented below.

A backend hosts the child process, owns the OS handles and worker threads, and
reports output bytes, process exit, and errors through
`Terminal_backend_callbacks`. It does not parse terminal bytes, mutate screen
state, or touch Qt Scene Graph state. `Terminal_session` is the ordering
boundary that ingests those callbacks; see
[Architecture](architecture.md) for the surrounding component map.

The two backends are documented side by side. Where a behavior is specific to
one platform, the platform is named.

## Startup, Process Group, And Session

Both backends share one start gate. `validate_native_backend_start_preconditions`
in `src/native_backend_io_core.cpp` validates the callbacks and launch config,
enforces start-once semantics, builds the effective launch environment, and
checks that the working directory exists. A backend that has already started,
already attempted a start, or has a start in progress rejects a second start
with `START_FAILED`. Both backends start three worker threads on success: a
reader, a writer, and a process-wait thread.

### POSIX session and process group

The POSIX child always becomes its own session and process-group leader.
`forkpty` runs `login_tty` in the child, which calls `setsid()`; the child
branch then reasserts its own process group with `setpgid(0, 0)` before
`execve`. The `setpgid` result is intentionally unchecked because `setsid` has
already established the group and the child is about to exec.

Because the child is always its own session/process-group leader, the two
`Terminal_process_group_policy` values (`BACKEND_DEFAULT` and
`CREATE_NEW_SESSION`) collapse to the same new-session behavior on POSIX. The
backend records the child's process group as the child pid directly rather than
reading `getpgid(child_pid)`, which could otherwise race the child running
`setsid`.

### Windows session and process group

The ConPTY backend creates the pseudoconsole, builds the child command line and
environment block, and launches the child with `CreateProcessW`. When the
launch config requests `Terminal_process_group_policy::CREATE_NEW_SESSION`, the
backend adds `CREATE_NEW_PROCESS_GROUP` to the creation flags; this is the
Windows mapping of the new-session request.

### Early-child setup-failure cleanup

When PTY setup fails after `forkpty` but before the session becomes live, the
POSIX backend reaps the child through one `terminate_unstarted_child(pid, bool)`
helper whose signal target depends on the child's lifecycle stage:

- Before exec is confirmed, the child may not yet have run
  `login_tty` -> `setsid` (or the explicit `setpgid`), so its own process group
  is not reliably its pid. The helper signals the single child pid.
- After exec is confirmed (the startup-error pipe closed without delivering an
  errno), the child is its own session/process-group leader, so the helper
  signals the whole group (`-pid`) to also reap any descendants it spawned.

This lifecycle-dependent targeting is intentional, not an inconsistency.

## Output Read, Pause, And Backpressure

Output bytes flow from each reader thread through a backend-local
paused-output decision under the backend mutex. That decision either appends to
a paused-output buffer (when output is paused or buffered bytes are already
pending and the backend can still buffer) or delivers the bytes to the
`output_received` callback. Both native backends derive their read chunk,
paused-buffer high-water, and paused-replay chunk sizes from
`derive_native_backend_output_delivery_limits` in
`src/native_backend_io_core.*`. POSIX uses the shared
`deliver_or_buffer_native_backend_output` helper in
`src/native_backend_io_core.h`; ConPTY implements the same FIFO/backpressure
contract in its local pipe-drain path. The session activates backpressure
through `Terminal_backend::set_output_paused`; the two backends behave
differently while paused, and that difference is the central cross-platform
contract here.

### Windows: bounded drain while paused

The ConPTY `read_loop` keeps the output pipe draining while output is paused,
but only until the paused buffer reaches the derived high-water mark. ConPTY
also applies its local 64 KiB paused-output ceiling to that derived high-water
mark and to the derived paused-replay chunk. With the default session queue
limits, this preserves the existing 64 KiB paused-output cap; smaller configured
delivery limits can lower the effective ConPTY read request, paused-buffer
high-water mark, and replay chunk. Once the cap is reached, the reader parks on
its output condition variable until output resumes or shutdown begins.
On resume, `set_output_paused(false)` delivers buffered bytes through the same
paused-output delivery path in chunks no larger than the effective replay
chunk, then wakes the reader. If a callback re-pauses output during a normal
resume, delivery stops after the current chunk. If a buffered delivery or
buffered bytes are already pending, newly read bytes append behind that buffer
up to the cap instead of bypassing it. During exit drain, buffered tail output
is delivered in order before the process exit is reported, and callback
re-pause requests are ignored so the `process_exited` callback cannot be
suppressed indefinitely.

### POSIX: drain while paused up to a high-water mark

The POSIX reader deliberately keeps draining the master while output is paused,
buffering the bytes application-side in the paused buffer rather than leaving
them in the kernel. It only parks for backpressure once the buffer reaches
the high-water mark derived from the owning session's output callback queue:
hard limit minus high-water limit minus one maximum backend read callback. With
the default queue limits this is 176 KiB (256 KiB hard limit, minus 64 KiB
high-water, minus a 16 KiB maximum native read callback), and paused replay is
chunked to the default 64 KiB high-water threshold. This is required
because macOS flow-controls a slave write almost immediately when the master is
not being read: a child that emits a small final line and exits while output is
paused would otherwise block in `write()` forever and never exit, which would
break exit-drain delivery. Reserving one backend read callback under the
session hard limit prevents exit-drain from enqueueing more callback output than
that session can hold before it drains, even when the threshold-crossing
callback overshoots high-water. If a buffered delivery or buffered bytes are
already pending, newly read bytes append behind that buffer up to the cap
instead of bypassing it. During running-state resume, the session may re-pause
between bounded chunks. Once the direct child has exited or termination has
begun, pause requests are accepted but do not latch; the wait thread reports
exit after the paused buffer is empty and no paused-output delivery is active.

## Write Path

A user write, paste write, terminal reply, or interrupt byte is appended to a
bounded write queue and drained by the writer thread. The queue is bounded by
`k_native_backend_max_queued_write_bytes` (1 MiB), accounted through
`native_backend_write_queue_can_accept`. A write whose bytes would exceed the
limit, or that arrives when the backend is not writable (not running, stopping,
or the writer has already failed), is rejected with `WRITE_FAILED`. An empty
write is also rejected.

The writer drains queued entries in order. The POSIX writer issues a
non-blocking `write()` to the master and treats the write itself as the
writability test; on `EAGAIN` it waits on its wake pipe (never polling the
master for `POLLOUT`) and retries. The ConPTY writer issues `WriteFile` against
the input pipe and loops until the chunk is fully written. A write that fails
while the backend is not stopping reports `WRITE_FAILED` and marks the writer
failed, which closes the write side.

## Interrupt (Ctrl+C)

The two backends deliver interrupt differently because POSIX has a signal
mechanism and ConPTY does not.

### POSIX interrupt

`interrupt()` reads the master's foreground process group with `tcgetpgrp` and
sends `SIGINT` to that group (`kill(-foreground_pgid, SIGINT)`). A missing
foreground group, a `tcgetpgrp` failure, or a `kill` failure is reported as
`INTERRUPT_FAILED`. No exit-reason override is recorded at request time; the
exit reason is derived from the wait status when the child is reaped (a child
killed by `SIGINT` resolves to `INTERRUPTED`).

### Windows interrupt

ConPTY has no signal channel, so `interrupt()` enqueues a single `\x03` byte on
the write queue. The queued entry does not classify process exit by itself. When
the writer reaches that entry in stream order, it records a narrow in-flight
`INTERRUPTED` override so the process-wait thread can still classify an exit
that races with `WriteFile` completion as interrupted.

After successful delivery, the writer converts the in-flight override into a
pending conventional Ctrl+C exit-code observation. Exit code 130 is then reported
as `INTERRUPTED` unless the backend observes stronger evidence that the child
continued past the interrupt. A successfully completed later non-interrupt write
only arms that clear; the observation is cleared when child output is read during
or after that successful write. `WriteFile` completion alone is not proof that
the child processed the write, and a failed/stopped later write cannot clear the
pending classification. On delivery failure, the writer drops the in-flight
override and surfaces `INTERRUPT_FAILED` while the process is still running.
Queued interrupt entries that the writer never reaches are discarded as ordinary
queued writes and do not affect exit-reason resolution.

## Terminate And Escalation

`terminate()` sets a `TERMINATED` exit-reason override under the mutex, clears
the paused state, wakes the workers, and starts an escalation worker governed by
`Terminal_termination_policy` (a graceful interval followed by a kill interval).

- POSIX escalation sends `SIGTERM` to the process-group signal targets, waits up
  to the graceful interval for them to disappear, then sends `SIGKILL` and waits
  up to the kill interval. A still-active group after forced termination, or a
  signal failure, is reported as `TERMINATE_FAILED`.
- ConPTY escalation waits up to the graceful interval for the process to exit,
  then calls `TerminateJobObject` on the child process job, then waits up to the
  kill interval for the root process to exit. A process still active after
  forced termination, or a `TerminateJobObject` failure followed by a failed
  root-process `TerminateProcess` fallback, is reported as `TERMINATE_FAILED`.

If the escalation worker thread cannot be created, both backends fall back to an
immediate forced kill (`SIGKILL` to the targets / ConPTY job termination with
root-process fallback) and report `TERMINATE_FAILED`.

After the direct child exits and the exit drain completes, the POSIX backend
also sends `SIGKILL` to the child's process group to reap any leftover
descendants (for example a backgrounded process still holding the slave open).
This kill happens after the child's exit is reported, so a descendant remains
observable as alive until the direct child's exit is delivered.

## Exit-Reason Resolution

Exit is reported exactly once. Both backends gate exit reporting on an
exit-reported flag set under the mutex, so the first reporter wins and later
attempts return without delivering a second `process_exited` callback.

The reason is resolved from the recorded override and the observed exit:

- If an exit-reason override is present (`INTERRUPTED` from an in-flight ConPTY
  interrupt write, or `TERMINATED` from a terminate request), that override is
  the reported reason.
- On Windows, if no override is present but a delivered ConPTY interrupt has a
  pending conventional Ctrl+C exit-code observation, exit code 130 reports
  `INTERRUPTED`.
- Otherwise the reason is the natural exit. On POSIX,
  `exit_reason_from_wait_status` maps a `SIGINT` death to `INTERRUPTED`, any
  other signal death to `TERMINATED`, and a normal exit to `EXITED`. On Windows
  a natural exit reports `EXITED`.

The override mechanism exists to resolve the race between an in-flight request
and the process-wait thread observing the child's death: terminate records the
intended reason at request time, while ConPTY interrupt records it only after the
writer reaches the Ctrl+C byte. The ConPTY interrupt path additionally retracts
its in-flight override on delivery failure
(see Interrupt, above) so the override never outlives a failed request while the
process is still running.

## Threading And Ownership Invariant

Backend worker threads only enqueue callbacks; they do not call into
`Terminal_session` terminal-domain state directly. `Terminal_session` is the
ordering boundary. The session installs callbacks built by
`make_backend_callbacks` in `src/terminal_session.cpp`: each `output_received`,
`process_exited`, and `error_reported` invocation enters a lifetime-owned
callback queue and then wakes the owner.

- With `Terminal_session_config::backend_event_notifier` set (the surface
  configuration), the callback only queues the command and wakes the GUI thread;
  the GUI thread later drains the queue in session order. Callback command count
  and output bytes are bounded in this ingress queue because a notifier-driven
  owner may not drain while the GUI thread is busy; output ingress that reaches
  its high-water mark requests backpressure. Tests may omit the notifier, which
  makes callback delivery synchronous through the same session methods.
- `Terminal_session_callback_lifetime::close()` (called from the session
  destructor) stops accepting new callbacks and waits for in-flight callbacks to
  finish before any session member is destroyed. This quiesces backend threads
  against the session before teardown.

Each public session operation drains the backend callback queue first, then
assigns a sequence and processes its own command, so backend output, backend
exit, backend errors, user input, parser replies, and resize are all applied in
one ordered stream. Backend exit is recorded once; a second exit command is
rejected as invalid state.

### Final output drain after process exit

When the process-wait thread reaps the child, the backend delivers any buffered
paused output before reporting exit, so output produced up to the moment of exit
is not lost. Delivery remains FIFO: reader tail bytes append behind existing
paused bytes, and deferred-session backpressure can pause running-state drain
between bounded chunks. Exit-drain itself does not latch new pause requests, so
the process exit callback cannot be lost behind a callback re-pause. The POSIX
backend additionally honors a consistent exit-drain window
(`k_exit_output_drain_timeout`) before finalizing the exit: on Linux the master
stays readable while a descendant holds the slave open, so the reader drains for
the full window; on macOS the master EOFs the instant the
session-leader child exits, so the reader finishes immediately and the wait
thread waits out the remainder of the window so the observable behavior matches
Linux. The drain wait is interruptible by shutdown so teardown is never delayed.

## Teardown

`shutdown()` is idempotent (guarded by a shutdown-started flag). It marks the
backend stopping, clears paused state, detaches the callbacks, wakes the
workers, kills the child (and its process group where established), and joins
the worker threads.

Backend destructors run shutdown inline when no backend-owned stack frame can
still touch the implementation. If destruction is reached from a backend worker
callback, or while a public backend call is still unwinding, the destructor
releases the implementation and defers `shutdown(); delete this;` to a fresh
thread. That keeps worker threads joined by a non-worker thread and keeps the
implementation alive until active public-call frames have returned.

The POSIX teardown ordering is load-bearing on macOS and is therefore explicit:

- The pending process-group `SIGKILL` targets `kill(-pgid)`, which fails with
  `ESRCH` during the brief window after `forkpty` before the child established
  its own group. To guarantee the wait thread's blocking `waitpid` returns, the
  child pid is also signaled directly with `kill(child_pid, SIGKILL)`.
- In ordinary shutdown, the master fd is closed before joining the workers, not
  after. While output is paused, the reader drains only up to the paused-output
  high-water mark and then parks; a high-volume child can still block in
  `write()` to the slave and fail to act on a pending `SIGKILL`. Closing the
  master releases that blocked write (delivering `EIO`/`SIGHUP`) so the kill can
  land and the wait thread can be joined. If the wait thread still needs the
  master for post-exit foreground/process-group cleanup, the close is deferred
  until that cleanup finishes.
- The bounded reader and writer poll intervals (`k_native_master_poll_interval`)
  exist so the I/O threads re-check their stop flags rather than trusting a poll
  on the PTY master, which is unreliable on macOS once the child has exited while
  a descendant still holds the slave open.

The deferred-teardown fallback is leak-not-use-after-free. If the fresh thread
cannot be started, the backend stops accepting callbacks, wakes its workers, and
issues the best termination request still available without joining threads or
deleting the leaked implementation. POSIX signals the known process groups and,
when the direct child has not already been reaped, the child pid. It keeps the
master fd owned by the leaked implementation because worker threads may already
have copied that fd before the fallback decided not to join them.
