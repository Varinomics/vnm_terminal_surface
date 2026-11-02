# P26: Terminal canvas idle observation

## Outcome and continuation

**Finding 146 remains low severity, with the timer behavior now reproduced in
an isolated Linux canvas.** All 21 canvas observations delivered exactly
30 status-timer callbacks during a three-second interval, including no-model,
retained-hidden, hidden-parent, and hidden-window states. None published a
status or error change during those intervals. The three empty-window controls
had no canvas timer.

The historical claim that hidden cursor blinking produces window frames was
**not reproduced on this backend**. A hidden blinking canvas
continued to deliver six blink callbacks per interval, but produced zero atlas
prepare/render callbacks and zero Qt `afterRendering`/`frameSwapped` signals in
all three observations. These software-Linux results do not establish physical
display or mobile behavior.

This PR adds an opt-in observation executable and this report. Production timer
and blink policies remain unchanged: the complete Companion path and actual
kernel wakeup telemetry were not executed, so no event strategy or production
performance optimization is adopted from these component measurements.

- Owner: `vnm_terminal_surface`.
- Branch: `work/linux-review-20261102-p26-terminal-canvas`.
- Draft PR: <https://github.com/Varinomics/vnm_terminal_surface/pull/49>.
- Production source baseline: `b67469c975113feb95b3cce7a573156a88a8616a`.
  The clean initial worktree and both evidence-producing builds were checked
  against their recorded source inventories; production files were unchanged.
- The branch's continuation is a bounded full-Companion idle observation on a
  setup with the actual runtime/transport and compatible Soter payload. A
  scheduler-trace-capable host is needed to quantify actual wakeups. Compare
  visible idle, a retained session hidden by changing sections, and the distinct
  cleared-session state before selecting a production change.

## Finding disposition

| Finding | Established evidence | Disposition | Remaining limit |
|---|---|---|---|
| 146 | Source-confirmed unconditional polling and full report copy; 30 delivered callbacks per three-second component observation even when hidden or without a model | Retain low severity; deliver a reusable observation fixture and measured report | Complete Companion idle path, actual scheduler wakeups, hardware/mobile energy and cadence remain unmeasured |

### Current source and consumer boundary

[`VNM_TerminalCanvas`](../../../src/vnm_terminal_canvas.cpp) starts its 100 ms
status timer in the constructor. Its visibility hook cancels touch input;
it does not stop status observation. `refresh_render_status()` copies
[`Qsg_atlas_recorder::snapshot()`](../../../src/qsg_atlas_renderer.cpp) before
checking `status_changed`. The
[`report type`](../../../include/vnm_terminal/internal/qsg_atlas_renderer.h)
includes counters, summaries, retained snapshot ownership, and optional touch
selection metadata. The copy under the recorder mutex is source-confirmed;
its standalone cost was not separately measured.

`refresh_cursor_blink()` uses the frame's cursor visibility/blink flags.
`toggle_cursor_blink_phase()` changes phase and calls `update()`. The experiment
shows why an update request must not be equated with a rendered window frame.

Companion source was inspected at
`logonomic` revision `b9e44f6fe131624d48b0338c23295dae67d526bd`:

- [MobileMain.qml](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/mobile/qml/MobileMain.qml)
  retains the session view inside permanent `StackLayout` pages. Changing the
  active section hides that view without destroying it. Returning to the
  session list clears the selected session, which is a different no-frame state.
- [MobileSessionView.qml](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/mobile/qml/MobileSessionView.qml)
  contains the canvas beneath a visibility-controlled viewport. Presentation
  readiness and errors influence the existing placeholder/canvas and input
  admission behavior; status publication is not merely diagnostic polling.
- [The mobile canvas wrapper](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/mobile/src/logonomic_mobile_terminal_canvas_item.cpp)
  requires matching canvas generation, frame sequence, and publication
  generation before crediting a new presentation. Hiding does not itself clear
  the frame. Disconnect already disables cursor blinking, and an ended session
  removes the input cursor; the retained-connected hidden state must not be
  conflated with either case.
- [The mobile build profile](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/mobile/CMakeLists.txt)
  consumes the renderer with `FULL=OFF` and `MSDF_TEXT_RENDERER=OFF`. The probe
  uses these canvas-relevant settings and the Qt LCD provider. The full
  application's additional RHI/atlas use for other components is outside this
  renderer-only probe.

