# Selection Lifetime Implementation

This document is the implementation and review plan for the contract in
[Selection lifetime design](selection_lifetime_design.md). It is a plan only;
it does not authorize source or test changes in this documentation step.
Until the phases complete and `docs/public_surface.md` is updated, the design
docs describe proposed behavior, not the current public API contract.

## Scope

The implementation goal is to make local terminal selection truthful:

- `selected_text()` and Ctrl+C copy the captured payload.
- Selection highlight spans are emitted only while visually attached to the
  published source snapshot or an explicitly compatible content basis.
- Visual detachment preserves the copy payload.
- The renderer remains passive.
- No render-time full-range selected-text comparison is introduced.

## Expected Touch Points

Selection and session:

- `include/vnm_terminal/internal/selection_contract.h`
- `src/selection_contract.cpp`
- `include/vnm_terminal/internal/terminal_session.h`
- `src/terminal_session.cpp`
- `include/vnm_terminal/internal/session_contract.h`, only if session command
  or result contracts need explicit selection lifetime fields.

Model and snapshot:

- `include/vnm_terminal/internal/terminal_screen_model.h`
- `src/terminal_screen_model.cpp`
- `include/vnm_terminal/internal/render_snapshot.h`

Surface and public boundary:

- `src/vnm_terminal_surface.cpp`
- `include/vnm_terminal/vnm_terminal_surface.h`, only if a public visual state
  is required.

Renderer pass-through:

- `include/vnm_terminal/internal/qsg_terminal_render_frame.h`, only if frame
  data needs shape changes.
- `src/qsg_terminal_renderer.cpp`, only for passive consumption of emitted
  spans.

Tests and docs after implementation:

- `tests/backend_session/backend_session_tests.cpp`
- `tests/surface_host/surface_host_tests.cpp`
- `tests/viewport/viewport_controller_tests.cpp`
- render or QSG tests if span emission/frame behavior needs direct coverage.
- `tests/CMakeLists.txt`, only if new test files are added.
- `docs/public_surface.md` and `docs/architecture.md`, after behavior changes
  are implemented and ready to publish.

## Evidence Gates

Each phase has before and after gates. A phase is not complete until its after
gate is recorded in the worker summary and reviewed.

Before evidence should identify:

- baseline commit or branch;
- focused tests or manual repro used for comparison;
- expected failing behavior before the phase;
- affected modules.

After evidence should identify:

- tests added or changed;
- tests run or intentionally not run;
- manual repro result when a relevant app build exists;
- performance observations when the phase can affect snapshot/render cost;
- review findings and resolutions.

## Phase 0 - Baseline And Payload Preservation

Before gate:

- Identify the branch baseline.
- Confirm whether the minimal payload-preservation patch is already present.
- Record the manual drift repro that motivates the change.

Work:

- Preserve the current minimal payload patch if it exists.
- Otherwise prepare a clean branch baseline before adding selection-lifetime
  changes.
- Do not broaden behavior in this phase.

After gate:

- The baseline commit is named in the phase summary.
- The payload-preservation behavior is either present or explicitly deferred to
  Phase 2 with a red test from Phase 1.
- No renderer or public API semantics changed.

## Phase 1 - Red Repro Tests

Before gate:

- Phase 0 baseline is recorded.
- The design contract has no unresolved blocker for test authoring.

Work:

- Phase 1 is test-only. Do not change production/source behavior in this
  phase; if a production hook is needed to observe a failure, split that hook
  into a later phase and keep the baseline-failure evidence intact.
- Inventory existing green tests for payload retention and selection-span
  behavior, then add only the missing red repros. The inventory must map each
  relevant contract item to an existing test name, an evidence level (`direct`,
  `indirect`, or `missing`), and the remaining gap. At minimum it must cover
  `selectionState`, payload retention, no-payload fallback, active empty
  selection, synchronized-output payload handling, detached visual state, drag
  preview, replacement cancellation, and resize/reflow behavior.
- Add failing tests for manual drift and selection highlight truthfulness where
  existing tests do not already cover the behavior.
- Add or identify tests that preserve copy payload after row mutation, scroll,
  reuse, and eviction.
- Add tests proving payload capture comes from the published source snapshot or
  a bit-equivalent source.
- Add synchronized-output tests proving hidden output is not selected while it
  is unpublished.
- Add resize and reflow tests proving visual detachment rather than stale
  highlight for incompatible or unproven geometry changes. Do not require
  detachment for geometry-only changes that preserve grid/content/mapping or
  for cases where a later phase supplies an explicit equivalence proof.
