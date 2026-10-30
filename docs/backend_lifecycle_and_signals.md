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

The built-in POSIX process owner supplies the PTY and child lifecycle for
ordinary request-only terminal launch. The public library does not require a
host-provided helper or a separate process-management repository.

The child starts in its own session and process group. The owner
retains the original group identity until its sole reap and keeps terminal
access for foreground signaling. Stop runs asynchronously, allowing the grace
interval before forced termination. Its scope is the exact root, original group,
and retained terminal foreground; it does not promise cleanup or recovery of
every descendant or an escaped process group.

On Linux kernels without retained process-group signaling, foreground cleanup
retains each observed member's native identity and signals that identity. This
is observed-member cleanup, not atomic containment of the whole process family.
The 256-member batch bounds simultaneous retention, not the terminal's group
size: subsequent scans reuse retired slots. Observed members that do not fit in
the current batch prevent an empty or settled result until they are handled.

Hosts can supply `Terminal_process_owner` from the installed, Qt-free
`vnm_terminal/terminal_process_owner.h` contract as the optional second argument
to `start_terminal(request, owner)`. That owner controls process birth, wait,
termination, and cleanup while the same POSIX backend owns terminal I/O and
resize. The backend selects the owner before birth and never retries a failed
supplied owner through its built-in implementation. Windows rejects a nonnull
owner before birth.

The owner contract transfers the PTY master descriptor only on successful
`launch()`. Every process born during a rejected launch or exception remains
that owner's cleanup obligation, so it must prepare cleanup resources before
birth. One backend worker calls `wait_root()` for exact root-exit observation.
`stop()` may run concurrently and must not block for its grace interval; a
failed stop stays committed. `finish()` must be idempotent and retire the
owner's waits after the sole waiter has finished or observed exit. `confirmed()`
describes the selected owner's actual cleanup obligations, not a guessed exit.

### Windows session and process group

The ConPTY backend creates the pseudoconsole, builds the child command line and
environment block, and launches the child with `CreateProcessW`. When the
launch config requests `Terminal_process_group_policy::CREATE_NEW_SESSION`, the
backend adds `CREATE_NEW_PROCESS_GROUP` to the creation flags; this is the
Windows mapping of the new-session request.

The root is created with `CREATE_SUSPENDED`, assigned to the backend's
kill-on-close Job with `AssignProcessToJobObject`, and resumed only after
assignment succeeds. This sequence preserves Store PowerShell compatibility:
creation-time `PROC_THREAD_ATTRIBUTE_JOB_LIST` admission can reject that MSIX
executable with `0xC0070005`, depending on the caller's context (the change in
commit `21187ee` was reverted).

The creation-to-assignment gap is an accepted containment limit. Between
`CreateProcessW` returning and `AssignProcessToJobObject`, the launching thread
can be preempted or suspended; native-resource adoption also takes `m_mutex`.
The interval has no fixed bound. If the host dies in it, the root can survive
as a suspended process that has run no code. Removing the gap completely would
require a different creation path; the backend deliberately keeps the single
Store-compatible path.

The conditional native check is CTest's
`vnm_terminal_windows_conpty_store_powershell` entry, or the backend test
executable's `--store-powershell-root` mode. It discovers the current user's
registered `Microsoft.PowerShell` package rather than resolving `pwsh` on PATH,
then checks the live root's package identity, image path and membership in the
terminal Job through the backend's retained process handle. Output and a natural
exit are also required. Absence of a registered package returns skip code 77;
an installed package that fails to start or lacks the expected identity fails.

Run this check outside terminal sessions and outside every Job. The check
returns 77 when the test host is already in a Job, because that context can mask
the creation-time admission regression. Windows CI tries Microsoft's published
PowerShell MSIX and reports unavailable coverage separately from a pass. It
exports `conpty-native-check-windows`, containing the test executable and its Qt,
MSVC and ConPTY runtime. After extracting the artifact, run:

```powershell
./run_store_powershell.ps1 -EvidenceDirectory C:/temp/store-root-check
./run_store_powershell.ps1 -Scenario packaged-descendants -EvidenceDirectory C:/temp/packaged-descendants-check
```

The script uses `Win32_Process.Create` to launch outside the invoking terminal's
Job and records the outcome in a fresh evidence directory. The root check also
verifies that launch context. The packaged-descendant check exercises a detached
MSIX grandchild through natural root exit, termination, destruction, and CPU
retention after child exit. No local build or package installation is performed
by this script.

### Early-child setup-failure cleanup

The selected process owner reports whether a child was born and whether the
start outcome is determinate. A rejected start does not establish that native
dispatch never occurred or that every descendant has stopped. The caller must
use the result's dispatch and determinacy fields before deciding whether a new
request is safe; a dispatched indeterminate start must not be retried.

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

On Linux, the backend retains a nonblocking, close-on-exec slave obtained through
`TIOCGPTPEER` without acquiring a controlling terminal. Failure to obtain that
drain peer rejects startup through the existing deferred cleanup path.

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
The initial PTY `winsize` includes these pixels. A resize sets the new grid
with the current pixels.
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

