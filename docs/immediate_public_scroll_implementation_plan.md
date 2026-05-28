# Immediate public scrolling implementation plan

## Status

Implementation plan for `docs/immediate_public_scroll_architecture.md`.
Phases 0 through 7 completed the opt-in implementation and evaluation tooling.
Phase 7 completed the performance, memory, replay, and default-policy
evaluation on 2026-05-28. The default-policy change was not approved: the
default remains `DEFER_UNTIL_CONTENT_PUBLICATION`, and immediate public
projection remains opt-in.

The goal is immediate visible scrolling during DEC synchronized-output holds
without publishing hidden live content. This plan is intentionally phase-gated:
implementation phases must be reviewed after each phase, and no externally
reachable immediate-scroll behavior may ship until public scroll publication,
release reconciliation, transcript replay, and app/API wiring are complete.

## MVP non-goals

- Do not make immediate public scrolling the default.
- Do not implement projection-backed selection creation.
- Do not resize public projections during synchronized-output holds.
- Do not split production rendering into separate content/view streams.
- Do not render from live model state while synchronized output is active.
- Do not optimize dirty ranges before full-viewport dirty snapshots are correct.
- Do not expose app CLI opt-in until the core surface behavior is complete.

## Global rules for every phase

- The default `DEFER_UNTIL_CONTENT_PUBLICATION` path must remain behaviorally
  unchanged unless a phase explicitly states otherwise.
- Every phase must add or preserve a deferred-mode regression check relevant to
  the touched area.
- Intermediate scaffolding must be runtime-inert unless guarded by an internal
  test-only policy path.
- Public projection rows are copied records for the MVP. Immutable row leases or
  copy-on-write storage are future optimizations.
