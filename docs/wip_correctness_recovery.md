# WIP: terminal responsiveness and termination correctness

This continuation document is explicitly requested by the user. It is the
authoritative task state for `vnm_terminal_surface` and its sibling
`vnm_terminal`; update it when implementation or verification changes.

## Objective and evidence

Implement the sound corrections from the single external correctness response,
including the broader native-operation, GUI output-processing, and capture I/O
fixes. Preserve input semantics, process ownership, and truthful completion.
The user observed three idle terminal windows disappear on mouseover after a
suspected Windows GPU reset while one active window survived. That observation
does **not** establish a GPU reset or identify the termination cause. Do not
report these corrections as proof that the observed incident is resolved.

Starting repository state: surface `79771b4`, application `10f94dd`, both clean
on `master` before this task. These revisions locate the baseline, not approval.
External input: `C:/Users/imak/Downloads/vnm_terminal_correctness_patches_20260922.zip`;
inspected extraction:
`C:/plms/scratch/terminal_correctness_response_1/vnm_terminal_correctness_patches_20260922`.
The external patches are proposals, not validated implementation. Their Qt and
Windows paths were not built. Reject their malformed session declaration and
their approach of accepting original Qt events and replaying copies, which loses
normal unhandled-event propagation.

## Fixed contracts

- Stop commitment cannot restore readiness after a native side effect; an actual
  noncommitting rejection restores the exact prior state.
- Native-operation admission is not completion. Startup and applied geometry
  change only on authoritative completion; stale callbacks cannot revive a
  stopped or replaced session. The application must consume changed semantics.
- GUI event dispatch preserves accepted/ignored behavior and normal propagation.
  Input ordering observes earlier admitted output that changes terminal modes;
  later output cannot extend that prerequisite indefinitely.
- A held native or storage operation must not become a GUI destructor join.
  Native handles, callbacks, module code, and cleanup obligations remain owned
  until actual retirement. Stop progress behind a held native operation must be
  resolved explicitly, not hidden by a reserved FIFO slot.
- Capture failure/overflow cannot publish a successful completed artifact.
  Requested finalization and settled storage are distinct. No shared schema bump
  for this optional capability; missing/unavailable data stays capture-local.
- Render retries are finite, generation-safe, and autonomous for idle content;
  internal rollback must not replenish their budget.
- Linux request transmission and receipt share one startup deadline; failed or
  partial control writes close the sending direction rather than append frames.

## Source ownership and executable sequence

1. **Native cleanup and Linux transport:** `src/windows_conpty_backend.cpp`,
   `src/posix_pty_backend.cpp`, `src/posix_pty_owner.h`, cleanup-owner headers,
   `cpp/process_custody`, and focused native tests. Trace surface start through
   session backend calls to native owners; replace detached ConPTY close with
   accounted ownership and preserve observer lifetime. Provider selection in
   `cmake/vnm_terminal_process_custody_dependency.cmake` also accepts an installed
   or explicit external provider: new transport APIs must be usable or produce a
   precise configuration diagnostic there, not silently assume bundled source.
   Ownership tracing confirmed the leaf moved into surface on September 19;
   framework now consumes that surface-owned leaf. No separate provider migration
   is required. The current Linux owner creates `startup_deadline` before sending
   START and calls `send_owner_start_until` with that deadline. Historical Ninja
   dependency caches are not current ownership evidence and must not be used for
   the candidate gate.
2. **Session/native-operation integration:** backend/session contracts,
   `src/terminal_session.cpp`, its header and session tests, native wrapper,
   surface startup integration and `vnm_terminal/src/main.cpp`. On terminate
   rejection, `process_terminate_command` restores backend and geometry readiness
   only when `backend_result.stop_committed` is false; committed stop errors
   leave the session unready. Move blocking native work behind retained owners
   and integrate receipts end to end. This owner controls session files; other
   lanes provide integration edits for it rather than editing those concurrently.
3. **Capture storage (parallel preparation):** capture writer implementation,
   capture public progress contract, focused storage tests. Session construction,
   raw-output admission, exit and destruction integration are coordinated with
   lane 2. Retain bounded ordered storage without blocking the GUI or introducing
   an unbounded static-destruction join. Preserve existing capture bytes/format.
   **User decision:** keep the UI responsive and expose an explicit exit-pending
   state until asynchronous capture storage settles. Failed or overflowed
   storage must not publish a successful artifact. This lane-local decision does
   not block the native, rendering, or transport fixes. Do not adopt the
   external patch's static-registry destructor join. There will be no second
   external response.
4. **Renderer recovery (independent):** `src/qsg_atlas_renderer.cpp`, its internal
   header and atlas/canvas tests. Both surface and canvas consumers use the atlas
   path. Verify idle failed-preparation recovery and stale retry cancellation;
   inspect scenegraph/device-loss termination paths without asserting causality.
5. **GUI budget and ordering (after session contract settles):**
   `src/vnm_terminal_surface.cpp`, surface header, session callback drain code,
   input/session integration tests. Existing surface and session helpers contain
   unlimited drains used by input actions. Replace those reachable drains with
   bounded consumption while keeping original Qt dispatch semantics. This shares
   files with lane 2 and must not be developed as an overlapping blind patch.

Build/test CMake registration is shared: workers coordinate individual changes
or supply a patch to the integration owner. Delete superseded synchronous paths,
unaccounted detached close paths, and any unused helpers after each migration;
do not preserve competing implementations for an unspecified later batch.

## Verification and risks