- Add scrollback-eviction tests proving payload survives while spans disappear.
- Add alternate-buffer tests proving spans do not cross buffer identity.
- Add Unicode tests for wide cells, trailing wide cells, combining marks, and
  variation sequences relevant to width.
- Add drag-across-snapshot-change tests proving the old anchor is not combined
  with an incompatible new extent.
- Add state tests for `selected_text()`, Ctrl+C copy, public `selectionState`,
  active empty payload, cancelled replacement returning to `NONE`, and render
  span emission.
- Add no-payload fallback tests proving that `NONE`, `DRAG_ARMED`, and
  cancelled replacement gestures do not consume copy shortcuts as local copies,
  while a committed empty selection does.

After gate:

- Each new red repro fails on the baseline for the intended reason, or the phase
  summary identifies existing green coverage and explains why no new red test
  is needed for that case.
- Every inventory item is disposed as one of: covered by an existing green test,
  covered by a new red repro, or intentionally deferred to a named later phase
  with the required production hook or evidence gate identified.
- The failures distinguish stale highlight from retained copy payload.
- No test requires render-time full-range selected-text comparison.
- No test relies on hidden synchronized output as a selectable source.

## Phase 2 - Internal State And Visual Lease Data

Before gate:

- Phase 1 red tests exist and are understood.
- No public API expansion has been accepted unless product use requires it.

Work:

- Introduce an internal selection state enum covering `NONE`, `DRAG_ARMED`,
  `DRAG_PREVIEW`, `ATTACHED_VISIBLE`, `ATTACHED_HIDDEN`, and `PAYLOAD_ONLY`.
- Add a visual lease structure that records source content basis, session epoch,
  buffer id, grid/reflow basis, viewport mapping, selected range, payload, and
  optional row or cell descriptors.
- The recorded source identity must be a content-basis identity. Do not use a
  whole render-frame generation as the sole identity if it changes for
  cursor-only, overlay-only, renderer-cache, font/DPR, or pixel-geometry-only
  updates.
- Define the concrete content-basis owner, field/type name, producer module, and
  increment/preserve matrix before later phases consume the identity. The matrix
  must cover terminal content mutation, cursor-only updates, viewport scroll,
  synchronized-output hold/release, resize/reflow, font/DPR changes, alternate
  buffer transitions, scrollback append, and scrollback eviction.
- Keep durable payload storage separate from visual attachment.
- Keep `selectionState` as the copyability property unless a separate visual
  state is deliberately added.
- Do not route selection semantics through QSG or renderer caches.
- Lock replacement gestures to `mouse-down clears`: arming a replacement
  selection clears the prior durable payload immediately. `commit replaces` is
  future work unless separately approved before implementation begins.

After gate:

- State-transition tests cover clear, replace, drag start, drag preview,
  finalize, detach, hide, and session reset.
- `selected_text()` and Ctrl+C use the same payload source.
- `PAYLOAD_ONLY` keeps `selectionState` active while emitting no spans.
- The replacement gesture policy is covered by tests for empty click, cancelled
  drag, active empty committed selection, and non-empty replacement.
- The content-basis field/type and increment/preserve matrix are documented in
  the phase summary and reviewed before Phase 3 begins.
- Public API changes, if any, are explicitly justified and reviewed.

## Phase 3 - Bind Surface Drag To Published Snapshot Source

Before gate:

- Internal state and lease types exist.
- The surface has access to the published snapshot identity used for hit-tests.

Work:

- Record the published snapshot source when a local selection drag is armed.
- Perform drag hit-testing against published snapshots only.
- Reject or detach drag preview when anchor and extent sources are not
  compatible.
- On mouse release, finalize from the compatible source or promote the last
  valid preview payload, including a valid empty payload, to `PAYLOAD_ONLY`.
- Avoid mixing a hit-test from one snapshot with text extraction from hidden or
  later model state.

After gate:

- Surface-host tests prove drag selection does not cross incompatible snapshot
  changes.
- Manual drag during active output either stays attached truthfully or detaches
  while keeping the payload.
- Hidden synchronized output remains unselectable until published.

## Phase 4 - Snapshot Payload Extraction Or Bit-Equivalence Proof

Before gate:

- Drag source binding is in place.
- Tests identify all paths that produce a selection payload.

Work:

- Prefer extracting payload from the `Terminal_render_snapshot` content used
  for hit-testing.
- If model extraction is retained, add an explicit bit-equivalence proof:
  matching session epoch, active buffer, snapshot/content basis, grid, viewport
  mapping, selected row descriptors, and selected cluster descriptors.
- Normalize endpoints through terminal-cell cluster boundaries.
- Preserve wide glyph and combining-mark behavior from the Unicode width
  policy.
- Do not call `selected_text()` over the full selected range during render
  snapshot evaluation.