- `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshots must never advance the
  safe live-content basis.
- Release dirty/coalescing must compare against the latest safe live-content
  basis, not the latest visible public-projection scroll snapshot.
- Phase 1 projection storage is viewport-only. Before any later phase scrolls,
  anchors, or renders outside the copied viewport, that phase must add a safe
  full public row source or invalidate/defer off-viewport requests.

## Shared deferred-mode regression matrix

Each phase that touches the relevant subsystem must preserve these checks:

- Default policy is deferred.
- Deferred wheel during synchronized-output hold is still visually deferred.
- Deferred transcript/replay remains compatible with old transcripts.
- Deferred mode does not create public projections.
- Deferred mode does not split or reorder publication boundaries except where an
  explicit backward-compatible test proves the same externally visible result.
- Deferred scrollbar behavior remains unchanged.
- Deferred public surface scroll APIs keep their existing return semantics.

## Phase 0: inert schema, diagnostics, and regression harness

### Objectives

- Add vocabulary and test harness without enabling immediate public scrolling.
- Add snapshot basis/purpose fields with safe defaults on existing snapshots.
- Define diagnostic reason enums and transcript schema compatibility.
- Establish the deferred-mode regression suite that later phases must keep green.

### Owned files and call sites

- `include/vnm_terminal/internal/render_snapshot.h`: add snapshot basis and
  purpose fields to `Terminal_render_snapshot`.
- `src/terminal_screen_model.cpp`: ensure normal live snapshots set
  `basis=LIVE_CONTENT, purpose=CONTENT`.
- `src/terminal_session.cpp`: ensure existing derived geometry/selection
  snapshots set the appropriate purpose without changing behavior.
- `include/vnm_terminal/internal/terminal_transcript.h` and
  `src/terminal_transcript.cpp`: add schema fields with compatibility defaults.
  The canonical MVP field set is schema version, snapshot basis, snapshot
  purpose, effective synchronized-output scroll policy, policy-change event and
  diagnostic reason, public projection generation, public viewport before/after,
  live viewport before/after on release, visible scroll applied, live content
  publication blocked, release reconciliation result, hidden-row eligibility and
  clamp reason, and public-projection disable reason.
- `tools/transcript_replay/terminal_transcript_replay.cpp`: parse old transcripts
  as `LIVE_CONTENT/CONTENT`.
- Tests under `tests/render_snapshot`, `tests/transcript`, and
  `tests/backend_session`.

### Behavior

- No public `VNM_TerminalSurface` property yet.
- No app flag yet.
- No public projection construction yet.
- Existing diagnostics are not removed. Any old boolean implication is retained
  for legacy snapshots, while new fields are present with legacy-compatible
  values.

### Tests

- Existing snapshots serialize as `LIVE_CONTENT/CONTENT`.
- Existing derived selection/geometry snapshots have stable basis/purpose values.
- Old transcript fixtures replay with compatibility defaults.
- New schema fields round-trip through capture/replay.
- Effective policy and mid-hold policy-change diagnostic fields round-trip.
- Deferred-mode wheel and release tests remain unchanged.

### Review gate

Reviewers must confirm this phase is runtime-inert and does not expose immediate
scrolling through any public API.

## Phase 1: copied public projection model

### Objectives

- Add immutable copied projection records.
- Build projections from already-safe public content only.
- Keep projection construction helper-only and runtime-inert outside tests.
- Store only the safe-basis viewport rows while recording safe-basis scrollback
  depth and active-grid metadata for later phases.

### Owned files and call sites

- New internal header/source for projection data, for example
  `include/vnm_terminal/internal/terminal_public_projection.h` and
  `src/terminal_public_projection.cpp`.
- `src/terminal_session.cpp`: helper to build a projection from the latest safe
  live-content basis, not from hidden live rows.
- Tests under `tests/backend_session` or a new projection-focused test target.

### Data choices

For Phase 1, copy row records into the projection only for the safe-basis
viewport window:

- safe-basis viewport rows from public primary scrollback when they are visible;
- safe-basis viewport rows from the public active grid when they are visible;
- cells;
- retained line provenance;
- content generation;
- copied or immutable style data;
- copied or immutable hyperlink identity data;
- public cursor metadata;
- public mode and color state;
- active-buffer kind and epoch;
- grid size and geometry generation.

Phase 1 also records metadata that describes the safe basis but does not imply
storage:

- `safe_basis_scrollback_depth`: public scrollback depth reported by the safe
  basis;
- `safe_basis_active_grid_rows`: active-grid rows reported by the safe basis;
- `first_copied_public_row`: public-row identity of the first stored viewport
  row;
- `stored_row_count` and `copied_row_bound`: the number of rows actually copied
  from the safe-basis viewport.

No Phase 1 accessor may be named as if the scaffold stored all public scrollback
rows. In particular, a method named `scrollback_row_count()` would be ambiguous
and must not be exposed by the projection scaffold.

Cell positions inside each copied row are row-relative. Row identity is carried
by the copied row's `public_row` field.

Projection generation is a monotonic session counter. It advances once per
captured projection and never resets during the session.

This phase is a safe-basis copied projection scaffold, not the full public row
store required for visible immediate scrolling. Phase 4 cannot render public
scrollback from this viewport-only scaffold. A later phase must add a safe full
public row source, or it must invalidate/defer any off-viewport request before
that request can affect visible behavior.

### Tests

- Projection stores only the safe-basis viewport rows, including public
  scrollback rows only when they were visible in that viewport.
- Projection records safe-basis scrollback depth and active-grid metadata
  separately from stored-row count.
- Capturing a scrolled safe-basis viewport with `offset_from_tail > 0` preserves
  `first_copied_public_row` and copied public row identities without storing the
  full public scrollback.
- Driving scrollback beyond the viewport keeps stored rows bounded by the
  visible/grid rows while safe-basis scrollback depth metadata grows.
- Projection does not include hidden row, cursor, mode, style, or hyperlink
  changes introduced after the safe basis.
- Mutating live style/hyperlink/model state after projection capture does not
  mutate copied projection data.
- Projection memory footprint is bounded by copied safe-basis viewport rows.
- Projection generation advances monotonically.

### Deferred-mode regression

- Deferred policy creates no public projection during a synchronized-output hold.

### Review gate

Reviewers must confirm copied projection rows have no mutable references to live
hidden state, the memory bound is explicit, and the viewport-only scaffold cannot
be mistaken for a full public row store.

## Phase 2: internal public viewport and release intent

### Objectives

- Add public viewport state and release-intent metadata.
- Implement sticky-tail state transitions and detached anchor refresh logic.
- Keep this internal/test-only; do not publish public scroll snapshots yet.

### Owned files and call sites

- `include/vnm_terminal/internal/viewport_contract.h` and
  `src/viewport_contract.cpp` if reusable viewport helpers are needed.
- `include/vnm_terminal/internal/terminal_session.h` and
  `src/terminal_session.cpp` for session-owned public viewport and release
  intent.
- Tests in `tests/backend_session`.

### Behavior

The public viewport controller is distinct from the live controller:

- parser/model updates mutate live viewport only;
- public scroll intent mutates public viewport only;
- sticky-tail is explicit and not derived from `viewportAtTail`;
- clamping to public offset `0` does not set sticky-tail;
- explicit scroll-to-bottom, End, or offset `0` sets sticky-tail.

Detached anchor refreshes whenever public scrolling changes the first visible
row.

Projection invalidation stores deferred release intent only:

- freeze public viewport values;
- keep sticky-tail intent and detached anchor metadata;
- do not use hidden live bounds for further scroll input.

Selection mutation during a public-projection hold is ignored for all mutation
paths listed in the architecture document. Existing payload copy remains allowed.

### Tests

- `viewportAtTail == true` with `sticky_tail == false` releases through the
  detached path in helper-level reconciliation tests.
- Clamping to public offset `0` does not set sticky-tail.
- Explicit scroll-to-bottom sets sticky-tail.
- Detached anchor refreshes when public scroll changes the first visible row.
- Scroll input after projection invalidation records deferred intent without
  reading hidden live bounds.
- Resize invalidation records `public_projection_geometry_invalidated` on the
  controller-side invalidation path and `detached_anchor_geometry_changed` with
  `retained_id_best_effort` on release when the detached retained row survives
  the geometry change.
- Memory-pressure invalidation records
  `public_projection_memory_pressure_invalidated`, keeps the configured
  public-projection disable reason, and preserves release-intent metadata.
- Post-invalidation scroll input records
  `public_projection_invalidated_deferred_intent`.
- Selection mutation paths are ignored while existing selected payload remains
  copyable.
- Visual-span compatibility and suppression are not part of Phase 2; Phase 2
  preserves cached payload copy behavior and leaves public-basis visual-span
  emission to the later public snapshot phases.

### Deferred-mode regression

- Deferred wheel and selection behavior are unchanged when no public projection
  is active.

### Review gate

Reviewers must confirm controller state cannot drift into public properties or
release reconciliation without explicit policy activation.

## Phase 3: synchronized-output boundary splitting and release reconciliation

### Objectives

- Implement the release-side safety machinery before visible public scrolling is
  reachable.
- Add policy-gated `DECSET 2026` and `DECRST 2026` boundary splitting for the
  immediate internal path.
- Implement release reconciliation and live-content basis separation.

### Owned files and call sites

- `src/terminal_session.cpp`: synchronized-output prescan/splitting currently
  around the synchronized-output detection path; extend it to handle policy-gated
  entry and release boundaries.
- `src/terminal_session.cpp`: release path and forced-release path.
- `src/terminal_session.cpp`: latest visible snapshot versus latest safe
  live-content basis slot ownership.
- Tests in `tests/backend_session` and `tests/transcript`.

### Behavior

Under internal immediate policy only:

- split at `DECSET 2026`;
- publish safe prefix before entering hold;
- capture projection from that safe-prefix basis;
- split at `DECRST 2026`;
- reconcile public release intent before release snapshot;
- publish release before processing post-release suffix bytes;
- forced release follows the same ordering before queued bytes can coalesce.

Under default deferred policy:

- preserve existing externally visible behavior.

Release fallback order:

- incompatible buffer kind or epoch records a deterministic incompatible-buffer
  result before sticky-tail or retained-anchor actions;
- sticky-tail follows live tail;
- exact retained anchor match;
- retained-id best effort when geometry changes but the retained row survives;
- nearest successor;
- nearest predecessor;
- oldest available live primary scrollback.
- deferred offset is an offset-only fallback when no detached anchor is
  available.

Epoch mismatch prevents exact restoration and records
`screen_buffer_epoch_changed`. Hidden alternate-screen release records
`buffer_transition_released` where applicable.

### Tests

- Safe prefix before `DECSET 2026` is published before projection capture.
- Hidden suffix after `DECSET 2026` is not in projection.
- `DECRST 2026` post-release suffix is not in the release snapshot and is
  processed afterward.
- Forced release publishes and reconciles before later queued bytes coalesce.
- No-payload synchronized-output hold preserves immediate release intent.
- Exact retained anchor restoration works.
- Content-generation mismatch falls back deterministically.
- Successor, predecessor, and oldest-live fallback paths are deterministic.
- `nearest_predecessor` and detached-anchor `oldest_available_live` remain
  schema-defined fallbacks but are not expected to be reachable through the
  viewport-only MVP projection; a full retained-row store can exercise them
  without invalidating the copied-row source.
- Detached fallback never silently falls to live tail unless sticky-tail is true.
- Sticky-tail release follows live tail across hidden scrollback growth.
- Primary -> alternate -> primary epoch mismatch prevents exact restoration and
  records `screen_buffer_epoch_changed`.
- Hidden alternate-screen release disables primary anchor application and records
  `buffer_transition_released`.
- Phase 3 in-bounds immediate public scroll emits no visible
  `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshot.