Reproduce deterministic defects with the actual owner/consumer boundary before
claiming fixes. Test oracles are existing lifecycle contracts, native/protocol
semantics, and Qt accepted/ignored propagation, not snapshots of new behavior.
Relevant gates cover committed/noncommitting stop errors, held native startup or
resize with responsive GUI and destruction, startup/output/exit ordering, stale
resize receipts, concurrent output and input modes, unhandled keys, mouse/wheel/
IME/selection behavior, held capture writes/finalization/overflow, accounted
ConPTY close plus observer retirement, and autonomous atlas recovery on surface
and canvas. Run real application startup, resize, input and shutdown against the
candidate surface. Native helper tests alone do not establish that integration.

All compiling commands use `queued-build`. User explicitly requires XENIA only:
FASTBuild 1.20, `queued-build --slots 2 -- <cmake> --build <dir> -- -dist -nolocalrace`,
no local fallback. Set `/Z7` (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`) and
`CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV=OFF`. Configure/regenerate in VS 2026 x64
MSVC 14.51 environment and verify compiler paths. Report installation/reboot
indicators before the first build. Require literal `FBuild: OK:` success.
Build/test against an immutable integrated source snapshot. Linux execution is a
separate required gate for Linux changes; unavailable infrastructure must be
reported rather than replaced by a Windows-only claim. Private hosted CI runs
only when a person dispatches it; do not automatically start hosted runs.

An already blocked OS or filesystem call is not made cancellable by moving it
to a worker. Final process/module lifetime and pending cleanup require explicit
review. Fresh independent review must cover changed lifecycle and integration
behavior; an implementation/design participant supplies continuity review only.

## Current state and next action

Planning/source tracing complete; implementation and candidate gates remain
open. No external patch is accepted wholesale. The renderer idle-recovery
selectors now pass on XENIA after adding the existing backend/window probe. The
Linux POSIX startup-transport slice built and its focused test passed on Ubuntu
24.04 WSL. No fix or runtime verification beyond those results is claimed by
this document. Root coordinates owners and mechanically integrates; focused
workers implement and verify. Keep canonical checkouts on `master`.

Active first slice:

- `response1_native_review` implements stop commitment and ConPTY accounted close
  (F6/F7), owning backend contract, minimal session stop edits, native backend
  postcommit errors, Linux STOP EOF fallback, cleanup owner and focused tests.
  Source tracing found five synchronous startup-failure close paths missed by
  the external patch; transfer the handle to its pre-reserved owner immediately
  after birth so those branches cannot block the GUI in native close.
  The normal-child close path passes, but failed-start ownership remains
  unresolved: the `qScopeGuard` submits the close owner once after the
  post-birth child-start failure, while the close worker never reaches cleanup.
  The focused registered native gate is
  `vnm_terminal_windows_conpty_close_ownership` and remains failing on that
  path.
- `response1_gui_review` implements finite shared-atlas retries (F5), owning atlas
  renderer, retry helper and atlas tests. It is not yet changing GUI input.
  Its Windows CTest registrations are in `tests/CMakeLists.txt`:
  `vnm_terminal_qsg_atlas_idle_recovery_threaded_d3d11` and
  `vnm_terminal_qsg_atlas_idle_recovery_basic_d3d11`. The executable selector is
  `--idle-recovery --backend d3d11`; both idle-recovery selectors now pass on
  XENIA after adding the existing backend/window probe to the test setup.
- `correctness_gate_setup` performs read-only toolchain/build-tree preflight.
  VS 2026 is complete/launchable and no reboot indicators were found. Existing
  FASTBuild surface/application trees use `/Z7` and capture-system-environment
  OFF. Initial direct XENIA TCP 31264 connection timed out; a later root
  `Test-NetConnection` succeeded, so no persistent worker outage is established.
  The official XENIA queued-build gate remains required for the integrated
  candidate; no integrated application result is yet recorded. Do not edit
  code/build inputs until that gate returns or fall back to local compilation.
  The Linux POSIX startup-transport slice was separately built and its focused
  test passed on Ubuntu 24.04 WSL; this does not replace the remaining
  integrated gates.
  Use an exact CTest whitelist because some fixtures invoke nested compilation.
- `correctness_plan` owns this document and coordinates exact worker-produced
  CMake test registration edits. Root owns integration decisions.

Next action: resolve the failed-start ConPTY cleanup handoff, then freeze the
two current slices and run the corresponding integrated XENIA and Qt/native
gates. Native async receipts (F1), GUI budgeting (F2), and asynchronous capture
(F3) remain required subsequent work, not omitted findings; the Linux startup
deadline/provider slice has a focused WSL result but still needs its remaining
integration coverage. Sequence session/capture/GUI shared-file integration with
one owner at a time. Obtain fresh independent review of the integrated changes
and run application-level gates. Record results and remaining gates here before
ending or pausing the task. The renderer result does not resolve or establish
the suspected GPU incident, and this work is not complete. Commit, push,
release, and arbitrary new features are not part of this request.

The broader F1/F3 gates explicitly include process/module shutdown and static
destructor joins. Moving calls to retained workers must not conceal those waits;
preserve actual system ownership and report any unresolvable held-call limit.

F1 source-backed proposal is under one bounded architecture check by
`native_async_architecture`; it is not yet an adopted implementation. Keep a
bounded native ordinary-operation owner with independent pressure and stop
progress. Windows `terminate()` currently waits inside `request_conpty_close()`
for active resize calls before starting escalation; moving that stop call to a
different thread alone cannot resolve it. Stop commitment/cancellation/escalation
must proceed independently of close retirement, which still retains the handle
until active calls finish. Cancellation before startup dispatch means no birth;
stop during native startup remains intent until the native owner is available,
not a claim that arbitrary OS startup calls are cancellable. One authoritative
startup receipt precedes buffered output/exit and controls readiness; current
resize receipts certify geometry. Public pending-start semantics, application
failure/timeout handling, and surface/resize/session consumers migrate together.
Do not apply the external patch's FIFO-only reserved stop as the solution.
