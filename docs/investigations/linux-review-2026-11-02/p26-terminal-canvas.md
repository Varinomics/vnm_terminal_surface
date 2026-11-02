# P26: Terminal canvas idle observation

## Outcome and continuation

Finding 146 remains **source-confirmed, low severity**. The canvas starts a
100 ms status timer at construction and leaves it active while the item lives.
Each delivery copies the full recorder report under its mutex before checking
whether the public status changed. This checkpoint does not establish the
number of additional operating-system wakeups, the CPU cost, physical frames,
or a battery impact.

The owning repository is `vnm_terminal_surface`; the task branch is
`work/linux-review-20261102-p26-terminal-canvas`, created from refreshed source
at `b67469c975113feb95b3cce7a573156a88a8616a`. That revision records the inspected
starting inputs, not a future dependency pin. The implementation worktree began
clean. This checkpoint adds only this expressly requested investigation report;
the production timer, renderer, and blink policies are unchanged. The draft PR
link will be recorded after publication.

**Next action:** finish the focused Linux baseline gates, then run a bounded
observation fixture through natural event-loop idle periods and hide/show
transitions. Resolve the Companion's actual retained-hidden lifecycle and
measure its complete affected path before enabling an optimization. A supported
report/fixture outcome is valid if that measurement boundary is unavailable.

## Finding disposition

| Finding | Evidence established | Disposition | Still unmeasured |
|---|---|---|---|
| 146 | Unconditional 100 ms status timer; complete report snapshot on delivery; blink timer does not consult visibility; Companion instantiates the canvas | Retain the low-severity source finding and investigate before choosing an event strategy | Callback delivery in idle/hidden states, CPU, scheduler wakeups, natural rendering, complete Companion path, mobile energy |

### Current source evidence

- [`VNM_TerminalCanvas` construction](../../../src/vnm_terminal_canvas.cpp)
  creates the status timer, connects it to `refresh_render_status()`, and starts
  it without a model or visibility condition. The only `visibleChanged` handler
  cancels touch input when hidden.
- `refresh_render_status()` takes `Qsg_atlas_recorder::snapshot()` before its
  `status_changed` early return.
- [`Qsg_atlas_recorder::snapshot()`](../../../src/qsg_atlas_renderer.cpp) returns
  `m_report` by value while holding `m_mutex`. The
  [`report type`](../../../include/vnm_terminal/internal/qsg_atlas_renderer.h)
  includes metrics, summaries, retained snapshot ownership, and optional touch
  selection metadata.
- `refresh_cursor_blink()` uses the frame's cursor visibility and blink flag.
  `toggle_cursor_blink_phase()` changes phase and calls `update()`. This proves
  an update request, not that a hidden item produces a physical frame.
- The inspected `logonomic/mobile/qml/MobileSessionView.qml` has a `TerminalCanvas`
  child inside a `Flickable` whose visibility follows
  `page.terminal_canvas_available`. That caller establishes reachability. The
  surrounding page lifetime and complete Companion idle path still require
  execution evidence.

Ten timer opportunities per second do not establish ten extra kernel wakeups:
other events can coalesce with them. Event-dispatcher wake notifications,
voluntary context switches, and render requests must be named as those metrics,
not relabeled as actual scheduler wakeups or energy savings.

## Contracts a candidate must preserve

Status publication is more than a ready flag. `refresh_render_status()` checks
the current ownership generation and matches committed prepare/capture/canvas
generations before advancing the displayed frame identity. Failed preparation
must still publish the existing error while retaining the last complete draw.
Clearing the canvas increments ownership and clears readiness, displayed
identities, and error synchronously. Stale render work cannot resurrect them.

Touch selection and mouse targeting use the recorder's actually rendered
snapshot, cell metrics, selection, and ownership generation. Notification timing
must not attribute a failed candidate to the displayed frame or allow stale
ownership to become input-ready.

The existing blink lifecycle contract preserves the active timer cadence and
phase through frequent blinking-frame replacements, stops on a nonblinking or
cleared frame, and releases the timer on canvas destruction. Hide/show and
window reattachment need measurement before selecting any different policy.
Neither timer will be stopped solely to improve its counter.

## Validation actually performed

| Check | Inputs | Actual result |
|---|---|---|
| Package integrity | P26 archive and its `MANIFEST.json` | All 11 inventoried payload members match size and SHA-256 |
| Current source and contract inspection | Refreshed owner source; canvas, recorder, contract and renderer fixtures; Companion call site | Source behavior above confirmed; no native reproduction claimed |
| Registered test discovery | `tests/CMakeLists.txt` | Exact CTest entries are `vnm_terminal_canvas_contract` and `vnm_terminal_canvas_renderer` |
| Focused Linux configure/build and tests | Pinned external Qt SDK, current owned dependencies, persistent ccache, queued build width at most four | Pending at this checkpoint |
| Natural idle/hidden observation | Visible stable frame, blinking frame, retained hidden item, hidden window, and no-model baseline | Pending at this checkpoint |
| Complete Companion path | Production producer, transport/model, canvas, and presentation lifecycle | Pending at this checkpoint |

`vnm_terminal_canvas_renderer` declares `SKIP_RETURN_CODE 77` and a 60-second
timeout. A skipped renderer entry is not renderer coverage. Its current helper
repeatedly calls `update()`, `requestUpdate()`, and `grabWindow()` while waiting
for pixels; that helper is useful for rendering contracts but cannot measure
natural idle behavior. Renderer evidence must name the actual platform and
graphics backend. Mesa OpenGL validates software-rendered Linux behavior only.

## Measurement boundary

Use the same release build, backend, instrumentation, frame content, and window
configuration for compared runs. Record bounded repetitions rather than relying
on a single noisy wall-clock result. A separate empty-window/no-canvas control
distinguishes toolkit activity from canvas work. Observe timer deliveries,
prepare/render deltas, published identities/errors, and process/thread CPU over
untouched event-loop intervals; use actual scheduler wakeup telemetry only when
the host exposes it.

Visible steady and blinking frames, retained-hidden items, hidden windows, and
hide/show resumption exercise different states. A resumed frame must reach its
matching publication generation naturally. Preparation failure/recovery and
touch-generation checks remain correctness gates for any chosen change.

## Native or unavailable follow-up

| Gate | Limit | Evidence still required |
|---|---|---|
| Representative Companion/mobile energy and cadence | No target-device measurements in this checkpoint | Screen-on idle, retained-hidden/background behavior, wakeups, energy, and resume cadence on a representative device |
| Hardware rendering | Software Mesa evidence does not establish GPU behavior | Applicable target graphics backend and hardware observation |
| Kernel scheduler wakeups | Availability has not yet been established | Record actual available scheduler events, or state the concrete unavailable capability and retain proxy labels |

This investigation does not authorize private hosted CI, live-user UI/device
operation, ConPTY work, or release qualification.