- Release dirty/coalescing compares against latest safe live-content basis, not
  latest visible public-projection snapshot.

### Deferred-mode regression

- Existing deferred release path remains externally unchanged.
- Existing deferred transcript and wheel behavior remain unchanged.

### Review gate

Reviewers must confirm release reconciliation is complete before any visible
public scroll snapshot can be emitted by a public or app-facing path.

## Phase 4: public projection scroll snapshots plus replay

### Objectives

- Emit immediate public scroll snapshots under the internal immediate policy.
- Make transcript replay consume those snapshots in the same phase.
- Keep public app opt-in unavailable until this phase is complete and reviewed.

### Owned files and call sites

- `src/terminal_session.cpp`: builder/publisher for
  `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshots.
- `src/vnm_terminal_surface.cpp`: text-area wheel path that currently records
  deferred publication.
- `include/vnm_terminal/internal/vnm_terminal_surface_render_bridge.h` if bridge
  accessors need basis/purpose or projection state.
- `src/terminal_transcript.cpp` and transcript replay tool.
- Tests in `tests/surface_host`, `tests/backend_session`, `tests/render_snapshot`,
  and `tests/transcript`.

### Behavior

A public projection scroll snapshot:

- uses only public projection and public viewport data;
- becomes latest visible snapshot;
- does not advance live-content basis;
- is allowed while live content publication is blocked;
- marks full visible viewport dirty;
- validates under public projection rules.

Text-area wheel can use this path in internal immediate mode. Public surface
scroll APIs remain deferred until Phase 5.

Phase 4 must not render public scrollback from the Phase 1 viewport-only
scaffold. Before this behavior can scroll or render outside the copied
safe-basis viewport, Phase 4 must add a safe full public row source or
invalidate/defer off-viewport requests.

Phase 4 remediation has added the safe full public row source. It batches
entry-boundary row snapshots by viewport stride, verifies the entry-boundary
source against the latest safe content basis, stores retained-line fragment
ordinals for wrapped-line release reconciliation, and falls back to the
viewport-only projection if the source check fails. The fallback marks fragment
ordinals as viewport-relative only, so release reconciliation must not report
`exact_anchor` from fallback-only rows.

### Tests

- The existing synchronized-output wheel test fails before this phase and passes
  after: post-wheel visible snapshot advances immediately.
- Replay public scroll snapshot before release and live content snapshot after
  release in the same phase.
- Hidden live row rewrites do not appear in public scroll snapshots.
- Hidden active-grid rewrites do not appear near public tail.
- Hidden cursor movement, mode changes, style changes, hyperlink changes, buffer
  transitions, selection metadata, and validation metadata do not leak through
  the snapshot path.
- Hidden scrollback growth does not change public scroll bounds.
- Multiple wheel events accumulate visibly during a hold.
- `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshots do not advance live-content
  basis.