The maintained mobile preview supplies fixture state and omits transport.
Existing latency/SIXEL modes drive active workloads rather than passive hidden
idle. The full maintained route also needs a compatible actual Soter payload.
That setup was not assembled or executed here; it is a concrete missing
measurement boundary, not a claim that full-path testing is impossible.

## Measurements

The executable is
[`vnm_terminal_canvas_idle_probe`](../../../benchmarks/terminal_canvas_idle/terminal_canvas_idle_probe.cpp),
under the existing opt-in benchmark switch. It is Linux-only because its CPU
and context-switch observations use `getrusage`. Renderer-only configurations
can build this target without configuring the full-surface benchmarks. It adds
no automatic performance test, fixed timing threshold, or production hook.

Each case ran three times as a new process under isolated Xvfb/xcb, Qt 6.12.0,
OpenGL 4.5, Mesa 25.2.8 llvmpipe (LLVM 20.1.2, 256 bits), and Qt's threaded
render loop. Every run's backend log confirms that selection. The second
repetition reversed case order. The frame-bearing cases use a six-row,
24-column frame with two text cells and a cursor; selection metadata and a live
transport workload are absent.

After initial natural presentation where applicable, the case establishes its
visibility state and settles for one second. The measured interval is an
ordinary event loop with one deadline timer, with no repeated `processEvents`,
`update`, `requestUpdate`, or `grabWindow` calls. Actual elapsed intervals were
3000.226–3003.589 ms. CPU includes the observation callbacks and deadline
machinery. It covers the probe process, not the separate Xvfb server or device
energy. The observed one-minute system load ranged from 1.08 to 1.82.

All 24 runs returned success. Counts below are the min–max across three trials;
CPU columns are median (min–max), in milliseconds per approximately three-second
interval. `prepare_count` equaled `render_count` in every run.

| State | Status callbacks | Blink callbacks | Recorder render callbacks | Qt swap signals | Process CPU ms | GUI CPU ms |
|---|---:|---:|---:|---:|---:|---:|
| `empty-window` | 0 | 0 | 0 | 0 | 0.464 (0.157–0.552) | 0.464 (0.157–0.552) |
| `no-model` | 30 | 0 | 0 | 0 | 3.848 (2.135–11.345) | 3.848 (2.134–11.344) |
| `visible-steady` | 30 | 0 | 0 | 0 | 2.287 (2.041–10.313) | 2.287 (2.040–10.314) |
| `visible-blink` | 30 | 5–6 | 5–6 | 5–6 | 23.332 (20.859–110.776) | 5.136 (3.969–55.783) |
| `hidden-steady` | 30 | 0 | 0 | 0 | 5.587 (2.116–7.884) | 5.587 (2.115–7.884) |
| `hidden-blink` | 30 | 6 | 0 | 0 | 2.648 (2.433–8.209) | 2.648 (2.432–8.210) |
| `hidden-parent` | 30 | 0 | 0 | 0 | 2.465 (1.768–3.884) | 2.465 (1.768–3.883) |
| `hidden-window` | 30 | 0 | 0 | 0 | 2.382 (1.865–2.584) | 2.382 (1.866–2.585) |

All measured intervals had zero public status/error notifications and no
render error. `hidden-parent` and `hidden-window` use a steady cursor;
`hidden-blink` separately exercises a hidden canvas with blinking enabled.
For the hidden-window case, the item's visibility remained true while the
window was invisible and unexposed. This is distinct from an effectively
invisible item.

The CPU ranges are noisy, especially visible blinking. They are descriptive
instrumented observations, not an estimate of per-callback CPU cost or evidence
of an optimization gain. The recurring timer-delivery result is consistent
across all repetitions. A total cost or battery claim requires the missing
full-path and target-device measurements.

### Event-loop and scheduler evidence

| State | GUI event-dispatcher `awake` signals | GUI voluntary context switches |
|---|---:|---:|
| `empty-window` | 2 | 1 |
| `no-model` | 32 | 31 |
| `visible-steady` | 32 | 31 |
| `visible-blink` | 37–43 | 42–48 |
| `hidden-steady` | 32 | 31 |
| `hidden-blink` | 34–38 | 33–37 |
| `hidden-parent` | 32 | 31 |
| `hidden-window` | 32 | 31 |

