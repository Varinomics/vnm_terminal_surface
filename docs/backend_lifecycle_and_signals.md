# Backend Lifecycle And Signals

This document is the per-platform contract for the native terminal backends:
the Windows ConPTY backend in `src/windows_conpty_backend.cpp` and the POSIX
PTY backend in `src/posix_pty_backend.cpp`. Both implement the
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

Startup errors use a flat taxonomy. A structurally invalid or policy-invalid
request reports `INVALID_LAUNCH_CONFIG`. A well-formed executable that cannot be
resolved or admitted reports `START_FAILED`. The dispatch and determinacy fields
describe, independently, whether native dispatch occurred and whether the
outcome is safe to retry; they do not refine or replace the error code.

### POSIX session and process group

The POSIX backend launches `vnm_process_custody_owner` beside its hosting
executable. The helper owns the workload's sole wait and native cleanup; the
backend owns its helper's sole wait and the presentation's master descriptor.
Linux gives the shell a new session and uses the helper's subreaper for tree
cleanup. On macOS, the persistent helper retains the controlling PTY session
and places the shell in its own foreground process group within that session.
Both `Terminal_process_group_policy` values use this isolated terminal session.
Linux also retains a nonblocking, close-on-exec slave obtained through
`TIOCGPTPEER` without acquiring a controlling terminal. Failure to obtain that
peer rejects startup through the existing deferred native-cleanup path.

Darwin workload START requires `Terminal_native_receipt_hook` to acknowledge
retention and persistence of the original helper's identity and status observer.
The hook separately receives exact root birth, root exit, or no-birth evidence;
revoking presentation callbacks does not revoke these native obligations.
The [custody contract](../cpp/process_custody/README.md) defines the helper's
scoped status and the distinction between root and helper observation.

### Windows session and process group

The ConPTY backend creates the pseudoconsole, builds the child command line and
environment block, and launches the child with `CreateProcessW`. When the
launch config requests `Terminal_process_group_policy::CREATE_NEW_SESSION`, the
backend adds `CREATE_NEW_PROCESS_GROUP` to the creation flags; this is the
Windows mapping of the new-session request.

### Early-child setup-failure cleanup

The POSIX helper withholds execution until it retains the child's native
identity and confirms that START remains admitted. A rejected gate permits
no-workload evidence only when no child was born or the still-gated child was
positively reaped. An admitted or uncertain birth retains its exact identity
and root-exit evidence separately from the startup rejection. Startup failure
never fabricates group settlement. Native cleanup continues independently of
the bounded startup return.

## Output Read, Pause, And Backpressure

Output bytes flow from each reader thread through a backend-local
paused-output decision under the backend mutex. That decision either appends to
a paused-output buffer (when output is paused or buffered bytes are already
pending and the backend can still buffer) or delivers the bytes to the
`output_received` callback. Both native backends derive their read chunk,
paused-buffer high-water, and paused-replay chunk sizes from
`derive_native_backend_output_delivery_limits` in
`src/native_backend_io_core.*`. Both backends route the paused-output decision
through the shared `deliver_or_buffer_native_backend_output` in
`src/native_backend_io_core.h`, each injecting its own admission predicate, and
both replay the paused buffer through the shared
`take_native_backend_paused_output_for_delivery_locked`,
`finish_native_backend_paused_output_delivery`, and
`drain_native_backend_paused_output_before_exit_report`, each injecting its own
stop predicate and post-drain wake hook; the two backends' predicates are
deliberately different and are spelled out at each injection site. The session
activates backpressure through `Terminal_backend::set_output_paused`; the two
backends behave differently while paused, and that difference is the central
cross-platform contract here.

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
write is also rejected. Once the wait thread has observed the child's exit, a
rejection sets `child_exited`: no reader remains for the bytes. The wait thread
stops input as soon as it observes the exit, before it reports the exit, so a
write can be refused this way while the session still believes the child is
running. No other rejection sets it, including a stop whose exit has not been
observed and the POSIX wait's loss of its exit observation.

The writer drains queued entries in order. The POSIX writer issues a
non-blocking `write()` to the master and treats the write itself as the
writability test; on `EAGAIN` it waits on its wake pipe (never polling the
master for `POLLOUT`) and retries. The ConPTY writer issues `WriteFile` against
the input pipe and loops until the chunk is fully written. A write that fails
while the backend is not stopping reports `WRITE_FAILED` and marks the writer
failed, which closes the write side.

## Terminal Size And Cell Pixels

The session hands the backend the grid at start (`initial_grid_size`) and on
every resize (`Terminal_backend_resize_request`), and the display's cell size in
device pixels through `set_cell_pixel_size` whenever it changes, before start
included. The surface reports that cell before it starts the backend.

### POSIX winsize

The POSIX backend writes both into the PTY `winsize`: `ws_row` and `ws_col`
carry the grid, and `ws_xpixel` and `ws_ypixel` carry the text area in pixels,
columns times the cell width and rows times the cell height. A pixel field is
zero while no cell size is known or when the product does not fit the field.
The spawn `winsize` already has pixels; on Linux the process-custody owner
creates the PTY with them. A resize sets the new grid with the current pixels.
A pixel-only change, such as a font change that keeps the grid or a move to a
screen with another device pixel ratio, reads the grid back with `TIOCGWINSZ`
and rewrites the pixel fields with `TIOCSWINSZ`, without a resize transaction.
The kernel signals `SIGWINCH` to the foreground process group only when the
stored `winsize` changes. A font change that also moves the grid updates the
pixels first and then resizes, so the child can observe two changes.