- Snapshot validation accepts valid public projection scroll snapshots and
  rejects malformed public projection snapshots.
- Existing selection payload remains copyable; visual spans are valid against the
  public projection or suppressed.
- No-payload hold permits immediate public scroll.
- High-frequency wheel input does not create unbounded queued public snapshots.
- Release after a public-projection scroll dirties the full live viewport.
- Effective synchronized-output scroll policy is latched per hold; mid-hold
  changes record `changed_mid_hold` diagnostics and apply to the next hold.

### Deferred-mode regression

- Default deferred text-area wheel still defers visible publication during a hold.
- Deferred transcript fixtures still replay.

### Review gate

Reviewers must confirm trace/replay evidence proves immediate public scrolling
without hidden content or metadata exposure.

## Phase 5: surface public APIs and app scrollbar integration

### Objectives

- Expose complete surface opt-in now that core behavior and replay are complete.
- Migrate surface local scroll APIs used by app scrollbar into the immediate
  policy path.
- Keep default deferred.

### Owned files and call sites

- `include/vnm_terminal/vnm_terminal_surface.h`: public property and API docs.
- `src/vnm_terminal_surface.cpp`: property implementation and public scroll API
  dispatch.
- `src/terminal_session.cpp`: public scroll API session path if needed.
- App repo `src/terminal_scrollbar.cpp`: scrollbar wheel, track, page, and thumb
  paths.