These are **not kernel wakeup counts**. Timer coalescing and other work prevent
converting delivered callbacks, dispatcher notifications, or context switches
into an exact number of additional OS wakeups. The probe therefore records
`kernel_wakeups: null`.

The host had no scheduler tracepoint inventory at either
`/sys/kernel/tracing/available_events` or
`/sys/kernel/debug/tracing/available_events`, no
`/proc/sys/kernel/sched_schedstats`, and no wakeup fields in `/proc/self/sched`.
`perf_event_paranoid` was 2; `perf`, `bpftrace`, and `trace-cmd` were unavailable.
Actual scheduler-wakeup coverage remains unavailable on this exposed setup.

### Hidden-state resumption

After each hidden observation, the probe installs a different frame sequence
and publication generation and shows the item/window naturally. All 12 checks
observed matching public canvas/frame/publication identities. Resume values
are median (min–max), in milliseconds:

| Hidden state | Matching replacement published | Resume ms |
|---|---:|---:|
| `hidden-steady` | 3/3 | 68.469 (34.816–99.260) |
| `hidden-blink` | 3/3 | 97.789 (60.737–98.511) |
| `hidden-parent` | 3/3 | 67.605 (38.103–72.714) |
| `hidden-window` | 3/3 | 82.428 (61.136–94.813) |

This checks one matching replacement publication. It does not establish full
Companion input-admission timing, hardware display latency, or complete blink
resume cadence.

## Correctness contracts retained

`refresh_render_status()` requires current ownership and a matching committed
prepare/capture/canvas generation before advancing the displayed identity.
Failed preparation must still publish its error while retaining the
last complete draw. Clearing increments ownership and synchronously clears
readiness, displayed identities, and error. Stale work must not resurrect them.
Touch input uses the recorder's actually drawn snapshot, metrics, selection,
and ownership; a failed candidate must not become the input basis.

The existing blink contract preserves active timer cadence and phase through
frequent blinking-frame replacements, stops for a nonblinking or cleared frame,
and releases the timer on destruction. Any future event-driven change must
preserve these contracts, failed-prepare publication, and hidden/window resume
behavior. Stopping a timer solely to improve its counter is not an accepted fix.

## Validation actually performed

| Gate | Actual inputs/backend | Result |
|---|---|---|
| P26 package manifest | Original handoff ZIP | All 11 inventoried payload members matched size and SHA-256 |
| Focused baseline configure/build | Release, Qt SDK, refreshed owned source, default full/MSDF-enabled owner configuration | Passed; only `vnm_terminal_canvas_contract` and `vnm_terminal_canvas_renderer` built |
| Registered `vnm_terminal_canvas_contract` | Offscreen/software | Passed, 0.06 s |
| Registered `vnm_terminal_canvas_renderer` | Registration forces offscreen on Linux | Skipped with return 77, 0.68 s; no renderer coverage credited |
| Exact renderer executable via `sdk-run-opengl` | Isolated Xvfb/xcb, Mesa llvmpipe OpenGL 4.5, threaded loop | Passed; includes retained failed-prepare identity, rendered touch presentation, image lifecycle, and pixel readback checks |
| Probe configure/build | Release, renderer-only LCD profile, MSDF/atlas off | Passed; 27 build steps; source inventory unchanged during build |
| Bounded probe matrix | Same LCD binary/backend, eight cases × three trials | 24 successful observations; all 12 hidden replacement checks matched |
| Patch integrity | New benchmark registration/source and this report | `git diff --check` passed |

A registered skip is preserved separately from the direct renderer execution.
Recorder `render_count` counts callback invocations even when `drew` is false;
it is not a physical-frame counter. Qt rendering/swap signals likewise do not
measure physical display or energy. No unchanged production gate was rerun
merely because benchmark/documentation files were added.

### Build inputs and setup outcomes

The external kit was `logonomic-linux-development-sdk-2026.10.23.1`, with Qt
6.12.0, GCC 13.3.0, CMake 3.31.10, Ninja 1.13.2, ccache 4.9.1, and glibc 2.39
on Ubuntu 24.04. The reused 559,011,740-byte original archive independently
matched SHA-256
`56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`.