After gate:

- Payload tests pass for visible rows, scrollback rows, synchronized-output
  holds, alternate buffer, wide cells, and combining marks.
- A code reviewer can point to the proof that payload source equals hit-test
  source.
- Large-selection render paths do not extract or compare the full payload per
  snapshot.

## Phase 5 - Emit Spans Only For Attached Visible Leases

Before gate:

- Payload source is implemented.
- Content-basis identity and visual lease data are implemented.
- Tests can observe render snapshot `selection_spans`.

Work:

- Implement the comprehensive visual lease compatibility predicate and make the
  session or snapshot producer emit `selection_spans` only for
  `DRAG_PREVIEW` and `ATTACHED_VISIBLE` with a compatible visual lease.
- Emit no spans for `NONE`, `DRAG_ARMED`, `ATTACHED_HIDDEN`, or
  `PAYLOAD_ONLY`.
- Re-evaluate compatibility on synchronized-output release, viewport scroll,
  active output, row mutation, scrollback eviction, alternate-buffer switch,
  resize, and reflow.
- Leave the renderer as a passive consumer of spans.

After gate:

- Tests prove stale spans disappear before they can cover text different from
  the copy payload.
- Cursor-only and paint-only updates preserve attachment.
- Viewport scroll can hide and, when compatible, show the same visual lease
  without changing payload.
- Renderer changes, if any, are limited to passive span/frame handling.

## Phase 6 - Validation, Manual Repro, And Performance

Before gate:

- Phases 1 through 5 have passed review.
- A relevant app or surface-host build target is identified for manual testing.
- The manual repro record names the app target, launch command shape, tracing
  location, exact input sequence, visual observation method, and
  clipboard/`selected_text()` assertion method. For the Varinomics app, the
  expected target is the `vnm_terminal` validation app built against this local
  `vnm_terminal_surface` checkout, launched with the Qt runtime on `PATH` and
  Codex started with `--dangerously-bypass-approvals-and-sandbox`.

Work:

- Run focused automated tests for selection, session, surface host, viewport,
  render snapshot, QSG frame behavior, and Unicode selection cases.
- Run the manual Codex repro after the relevant app build: start Codex with
  tracing, select visible text, mutate or scroll the backing rows, confirm the
  highlight hides before covering replacement text, and confirm Ctrl+C still
  copies the original payload until clear or replacement.
- Repeat the manual repro for synchronized output, resize/reflow, scrollback
  eviction, alternate buffer, and wide/combining text.
- Check large scrollback selections for snapshot/render cost. The render path
  must not scale with the full selected text payload on every snapshot.

After gate:

- Focused tests pass.
- Manual repro results are recorded with the build target and command shape.
- Performance notes show no render-time full-range selected-text comparison.
- The implementation is ready for public docs updates.

## No-Go Criteria

- Render-time `selected_text()` comparison across the full selected range.
- Extracting or copying a committed payload from hidden synchronized-output
  state, a later screen-model state, or any source other than the published
  hit-test source without a tested bit-equivalence proof.
- Renderer or QSG node ownership of selection semantics.
- User selection of hidden synchronized output that has not been published.
- Stale highlight silently covering text different from the copy payload.
- Clearing a durable copy payload merely because visual attachment is lost.
- Treating dirty rows as semantic identity.
- Public API expansion before internal state and host need are demonstrated.
- Tests that pass by accepting stale visual spans.
- Long-running output paths that do O(full selected range) work per snapshot.

## Rollback Criteria

Rollback the phase if any of these are introduced:

- Ctrl+C or `selected_text()` loses the retained payload without explicit clear,
  replacement, or session reset.
- A selection span can be observed over text that differs from the copy payload.
- Hidden synchronized output becomes selectable before publication.
- Large scrollback selection causes per-snapshot work proportional to the full
  selected range.
- Renderer code starts making lease-validity decisions.
- Unicode selection duplicates a wide glyph, drops combining marks, or selects a
  trailing wide cell as independent text.
- Public `selectionState` changes meaning without a reviewed public contract.

## Review Protocol

Implementation should be worker-led one phase at a time.

For each phase:

- The worker implements only that phase's scope.
- The worker records before and after evidence in the phase summary.
- Run three xhigh read-only reviews after the worker patch.
- Read-only reviewers do not edit the branch. They report design-contract
  violations, missing evidence, no-go criteria, performance risks, and test
  gaps.
- The worker resolves findings. Material rewrites repeat the three-review pass.
- After phases that affect surface behavior, run the manual test after the
  relevant app build.

The phase is complete only when the worker summary, automated evidence, manual
evidence where applicable, and three xhigh reviews agree that the phase meets
the design contract.