- App tests under `tests/terminal_app`.

### Behavior

The session now has internal latch plumbing for the effective hold policy.
Phase 5 still owns the public surface setter and app wiring. In immediate mode
during a hold:

- the effective scroll policy is latched at synchronized-output entry;
- changing the public property mid-hold records
  `synchronized_output_scroll_policy_changed_mid_hold` and affects only the next
  hold;
- a mid-hold change from deferred to immediate does not create a projection for
  the current hold;
- a mid-hold change from immediate to deferred does not destroy the active
  projection for the current hold;
- direct public viewport properties report public projection values;
- invalidation freezes public properties at last public values;
- public scroll APIs mutate public viewport when projection is valid;
- after invalidation, public scroll APIs record deferred intent and do not use
  hidden live bounds;
- scrollbar range/thumb use the same public basis as direct API readers;
- scrollbar wheel, track click, page scroll, and thumb drag reach the surface
  even when frozen public scrollbar state appears at a boundary, so deferred
  intent can be recorded.

Transcript route taxonomy uses source strings on `surface.scroll_intent` and
`surface.scroll`: `api.lines`, `api.offset`, `key.page`,
`surface.text_area.wheel`, `app.scrollbar.wheel`, `app.scrollbar.page`,
`app.scrollbar.track`, and `app.scrollbar.thumb`. In production app scrollbar
input, `app.scrollbar.page` is the plain track-page route and
`app.scrollbar.track` is the Ctrl-track absolute-position route. Wheel-trace
events use `surface.text_area.wheel` for direct text-area wheel input. The
replay tool uses the production-equivalent published-state path for
`surface.text_area.wheel`, the public line-scroll path for `key.page`,
`app.scrollbar.wheel`, and `app.scrollbar.page`, and the public offset path for
`api.offset`, `app.scrollbar.track`, and `app.scrollbar.thumb`. These strings
are replay taxonomy, not additional public scrolling mechanisms. Wheel-trace
`source` names identify ingress origin; wheel-trace `route` names identify the
chosen handler path.
Diagnostics keep visible scroll, deferred intent, and event acceptance separate;
`DEFERRED_INTENT_RECORDED` must not be treated as local/visible scroll applied.

### Tests

- Surface property docs and behavior for `scrollbackRows`,
  `viewportOffsetFromTail`, `viewportAtTail`, and `viewportVisibleRows` in
  immediate mode during a hold.
- Mid-hold policy change from deferred to immediate records the diagnostic and
  does not affect the current hold.
- Mid-hold policy change from immediate to deferred records the diagnostic and
  does not affect the current hold.
- The changed policy applies to the next synchronized-output hold.
- Direct API readers and app scrollbar observe the same frozen public basis after
  invalidation.
- Public surface scroll APIs publish public scroll snapshots while valid.
- Public surface scroll APIs record deferred intent after invalidation.
- Public surface scroll APIs return `true` when visible public scroll is applied.
- Public surface scroll APIs return the documented value when only deferred
  intent is recorded after invalidation, and diagnostics identify the deferred
  intent cause.
- Public surface scroll APIs return `false` when neither visible public scroll
  nor accepted deferred intent is applied.
- Scrollbar wheel, track click, page scroll, and thumb drag mutate public
  viewport while valid.
- Scrollbar input after invalidation reaches the surface and records deferred
  intent even if frozen scrollbar state is at a boundary.