- POSIX asks the selected process owner to terminate using the configured grace
  and kill intervals. The owner controls signal targets and collects the root's
  wait; the backend continues to manage terminal output and callback delivery.
- ConPTY escalation waits up to the graceful interval for the process to exit,
  then calls `TerminateJobObject` on the child process job, then waits up to the
  kill interval for the root process to exit. A process still active after
  forced termination, or a `TerminateJobObject` failure followed by a failed
  root-process `TerminateProcess` fallback, is reported as `TERMINATE_FAILED`.

Root exit and descendant cleanup are different outcomes. A process-exit
callback reports the root's state; it does not establish transitive termination
of every process that the terminal workload may have created. A supplied owner
documents any stronger cleanup scope it provides.

On Windows, one process-wide scanner checks running ConPTY terminals at 100 ms
intervals. Packaged programs can escape Job inheritance even without a breakaway
flag. The scanner adopts provable descendants into separate, initially empty
kill-on-close Jobs. A packaged program launched from the terminal, including one
with its own console, therefore closes with that terminal when discovery succeeds.
Normal root exit, explicit termination, and backend destruction all stop discovery,
terminate every owned Job, and require those Jobs to report zero active processes
before native cleanup settles. A failed observation retains the exact Job handle
for the existing deferred cleanup owner; it never counts as successful settlement.

Discovery uses two process snapshots with retained parent and child handles.
Each scan pass shares those snapshots across all running terminals: one first
snapshot, and a second only when at least one family has candidate handles.
The global snapshots run outside family ownership locks. Each family rechecks
that discovery is enabled before committing adoption, so cleanup can stop it
between snapshots without admitting a process after its termination request.
The parent must belong to an owned Job, both instances must still be running
after the second snapshot, that snapshot must name the same parent PID, and the
child must have a strictly later creation time. Equal times are unproven. The
child must be proven absent from every owned Job before assignment; a failed
membership query is unproven. These checks prevent PID reuse from capturing an
unrelated process and prevent repeat adoption or nesting among owned Jobs.
Children escaping from adoption Jobs can themselves be adopted on later scans.

This is best-effort discovery. A parent that exits between scans cannot establish
the relationship; short-lived children and processes that cannot be opened may
remain outside custody. Windows also allows a creator with the required rights
to name another process through `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`; such a
process is treated as that named parent's child. The scanner does not establish
transitive cleanup of processes it never observed or could not prove.

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
changing the selected owner's native cleanup obligation.

On Darwin, the default child's session-leader exit revokes its controlling
terminal. Normal PTY hangup/reset and tail output during an explicit graceful
stop are supported. Continuing an external writer after root exit, resetting
then writing after that exit, and preserving cleanup output after natural root
exit require an owner that retains the controlling session; the default owner
does not guarantee those post-exit behaviors.

The default Darwin owner checks for queued output every 250 ms while waiting
for exit or stop. It queries process state with `sysctl` only when output is
queued, and resumes flow-stopped output only when `P_WEXIT` confirms committed
exit. Active cleanup observes at 10 ms intervals. These are lifecycle and PTY
flow-control checks, separate from CPU sampling.

## Teardown

`shutdown()` is idempotent. It marks the backend stopping, clears paused state,
revokes presentation callbacks, wakes workers, and requests native cleanup.
A cleanup reservation created before native birth retains the implementation
until active public calls and worker threads have drained, including destruction
from inside a callback. No cleanup thread allocation is required after birth.

On POSIX, shutdown requests termination through the selected owner and joins
the reader, writer, and wait workers while retaining their descriptors. Keeping
those descriptors until workers finish prevents pending I/O from reaching an
unrelated file that reuses its number. The owner then completes its lifecycle
retirement. Native process cleanup guarantees belong to that owner; presentation
teardown and a root-exit callback do not extend its scope.

## CPU Accounting

`VNM_TerminalSurface::process_cpu_time()` returns an optional cumulative sample.
The default POSIX owner reports CPU accounting unavailable. A supplied owner
can provide a sample with an explicit coverage scope. The Windows native backend
sums its root and adoption Jobs, which do not nest within one another, including
CPU credited to exited members. Before observing an escape, it reports
`WORKLOAD_FAMILY` for the root Job's members; that is a membership scope, not a
guarantee that every descendant is included. Once an escape is proven, or scanner
resource failure prevents discovery, its scope becomes
`OBSERVED_FAMILY`: other escaped processes may have been inaccessible or missed
between scans. All cumulative CPU reported by Windows is preserved, including
CPU Windows credits when assigning a process that has already run. No
pre-adoption subtraction is made. Empty adoption Jobs are retired after their
final CPU totals are retained; those totals remain available until backend
retirement. Process snapshots include live members of all retained owned Jobs.

`OBSERVED_FAMILY` identifies best-effort root and descendant observations and
can miss short-lived or detached children and work outside the observation
window. `WORKLOAD_FAMILY` identifies native workload accounting. Query failure
remains unavailable rather than becoming an idle sample. Counters from different
launches are not comparable, and the online logical processor count describes
the machine rather than an affinity or quota limit.

The surface API owns no sampling timer. Hosts choose whether and how often to
query; an optional owner's documentation defines its query cost and scope limits.