| Setup check | Observed outcome and disposition |
|---|---|
| Extracted SDK payload checksums | Check returned failure for 44 regenerated Python `__pycache__/*.pyc` files. Every listed non-bytecode payload, including Python sources and Qt/tool binaries, matched. The extraction is not described as wholly byte-identical. |
| Initial read-only SDK preflight | Could not create the required in-SDK temporary file. Reused extraction was copied into task-writable storage; preflight then passed. Documented missing `bin/{cmake,ctest,cpack,ninja}` links point to the bundled tools. |
| Initial restricted Xvfb attempt | Could not open local display sockets before test execution. The authorized isolated fixture was rerun with sandbox escalation and passed; the 24 observation runs used that same approved isolated setup. |

These are setup/integrity outcomes, not canvas product failures. The selected
contract and executed renderer/probe gates reported no product failure.

Compiler-probing configure/preflight commands reserved one queued slot. Focused
builds reserved four slots and used native parallel width four. Autogen width
was four for the baseline build and one for the LCD probe build. Compiler
launchers actually used the persistent task ccache: 71/71
cacheable calls, 13 hits and 58 misses across both builds; 13 of the LCD build's
20 compilations reused baseline cache entries. Current source/dependency trees
and the cache were outside the external SDK.

Captured owned source inputs were `vnm_cmake` at `66d5ee14f4f69c952daa58a95a4759ad77a636f4`,
`vnm_fonts` at `3a7e37bab54a705ddc08e3de5cc3e4f655256396`,
`vnm_msdf_text` at `3e89f1358c88b46f7c68b99dd9bf76003b5a3db3`, and, for the full
baseline configuration, `vnm_qt_dispatch` at
`666f6d054b4cf79b39a240370eeb7053d2a07ee3`. The baseline's declared external
msdfgen input was v1.13 at `1874bcf7d9624ccc85b4bc9a85d78116f690f35b`;
the LCD probe did not require its atlas. These are provenance records;
owned forward dependency declarations still track `master`.

The retained evidence bundle contains both builds' source/CMake inventories,
configure/build logs, SDK integrity/preflight outcomes, registered/direct test
logs, scheduler capability probe, all 24 raw stdout/stderr pairs,
`idle-observations.json`, `idle-summary.json`, and the repetition driver.

## Reproduction and outstanding boundary

With the documented external SDK activated, persistent ccache configured, and
explicit current owned-source overrides selected, configure a separate Release
tree with:

```text
BUILD_TESTING=OFF
VNM_TERMINAL_SURFACE_BUILD_TESTING=OFF
VNM_TERMINAL_SURFACE_BUILD_FULL=OFF
VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER=OFF
VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_TEXT=OFF
VNM_TERMINAL_BUILD_BENCHMARKS=ON
```

Build and execute the focused target:

```sh
queued-build --slots 4 -- cmake --build build-idle-lcd --parallel 4 \
  --target vnm_terminal_canvas_idle_probe
sdk-run-opengl build-idle-lcd/benchmarks/terminal_canvas_idle/vnm_terminal_canvas_idle_probe \
  --scenario hidden-blink --duration-ms 3000
```

`--help` lists all eight cases. Use the same scale/environment and repeat each
case three times to reproduce the recorded observation boundary. The fixture
returns 77 when the initial natural render is unavailable on an unsupported
headless backend; that is not a successful rendering observation. A missed
matching publication under the selected OpenGL backend returns ordinary failure.

| Remaining gate | What is missing | Concrete next observation |
|---|---|---|
| Complete Companion idle path | Actual production runtime/transport with a compatible Soter payload and passive observation setup was not run | Connected idle session → retained session hidden by section change → restored session, with producer/model/canvas/presentation evidence; keep back-to-list clearing separate |
| Actual process wakeups | Host does not expose the required scheduler tracepoint/counter surface | Collect scheduler events for the same bounded states on a capable host; retain Qt and context-switch counters as separate metrics |
| Mobile energy/cadence and hardware rendering | No representative device or hardware-rendering measurement | Measure target wakeups/energy and resume/blink cadence before making device-specific claims |

The result is a completed bounded owner-component investigation with those
explicit full-path/native limits. No private hosted CI, live-user UI/device
operation, ConPTY work, or release qualification was performed.