- Hidden live scrollback growth does not move range/thumb before release.
- Release updates range/thumb from reconciled live state.
- Transcript capture/replay acceptance tests cover public
  `scroll_viewport_lines`, public `scroll_to_offset_from_tail`,
  `surface.text_area.wheel`, `key.page`, and app scrollbar
  wheel/track/page/thumb routes before exposure can be considered green.

### Deferred-mode regression

- Existing public surface scroll API behavior remains unchanged under default
  deferred policy.
- Existing scrollbar behavior remains unchanged under default deferred policy.

### Review gate

Reviewers must confirm public API and app scrollbar semantics match the
architecture, remain opt-in, and have capture/replay acceptance coverage for
both public surface scroll APIs and every app scrollbar route.

## Phase 6: app CLI flag and deterministic manual validation

### Objectives

- Add explicit user-facing opt-in for `vnm_terminal`.
- Add deterministic repro scripts alongside Codex manual validation.
- Keep the default deferred.

### Owned files and call sites

- App repo `src/main.cpp`: CLI parse/help/config wiring.
- App repo tests under `tests/terminal_app`.
- Surface/app manual validation docs or scripts.

### CLI

Add an explicit app flag such as:

```text
--synchronized-output-scroll-policy=defer
--synchronized-output-scroll-policy=immediate-public
```

The flag wires to `surface->set_synchronized_output_scroll_policy(...)`.

### Tests

- Help text lists the flag.
- Parser accepts valid values and rejects invalid values.
- App wiring sets the surface policy.
- Default app launch remains deferred.

### Manual validation

- Deterministic local synchronized-output script that emits safe prefix, enters
  hold, emits hidden changes, waits for wheel input, releases, then emits suffix.
- Active Codex output scenario remains a secondary manual validation scenario.
- Trace acceptance criteria:
  - effective policy is immediate for the hold;
  - public scroll snapshot appears before release;
  - hidden row/cursor/mode/style/hyperlink sentinels are absent before release;
  - release reconciliation result is recorded;
  - post-release suffix is absent from release snapshot and appears afterward.

### Review gate

Reviewers must confirm manual validation is reproducible without environment
variables and default app behavior remains deferred.

## Phase 7: performance, memory, and default-policy decision

### Objectives

- Decide whether immediate public projection remains opt-in or can become the
  default later.

### Required measurement methods

- Memory benchmark with configured scrollback limits and long synchronized-output
  holds.
- High-frequency wheel benchmark during a hold.
- Replay corpus covering old deferred transcripts and new immediate transcripts.
- Manual validation matrix with deterministic script plus at least one real TUI
  workload.

### Minimum thresholds before proposing default change

- No hidden content or metadata leak in focused tests, replay, or manual traces.
- Additional projection memory is bounded to one copied public projection:
  public scrollback limit plus active-grid rows, plus fixed metadata overhead.
- High-frequency wheel input does not produce unbounded snapshot queues.
- Wheel-to-visible-public-snapshot latency remains within one normal render tick
  under nominal load and does not show multi-second stalls under stress.
- Deferred-mode regression suite remains green.

### Review gate

Changing the default requires a separate reviewed proposal with measured numbers,
trace artifacts, and explicit approval. This implementation plan does not
approve a default change.

### Completion status: 2026-05-28

Phase 7 is complete for opt-in implementation and evaluation tooling. Evidence
is recorded in `docs/immediate_public_scroll_phase7_report.md` and the generated
benchmark JSON at `docs/immediate_public_scroll_phase7_benchmark_results.json`.
This completion status depends on the conservative decision to keep the default
deferred, which lowers the immediate evidence bar to opt-in readiness. Any
future default-change work reopens the manual validation matrix and transcript
corpus as gating evidence, not residual hygiene.

Decision: keep `DEFER_UNTIL_CONTENT_PUBLICATION` as the default. The focused
benchmark did not show hidden text/style/hyperlink/cursor/mode leakage or
unbounded public-scroll snapshot growth, but the copied projection still carried
a measurable memory cost at a realistic 10000-row scrollback limit, the
GUI/manual validation path remains documented rather than completed evidence,
and existing manual transcript captures are stale/mixed under current replay
diagnostics. Manual and corpus validation remain non-blocking only for the
current opt-in/default-deferred decision; they are gating evidence for any future
default proposal. A default change still requires the separate reviewed proposal
required by the review gate above.