### Windows cell geometry

`ResizePseudoConsole` takes a character grid only. The ConPTY backend declares
a fixed 10x20 cell through `fixed_cell_pixel_size`: OpenConsole places sixel
images on that cell and moves its own cursor by it, whatever the host renders.
The session answers CSI 14 t and CSI 16 t with that cell instead of the
display's, so every Windows client reads virtual pixel sizes, and it does not
forward the display's cell to the backend. OpenConsole also keeps the text an
image covers, which the model erases, so a ConPTY repaint prints that text
again and the model then clears the image pixels under it, as printing does
per `dcs-sixel-product-decisions` in the sequence matrix.

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

The code-130 classification is necessarily best effort when a later input write
overlaps final child output and exit. ConPTY reports that `WriteFile` reached its
input pipe but provides no acknowledgement that the child consumed the bytes. A
successful later write and output can therefore clear the pending interrupt
observation before the process-wait thread marks the session as stopping, even
when the child never reads that write, and the exit is reported as `EXITED` with
code 130. If the later write fails or the process-wait thread marks the session
as stopping first, the same exit is reported as `INTERRUPTED` with code 130. The
ambiguity affects the reason metadata only; exit delivery, output drain, and the
native exit code are unchanged.

## Terminate And Escalation

`terminate()` sets a `TERMINATED` exit-reason override under the mutex, clears
the paused state, and wakes the workers.

- POSIX sends STOP with the requested grace interval to its persistent helper.
  The helper attempts graceful termination, then forced cleanup. On macOS it
  signals ordinary foreground and background groups through the retained tty
  and keeps the original root's sole wait. After that wait is collected, a tty
  membership snapshot naming only the retained helper confirms scoped cleanup.
  Bounded exhaustion while inert members remain produces terminal
  `UNCONFIRMED`; failure or timeout never proves emptiness. A live root remains
  pending. A failed STOP send closes the sending direction so control EOF
  requests the same native cleanup.
- ConPTY escalation waits up to the graceful interval for the process to exit,
  then calls `TerminateJobObject` on the child process job, then waits up to the
  kill interval for the root process to exit. A process still active after
  forced termination, or a `TerminateJobObject` failure followed by a failed
  root-process `TerminateProcess` fallback, is reported as `TERMINATE_FAILED`.

The POSIX helper starts cleanup on root exit independently of the presentation's
output drain and exit callback. Root exit does not establish job emptiness, and
callback delivery does not authorize releasing a native cleanup obligation.

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

Synchronous Surface input and geometry operations capture the callback enqueue
epoch when the event enters the GUI thread. They apply callbacks through that
fixed frontier before route decisions, then admit their input or resize command
ahead of callbacks that arrived later. Direct session input and resize calls
capture their own frontier. Resize policy setters capture before changing the
policy, apply the new policy, and then process callbacks through the captured
epoch; arbitration settlement retains its request-id and held-tail order.
Later callbacks remain queued for a following event or an asynchronous drain.
The session still records commands in one ordered stream. Backend exit is
recorded once; a second exit command is rejected as invalid state.

### Final output drain after process exit

When the process-wait thread observes the original root's exit, the backend
delivers buffered paused output before reporting exit. Delivery remains FIFO:
reader tail bytes append behind existing paused bytes, and deferred-session
backpressure can pause running-state drain between bounded chunks. Exit drain
does not latch new pause requests, so the exit callback cannot remain trapped
behind a callback re-pause. The POSIX backend honors the 250 ms
`k_exit_output_drain_timeout` even if PTY EOF finishes the reader early. At that
boundary it stops admitting additional output to the final drain, then delivers
the accepted tail in the existing bounded chunks. Linux stops slave output and
drains until native read/poll reports no input, including pending flip-buffer
bytes. Darwin resumes flow-stopped output and snapshots the queued FIFO prefix;
later producer writes cannot extend that prefix. Synchronous callback duration
still contributes to delivery time. Shutdown can interrupt delivery without
interrupting the helper's native cleanup.

After macOS cleanup settlement the helper retains its controlling terminal until
the client retires output through control-channel EOF. `finish()` half-closes the
client's sending direction before closing and collecting the helper, so a
duplicate client endpoint cannot delay normal retirement. Client abandonment
also retires the terminal when its last endpoint closes; an endpoint leaked to
another live process can postpone that EOF. The helper then revokes its retained
terminal so later producer output cannot block the session leader's native exit.
Unexpected drain-control failures
use the existing `READ_FAILED` path and do not establish complete delivery.

## Teardown

`shutdown()` is idempotent. It marks the backend stopping, clears paused state,
revokes presentation callbacks, wakes workers, and requests native cleanup.
A cleanup reservation created before native birth retains the implementation
until active public calls and worker threads have drained, including destruction
from inside a callback. No cleanup thread allocation is required after birth.

On POSIX, shutdown sends forced STOP to the helper and joins the reader/writer/wait
workers while retaining the presentation's master descriptor. It drains the
helper's remaining receipts and collects that helper's sole wait before closing
the master and any Linux drain peer. Retaining the descriptors until workers
finish prevents their pending I/O from reaching an unrelated file that reuses
its number. The helper retains
its own PTY descriptors throughout native cleanup. Bounded I/O polling allows
presentation threads to re-check their stop flags even when the slave remains
open. Helper death without an admitted terminal outcome leaves native cleanup
unconfirmed; presentation teardown and root exit never replace that outcome.
