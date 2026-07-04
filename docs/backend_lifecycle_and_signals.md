# Backend Lifecycle And Signals

This document is the per-platform contract for the native terminal backends:
the Windows ConPTY backend in `src/windows_conpty_backend.cpp` and the POSIX
forkpty backend in `src/posix_pty_backend.cpp`. Both implement the
`Terminal_backend` interface in
`include/vnm_terminal/internal/backend_contract.h` and share the read, write,
and deliver-or-buffer plumbing in `src/native_backend_io_core.*`.

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

Output bytes flow from the reader thread through
`deliver_or_buffer_native_backend_output` in `src/native_backend_io_core.h`.
Under the backend mutex, that helper either appends to a paused-output buffer
(when output is paused and a backend-supplied `can_buffer` predicate allows it)
or delivers the bytes to the `output_received` callback. The session activates
backpressure through `Terminal_backend::set_output_paused`; the two backends
behave differently while paused, and that difference is the central
cross-platform contract here.

### Windows: park while paused

The ConPTY `read_loop` parks on its output condition variable while output is
paused and does not read. As a result the paused buffer holds at most the
single in-flight chunk captured when a pause races an already-issued `ReadFile`,
and that chunk is delivered when output resumes. The paused buffer cannot grow
unbounded, so the ConPTY `can_buffer` predicate is unconditionally true and no
byte cap is applied. On resume, `set_output_paused(false)` delivers any buffered
chunk and wakes the reader.

### POSIX: drain while paused up to a high-water mark

The POSIX reader deliberately keeps draining the master while output is paused,
buffering the bytes application-side in the paused buffer rather than leaving
them in the kernel. It only parks for backpressure once the buffer reaches
`k_paused_output_high_watermark_bytes` (1 MiB). This is required because macOS
flow-controls a slave write almost immediately when the master is not being
read: a child that emits a final line and exits while output is paused would
otherwise block in `write()` forever and never exit, which would break
exit-drain delivery. Draining application-side lets small final output through
while still applying backpressure for genuinely high-volume output. The POSIX
`can_buffer` predicate stops buffering once the process is stopping, so
shutdown-time output is delivered rather than accumulated.

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
the write queue. Because the process-wait thread can observe the child's exit
before the writer's delivery callback returns, `interrupt()` optimistically
records an `INTERRUPTED` exit-reason override at request time so that an exit
observed during interrupt delivery still classifies as interrupted. The write
queue entry is tagged so the writer can confirm or retract that classification:

- On successful delivery, the writer reasserts the `INTERRUPTED` override (only
  while the backend is not stopping).
- On delivery failure, the writer drops the optimistic `INTERRUPTED` override
  and surfaces `INTERRUPT_FAILED` -- but only while the process is still running
  (guarded by the exit-reported flag under the mutex). If the child has already
  exited and been classified, that classification stands, because the exit may
  well have been the interrupt.

This prevents a later natural exit from being misreported as interrupted when
the interrupt byte never reached the child.

## Terminate And Escalation

`terminate()` sets a `TERMINATED` exit-reason override under the mutex, clears
the paused state, wakes the workers, and starts an escalation worker governed by
`Terminal_termination_policy` (a graceful interval followed by a kill interval).

- POSIX escalation sends `SIGTERM` to the process-group signal targets, waits up
  to the graceful interval for them to disappear, then sends `SIGKILL` and waits
  up to the kill interval. A still-active group after forced termination, or a
  signal failure, is reported as `TERMINATE_FAILED`.
- ConPTY escalation waits up to the graceful interval for the process to exit,
  then calls `TerminateProcess`, then waits up to the kill interval. A process
  still active after forced termination, or a `TerminateProcess` failure, is
  reported as `TERMINATE_FAILED`.

If the escalation worker thread cannot be created, both backends fall back to an
immediate forced kill (`SIGKILL` to the targets / `TerminateProcess`) and report
`TERMINATE_FAILED`.

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

- If an exit-reason override is present (`INTERRUPTED` from a confirmed/optimistic
  ConPTY interrupt, or `TERMINATED` from a terminate request), that override is
  the reported reason.
- Otherwise the reason is the natural exit. On POSIX,
  `exit_reason_from_wait_status` maps a `SIGINT` death to `INTERRUPTED`, any
  other signal death to `TERMINATED`, and a normal exit to `EXITED`. On Windows
  a natural exit reports `EXITED`.

The override mechanism exists to resolve the race between a request (interrupt
or terminate) and the process-wait thread observing the child's death: the
request records the intended reason before the OS-level effect is confirmed, so
an exit observed in that window is still classified by intent. The ConPTY
interrupt path additionally retracts its optimistic override on delivery failure
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
is not lost. The POSIX backend additionally honors a consistent exit-drain
window (`k_exit_output_drain_timeout`) before finalizing the exit: on Linux the
master stays readable while a descendant holds the slave open, so the reader
drains for the full window; on macOS the master EOFs the instant the
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
  after. With output paused the master is not drained, so a child can block in
  `write()` to the slave and cannot act on a pending `SIGKILL`. Closing the
  master releases that write (delivering `EIO`/`SIGHUP`) so the kill can land
  and the wait thread can be joined. If the wait thread still needs the master
  for post-exit foreground/process-group cleanup, the close is deferred until
  that cleanup finishes.
- The bounded reader and writer poll intervals (`k_native_master_poll_interval`)
  exist so the I/O threads re-check their stop flags rather than trusting a poll
  on the PTY master, which is unreliable on macOS once the child has exited while
  a descendant still holds the slave open.

The deferred-teardown fallback is leak-not-use-after-free. If the fresh thread
cannot be started, the backend stops accepting callbacks, wakes its workers, and
issues the best termination request still available without joining threads or
deleting the leaked implementation. POSIX signals the known process groups and,
when the direct child has not already been reaped, the child pid. It keeps the
master fd open while a public backend call that may have copied it is still
active, then closes it as that call unwinds. If the wait thread still needs the
master for post-exit process-group and foreground-group cleanup, that cleanup
runs before the pending close.
