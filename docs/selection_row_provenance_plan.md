# Selection Row Provenance Plan

## Status

Phase 6 documentation readiness update, May 2026.

Phases 1a, 1b, 1c, 2, 3, 4a, 4b, 4c, and 5 are implemented and reviewed. This
document is now the retained-line provenance design record and final
implementation-status summary for that work. Phase 6 is a docs/manual-validation
readiness phase; it does not authorize production changes.

Implemented architecture:

- The model owns retained-line provenance. Each retained row carries a
  `retained_line_id` and row-level `content_generation`.
- Row moves preserve retained-line provenance across screen, scrollback, saved
  buffer state, and supported lifecycle operations; newly created or reused rows
  receive fresh identity, and evicted/discarded rows retire identity.
- Render snapshots publish visible retained-line provenance through
  `visible_line_provenance` using `Terminal_render_line_provenance`.
- Selection visual leases capture selected retained-line descriptors through
  `terminal_selection_line_lease_t` entries in the lease.
- Visual lease advancement and span emission are driven by retained-line
  provenance proof. Dirty-row proof is retained for render damage and
  diagnostics only.
- Phase 4a preserves visible spans across scrollback growth when selected
  retained lines are still retained and unchanged.
- Phase 4b preserves spans across synchronized-output release when selected
  retained lines are unchanged in the released content.
- Phase 4c covers fail-closed boundaries, including active-buffer switches,
  alternate-buffer provenance separation, selected-line discard/purge, and
  resize/reflow limits.
- Phase 5 retires dirty-row semantic advancement. Dirty rows remain render
  damage metadata and diagnostic context, but they are not the semantic source
  of selection truth.
- Phase 5 viewport projection remains explicit opt-in through
  `Terminal_session_config::selection_viewport_projection_enabled`; the default
  remains false.
- V1 retained-line validation is intentionally coordinate-strict: a selected
  descriptor must still resolve at `selection_start.row + row_offset`.
  Selected-row movement/reorder and row-origin eviction detach instead of
  projecting spans by retained-line ID.
- Optional row modification hover metadata was not exposed in this phase. It
  remains future work after provenance lifecycle behavior is stable.

## Background

The current selection lifetime design is intentionally fail-closed. Copy payloads
are durable, but visible selection spans are emitted only while the terminal can
prove that the selected text still corresponds to the visible terminal content.

Recent manual testing with traced Codex output showed that the remaining
selection disappearance is not a renderer repaint bug. The session removes
`selection_spans` before rendering. The decisive trace shape was:

- `dirty_rows_overlap_selection=false`
- `dirty_rows_proof=unstable-mutation-identity`
- `viewport_mapping_match=false`
- `selection_span_count=0`

This means active output changed content and viewport/scrollback mapping in a
way that made the current dirty-row proof ambiguous. The selected row may look
stationary to the user, but the model cannot prove that it is the same retained
line with unchanged selection-relevant content, so the visual lease detaches.

The retained-line provenance mechanism described here has been implemented to
address that product evidence. Conservative detachment remains required when the
model cannot prove retained-line identity and content stability, but dirty-row
ambiguity alone no longer detaches a visual lease when retained-line proof
matches.

## Objectives

1. Preserve visible selection spans during active output when selected retained
   lines still have the same retained-line identity and row-level content
   generation.
2. Continue to fail closed when selected retained lines are overwritten, reused,
   evicted, discarded, reflowed without proof, or switched to a different active
   buffer.
3. Keep copy payload durability independent from visual span attachment.
4. Avoid large text comparisons over selected ranges on streaming updates.
5. Keep the renderer passive: the model/session decide selection spans; QSG only
   draws them.
6. Provide a foundation for a future row modification date/time hover feature
   without using wall-clock timestamps as correctness proof.

## Non-goals

1. Do not preserve visual selection across column-count reflow in the first
   implementation.
2. Do not implement cell-level or cluster-level provenance initially.
3. Do not use wall-clock timestamps to prove row identity or content stability.
4. Do not make dirty rows the semantic source of selection truth.
5. Do not move selection validity into the renderer or QSG layer.
6. Do not extend surface-side text comparison as a correctness proof.
7. Do not guarantee v1 preservation for partial-row selections when unselected
   columns on the same retained line change. Row-level generation may detach
   conservatively.
8. Do not automatically reattach visual spans across active-buffer switches in
   v1, even if saved primary-buffer provenance is restored later.

## Terminology

`viewport row` means a visible screen coordinate.

`logical row` means the current coordinate in retained terminal history, using
existing snapshot viewport semantics.

`retained line` means the model-owned identity that moves with retained row
content across screen rows, scrollback rows, and saved buffer state.

`retained_line_id` is identity.

`logical_row`, `viewport_row`, and `row_offset` are coordinates, not identity.

The plan should avoid names such as `row_instance_id` if they can be mistaken
for physical viewport slots or storage slots.

## Core design

The implemented architecture adds model-owned retained-line provenance for every
retained terminal row.

Correctness data follows this shape:

```cpp
struct Terminal_retained_line_provenance
{
    std::uint64_t retained_line_id = 0;
    std::uint64_t content_generation = 0;
};
```

Display-only data should live in a separate structure so it cannot accidentally
enter compatibility predicates:

```cpp
struct Terminal_retained_line_display_metadata
{
    std::chrono::system_clock::time_point created_time;
    std::chrono::system_clock::time_point last_modified_time;
};
```

Optional later correctness or display fields:

```cpp
std::uint64_t style_generation = 0;
std::uint64_t row_digest = 0;
```

Correctness uses `retained_line_id` and `content_generation`.

Wall-clock display metadata can support a future mouseover modification-time UI,
but must not be used by selection compatibility.

## Semantics

`retained_line_id` is stable for a retained terminal row/line.

- It is created when a new retained row is born.
- It moves with that retained row when rows scroll, shift, or enter scrollback.
- It is never reused after eviction or discard.
- It is scoped to the model/session identity and must not be confused across
  active buffers.

`content_generation` is a row-level selection-relevant content generation.

- It increments only when the row's selection-relevant cell state actually
  changes: text, cell occupancy, wide-cell state, combining sequence, variation
  sequence, wrap-affecting content, erase state, insert/delete-cell shifts, or
  other content-equivalent row changes.
- Idempotent writes that leave the row's selection-relevant cell state exactly
  unchanged should not increment it.
- Cursor-only updates do not increment it.
- Style-only behavior can start conservative if the implementation cannot yet
  separate style from content, but the target behavior is that style-only changes
  do not invalidate selected text. A later `style_generation` can track display
  metadata separately.
- Under-incrementing is a stale-highlight correctness bug.
- Over-incrementing is fail-closed and acceptable temporarily, but it reduces the
  benefit and must be visible in tests or traces.

## Storage strategy

The default implementation strategy is to co-locate provenance with row storage,
not to maintain a long-lived parallel provenance vector.

A row wrapper is preferred because current scroll, insert/delete, and saved-state
paths move raw `std::vector<Cell>` rows directly. Co-located row state makes C++
row moves carry provenance by construction.

A temporary parallel-array scaffold is allowed only if it has a named removal
point before Phase 4 and every row movement site is audited in the same phase.

## Model lifecycle rules

### Appending and writing

Writing to an existing retained line preserves `retained_line_id` and increments
`content_generation` only when selection-relevant row content changes.

All text-writing paths, erase paths, insert/delete-cell paths, wide-cell repair
paths, and row-clear paths must go through a centralized generation-update helper
or be explicitly audited.

### Primary scroll up

Rows moved upward keep provenance.

The row pushed into scrollback keeps its provenance.

The new blank bottom row receives a fresh `retained_line_id`.

### Primary scroll down and reverse index

Rows moved downward keep provenance.

The row displaced out of the scroll region or screen retires its
`retained_line_id` unless it is retained elsewhere by an explicit model rule.

The new blank inserted row receives a fresh `retained_line_id`.

### Scroll regions

Scroll-region operations must be specified explicitly and tested. This includes
`IND`, `RI`, `SU`, `SD`, `IL`, and `DL` inside margins.

General rule:

- retained content rows moved within the scroll region keep provenance
- rows discarded from the region retire their IDs unless retained in scrollback
- rows moved into scrollback keep provenance
- new blank rows created by the operation receive fresh IDs
- if selected retained lines are displaced, discarded, or ambiguously remapped,
  visual spans detach and payload remains

### Scrollback growth

The scrollback row keeps the provenance of the screen row that moved into
scrollback.

Existing retained lines keep their provenance even though logical row numbers and
viewport mappings change.

### Scrollback eviction

Evicted retained lines retire their IDs.

If a selected retained line is evicted, visual spans detach and payload remains.

If eviction happens before selected retained lines, v1 remains conservative:
row-origin changes detach visual spans even when retained provenance for the
selected lines might theoretically still match after coordinate adjustment. This
avoids projecting selection spans across an unproven origin shift.

### Zero scrollback

Rows discarded by scrolling retire their IDs.

Rows newly created in reused storage receive fresh IDs before any cell write.

Identical text must not preserve selection if the row is a different retained
line.

### Insert/delete line

Moved retained lines keep provenance where the operation semantically moves row
content.

New inserted blank rows receive fresh IDs.

Rows deleted or displaced out of retention retire IDs.

V1 selection validation is stricter than retained-line movement: if an
insert/delete-line operation moves, reorders, or ambiguously remaps a selected
retained line away from its expected logical row, visual spans detach
conservatively.

### Insert/delete cells and erase characters

Intra-row column shifts and erase-character operations change row content unless
they are proven idempotent. They must update `content_generation` accordingly.

### Erase, clear, and reset

Affected retained lines preserve `retained_line_id` but increment
`content_generation` if their selection-relevant content changes.

Clear-scrollback operations retire provenance for purged scrollback rows and must
detach visual spans if any selected retained line was purged.

Soft reset, hard reset, clear-screen variants, and full buffer replacement must
be classified in implementation. If classification is ambiguous, allocate fresh
provenance for affected rows and detach visual spans.

### Primary repaint recovery

Primary repaint recovery must not silently copy cell rows without provenance.

V1 rule: rows reconstructed or inferred by primary repaint recovery receive fresh
provenance unless the implementation has an explicit proof that the captured row
object and content state are unchanged. This is fail-closed and may detach
visual spans.

If future work wants preservation through recovery, the recovery candidate must
capture provenance together with cells and prove that it still describes the same
retained content.

### Alternate buffer

Primary and alternate buffers must have non-confused provenance.

Saved buffer state must either save provenance in lockstep with cells or
regenerate provenance on restore. V1 rule: save provenance with saved cells so
model state remains internally consistent, but active-buffer switches still
force visual detachment. Payload may remain.

Returning from alternate buffer must not automatically reattach visual spans in
v1. If this becomes a product goal, it requires a separate reviewed phase.

### Resize and reflow

Column-count reflow remains a hard visual-lease boundary in v1.

Height-only or width-preserving resize can preserve retained-line provenance for
surviving rows. Rows newly exposed or newly created receive fresh IDs. Rows
discarded by shrinking retention retire IDs.

If any resize path cannot prove retained-line continuity, it must detach visual
spans conservatively.

### Synchronized output

Hidden synchronized mutations update retained-line provenance as they happen.

While publication is blocked, hidden content is not selectable.

On release, selection compatibility is decided from final published retained-line
provenance, not from aggregated dirty row indexes.

A synchronized release that only changes unrelated retained lines should preserve
selection by retained-line proof. A release that changes selected retained-line
content must detach visual spans.

## Retained-line lookup and eviction status

Visible snapshot provenance alone is not enough to distinguish offscreen retained
lines from evicted/discarded lines.

The final stale-span guard must be model-side validation of selection requests
against the full retained-line set. The model owns retained-line storage and is
the only component that can reliably distinguish visible, offscreen, evicted,
discarded, and replaced lines.

Supporting APIs may include:

1. a retained-line lookup by `retained_line_id` that returns current logical row,
   buffer, and content generation if retained
2. an explicit list or set of evicted/discarded retained-line IDs in model
   results

These supporting APIs do not replace the final model-side request validator.

## Snapshot design

Expose visible retained-line provenance in render snapshots for diagnostics,
lease capture, and visible span construction.

Implemented snapshot field:

```cpp
std::vector<Terminal_render_line_provenance> visible_line_provenance;
```

Each entry corresponds to the vector index's viewport row. Avoid storing a
separate `viewport_row` unless it is explicitly debug-only.

```cpp
struct Terminal_render_line_provenance
{
    std::int64_t logical_row = 0;
    std::uint64_t retained_line_id = 0;
    std::uint64_t content_generation = 0;
};
```

Validation rules for content snapshots:

- `visible_line_provenance.size() == grid_size.rows`
- each entry maps to the corresponding visible row by vector index
- each entry has nonzero provenance for real retained rows

Non-content or adapter snapshots must be specified explicitly:

- empty/unstarted snapshots may have no provenance and must emit no selection
  spans
- geometry-only snapshots derived from an existing public snapshot must copy the
  previous provenance if they preserve previous content
- selection-only snapshots that reuse previous content must preserve previous
  provenance
- any snapshot path unable to provide valid provenance must clear or suppress
  selection spans fail-closed

## Visual lease design

Extend `terminal_selection_visual_lease_t` with selected retained-line
descriptors.

Implemented lease descriptor:

```cpp
struct terminal_selection_line_lease_t
{
    int row_offset = 0;
    std::uint64_t retained_line_id = 0;
    std::uint64_t content_generation = 0;
};
```

The lease stores one descriptor per normalized selected logical row. `row_offset`
is relative to the normalized selection start row. In v1 it is a strict
coordinate expectation: the descriptor must still describe the retained line at
`selection_start.row + row_offset`. It is not identity, and v1 does not search
by ID to project moved selected rows.

The lease still stores existing source identity, selected range, durable payload
identity, and provisional payload identity. Retained-line provenance supplements
existing proof; it does not replace payload identity.

## Selection request and span emission rule

Model-side provenance validation is mandatory.

The snapshot request must not remain a plain vector of selection ranges. It
should carry expected retained-line descriptors with each requested selection.

Example shape:

```cpp
struct Terminal_render_selection_request
{
    Terminal_selection_range range;
    std::vector<terminal_selection_line_lease_t> expected_lines;
};
```

The model validates each request against its full retained-line set before
constructing spans.

Validation rule:

- each lease descriptor is checked at `selection_start.row + row_offset`
- the retained line at that logical row must match both `retained_line_id` and
  `content_generation`
- matching by `retained_line_id` alone is not enough in v1; the implementation
  does not search retained storage for moved or reordered selected rows
- if the expected retained line is visible at that validated row, emit the
  corresponding visible span fragment
- if a validated retained line is retained but offscreen, emit no span for that
  line but keep visual attachment eligible
- if the expected row is missing, evicted, discarded, moved, reordered, or has a
  different generation, suppress spans and report mismatch so the session can
  detach to payload-only

For multi-row selections, all selected retained lines must be retained and
unchanged before any visible fragment is emitted. This avoids partial stale
highlights when an offscreen selected line was evicted or changed.

Keep existing outer gates:

- session epoch is compatible
- active buffer is compatible
- grid/reflow basis is compatible
- selection range is valid
- payload identity is compatible

The current dirty-row proof can remain as a transition diagnostic or fast path,
but it must not be the semantic authority once retained-line proof is available.

A shared provenance compatibility helper must produce the proof result used by
surface/session/model-facing paths. Avoid duplicating hand-coded predicates in
surface event handling, session lease advancement, and model span construction.

## Failure reproduction policy

The strict failure-before-fix policy applies to behavior-changing phases.

For behavior-changing phases:

1. Add or identify a focused failing test.
2. Run the test on the pre-change baseline or with the new behavior disabled.
3. Record the failing assertion or trace signature.
4. Apply the implementation.
5. Run the same test and record the passing result.
6. Review the test and implementation before moving to the next phase.

If a behavior test cannot be made to fail before the fix, it is not accepted as
proof of that fix. It can still be retained as coverage, but not as evidence.

For additive data phases, tests are coverage and invariant checks, not proof of a
behavioral fix. Those phases must preserve current behavior.

## Phased implementation plan

The phase entries below preserve the original design gates. The following
outcome summary records the implemented state after review:

- Phase 1a migrated row storage to co-located retained-line state so cell rows
  and provenance move together.
- Phase 1b added retained-line ID allocation, movement, and retirement.
- Phase 1c added row-level content-generation mutation and idempotence rules.
- Phase 2 published visible retained-line provenance in render snapshots and
  validated snapshot provenance shape.
- Phase 3 captured retained-line descriptors in selection visual leases.
- Phase 4a enabled provenance-based preservation across scrollback growth when
  selected descriptors still resolve at their expected logical rows.
- Phase 4b enabled provenance-based preservation across synchronized-output
  release.
- Phase 4c locked down fail-closed edge cases and buffer boundaries before
  dirty-row semantic retirement.
- Phase 5 retired dirty-row semantic advancement. Dirty rows remain render
  damage and diagnostic metadata.
- Phase 6 adds documentation readiness and manual validation material. It does
  not add hover metadata or change production behavior.

### Phase 0: Baseline and documentation

Goals:

- Keep the current implementation unchanged.
- Preserve this design document as the controlling plan.
- Add trace timestamps only if a dedicated diagnostics patch is approved and
  reviewed. Otherwise leave timestamps out of this plan.

Tests:

- No behavior tests required.
- Manual trace evidence already demonstrates the current over-detach failure.

Gate:

- This plan must pass Codex and Claude review before implementation starts.

### Phase 1a: Migrate row storage to co-located retained-line state

Goals:

- Introduce co-located row storage that can carry cells plus retained-line
  provenance. The intended shape is a `Terminal_screen_row`-style wrapper that
  owns the row cells and an inert `Terminal_retained_line_provenance` member.
- Preserve existing terminal behavior and existing selection behavior.
- Avoid long-lived parallel provenance arrays.
- Keep retained-line identity semantics inert and zero-initialized until
  Phase 1b. Phase 1a must not allocate, move, retire, expose, validate, or
  mutate real retained-line identities.
- Make row moves carry row-owned metadata by construction.

Implementation touch points:

- live screen row storage
- scrollback row storage
- extend `scrollback_row_t` in place or wrap it equivalently; do not create a
  separate parallel provenance container
- saved buffer state
- primary repaint recovery state
- grid resize / cell resize paths that rebuild row storage
- any helper or adapter that currently assumes a raw `std::vector<Cell>` row

Tests are coverage/invariant tests, not proof of behavior fix:

- existing screen model behavior remains unchanged
- row wrapper moves preserve cell content
- scrollback rows preserve cell content
- saved buffer state preserves/restores cell content
- primary repaint recovery remains fail-closed or behavior-equivalent

Rollback:

- Phase 1a can be reverted independently if row-wrapper migration destabilizes
  existing model behavior. The gate is behavior-equivalence: relevant existing
  tests must remain green, and reviewers should reject any Phase 1a diff that
  starts ID allocation, generation updates, snapshot publication, or selection
  compatibility changes.

### Phase 1b: Add retained-line ID allocation, movement, and retirement

Goals:

- Allocate fresh retained-line IDs for new retained rows.
- Move retained-line IDs with retained content rows.
- Retire IDs when retained lines are evicted, discarded, purged, or replaced.
- Preserve existing selection behavior.

Implementation touch points:

- scroll-region operations
- insert/delete-line operations
- resize paths
- scrollback append eviction
- manual `set_scrollback_limit` eviction
- clear-scrollback paths
- alternate-buffer save/restore
- zero-scrollback discard/reuse
- primary repaint recovery

Tests are coverage/invariant tests, not proof of behavior fix:

- retained IDs are stable across scroll into scrollback
- new rows get new IDs
- streaming scrollback eviction retires IDs
- manual `set_scrollback_limit` eviction retires IDs
- zero scrollback reuses storage with fresh IDs
- partial scroll regions retire displaced rows and preserve moved rows
- clear scrollback retires purged IDs
- primary repaint recovery is fail-closed with fresh IDs unless explicitly proven
- alternate buffer provenance is saved/restored consistently but not visually
  reattached

Rollback:

- Phase 1b can be reverted without changing selection behavior if lifecycle tests
  expose unsafe ID movement or retirement.

### Phase 1c: Add content-generation mutation and idempotence rules

Goals:

- Increment retained-line `content_generation` only when selection-relevant row
  content actually changes.
- Keep cursor-only and idempotent row writes generation-stable.
- Preserve existing selection behavior.

Implementation touch points:

- row mutation helpers
- printable/write paths
- erase/clear paths
- insert/delete-cell paths
- wide-cell repair paths
- combining/variation sequence paths
- wrap-affecting content paths
- style-only paths, if they currently share content mutation helpers

Tests are coverage/invariant tests, not proof of behavior fix:

- row content mutation increments generation only on actual content change
- idempotent write does not increment generation
- cursor-only update does not increment content generation
- insert/delete-cell content shifts increment generation
- erase-character changes increment only when content changes
- wide-cell occupancy changes increment generation
- combining/variation sequence changes increment generation
- style-only change is either proven generation-stable or documented as
  temporarily fail-closed

Rollback:

- Phase 1c can be reverted without changing selection behavior if generation
  under-increment or excessive over-increment risks are found.
### Phase 2: Publish retained-line provenance in render snapshots

Goals:

- Add visible retained-line provenance to render snapshots.
- Define snapshot provenance availability for content, empty, geometry-only, and
  selection-only snapshots.
- Validate snapshot provenance shape.
- Keep selection behavior unchanged.

Implementation touch points:

- `Terminal_render_snapshot`
- snapshot construction
- geometry snapshot adapters
- selection-only snapshot reuse paths
- empty snapshot helpers
- snapshot validation
- tests using render snapshots

Tests are coverage/invariant tests:

- content snapshot has one provenance descriptor per visible row
- descriptors match visible logical rows
- provenance changes only when lifecycle rules require it
- cursor-only snapshot preserves provenance
- geometry-only and selection-only snapshots preserve or suppress provenance by
  documented rule
- malformed provenance vectors fail validation

Rollback:

- Phase 2 can be reverted without behavior change if snapshot adapter paths are
  not ready.

### Phase 3: Capture retained-line provenance in selection visual leases

Goals:

- Capture selected retained-line descriptors when a selection is committed or
  previewed.
- Keep emission behavior unchanged unless only diagnostics are added.

Implementation touch points:

- selection visual lease data structure
- selection range commit path
- drained published source path
- drag preview/selection source path
- trace diagnostics

Tests are coverage/invariant tests:

- selected descriptors match the snapshot used for payload extraction
- multi-row selection stores one descriptor per selected logical row
- drained published source path captures descriptors from the drained snapshot
- selected-row mutation still detaches under current behavior
- unrelated-row mutation behavior remains unchanged in this phase

Rollback:

- Phase 3 can be reverted without behavior change if lease descriptor capture is
  wrong.

### Phase 4a: Preserve spans across scrollback growth with stable retained lines

Goals:

- Preserve visual spans when active output changes viewport/scrollback mapping
  but selected retained lines remain retained and unchanged.
- Add model-side request validation for retained-line descriptors.
- Keep dirty-row proof available only as diagnostic/transition support.

Implementation touch points:

- selection request shape
- model-side request validation
- span emission suppression logic
- lease source advancement
- shared provenance compatibility helper
- trace field such as `preservation_reason=provenance-match`

Required pre-fix failure evidence:

- active-output scrollback-growth preservation test detaches with
  `viewport_mapping_match=false`

Tests:

- select a stable visible row and append active output that grows scrollback;
  spans stay
- select rows in scrollback and stream output at tail; payload and reattachment
  survive if selected retained lines are still retained
- selected scrollback row evicted by later output detaches visual spans and
  preserves payload
- selected-row content mutation detaches visual spans and preserves payload
- selected-row idempotent rewrite to identical cell state preserves generation
  and therefore may preserve spans
- replacement row with identical text but different retained-line ID detaches
- zero scrollback discarded selected row detaches
- reflow still detaches

Rollback:

- Phase 4a must have a single switch or revert boundary restoring old dirty-row
  behavior if stale-span negatives fail.

### Phase 4b: Preserve spans across synchronized-output release by provenance

Goals:

- Preserve visual spans after synchronized-output release when selected retained
  lines were not changed.
- Keep hidden synchronized output unselectable before publication.

Required pre-fix failure evidence:

- synchronized-output preservation test detaches with
  `dirty_rows_proof=unstable-mutation-identity`

Tests:

- synchronized output release that grows scrollback but does not touch selected
  retained lines preserves spans
- synchronized output release that scrolls a selected retained line into
  scrollback without mutating it preserves or reattaches spans when visible
- synchronized output release that mutates selected retained-line content
  detaches visual spans and preserves payload
- synchronized output release with selected retained-line eviction detaches

Rollback:

- Phase 4b can be reverted independently from Phase 4a if synchronized output
  proves unsafe.

### Phase 4c: Fail-closed edge cases and buffer boundaries

Goals:

- Lock down negative cases before retiring old semantic shortcuts.

Tests:

- active-buffer switch detaches visual spans and does not automatically reattach
  after return
- alternate buffer provenance cannot collide with primary provenance
- release-time worker-output drain preserves spans when retained-line proof
  matches; pre-fix signature should be recorded if this is used as proof
- partial scroll-region displacement detaches when selected retained lines are
  discarded or ambiguously remapped
- clear scrollback detaches if selected retained lines were purged
- height-only resize preserves retained-line IDs for surviving rows; column
  reflow detaches

Rollback:

- Phase 5 cannot start until all Phase 4c stale-span negative tests are green.

### Phase 5: Simplify or retire dirty-row semantic proof

Goals:

- Keep dirty rows for render damage.
- Remove or narrow selection validity paths that treat dirty rows as semantic
  identity proof.
- Keep diagnostics for dirty-row ambiguity during transition.

Tests:

- existing dirty-row preservation tests pass through retained-line provenance
- dirty-row ambiguity no longer causes detach when retained-line proof matches
- stale-span negative tests remain green
- trace shows `preservation_reason=provenance-match` where appropriate and no
  longer depends on dirty-row proof as the only preservation reason

Required pre-fix evidence:

- tests that assert provenance is the preservation reason fail or show the old
  dirty-row-only reason before this phase

Rollback:

- Do not start Phase 5 until Phase 4a through Phase 4c have passed reviews and
  manual testing.

### Phase 6: Documentation, manual validation, and optional hover preparation

Status:

- Documentation readiness phase.
- No production code changes are planned.
- `last_modified_time` hover exposure is deferred; it is not part of the Phase 6
  completed scope.

Goals:

- Update selection lifetime docs to describe implemented retained-line
  provenance and no longer describe it as only deferred future work.
- Add a concise manual validation checklist for the original Codex
  selection-drift/vanishing scenario.
- Record that dirty rows remain useful for render damage and diagnostics but are
  not the semantic source of visual lease truth.
- Keep Phase 5 viewport projection documented as explicit opt-in/default false.

Validation:

- Automated tests are not required for this docs-only Phase 6 patch.
- Previous focused selection, backend session, surface host, viewport, render
  snapshot, QSG, and Unicode selection tests remain the automated evidence base.
- Manual validation should follow
  [Selection row provenance manual validation](selection_row_provenance_manual_validation.md).

Required evidence:

- documentation no longer describes retained-line provenance as only deferred
  future work
- manual Codex active-output selection stays visible while selected retained
  lines are unchanged and retained
- visual detachment still preserves copy payload when retained-line proof fails
- manual trace capture, when enabled, shows dirty-row ambiguity as diagnostic
  context rather than the sole semantic preservation gate

## Review process

Before implementation:

1. Review this plan with at least three Codex agents at xhigh.
2. Review this plan with two Claude sessions.
3. Consolidate findings.
4. Patch the plan.
5. Repeat review until there are no functional design blockers.

During implementation:

1. Each phase is assigned to a worker agent at xhigh.
2. The main thread orchestrates and does not implement non-trivial code.
3. Each behavior-changing phase must include tests and failure-before-fix
   evidence.
4. Each phase receives at least two Codex reviews and one Claude review before
   the next phase starts.
5. If a review finds a correctness issue, address it before proceeding.
6. Manual testing happens after behavior-changing phases and before commit.

## Risks

1. Incorrect ID movement can preserve stale highlights.
2. Under-incrementing `content_generation` is a correctness bug.
3. Over-incrementing `content_generation` is fail-closed but may leave the UX
   problem unresolved for idempotent or style-only updates.
4. Row storage refactoring is broad because current code moves raw cell vectors
   directly.
5. Alternate buffer save/restore can accidentally duplicate or confuse retained
   line identities.
6. Reflow equivalence is hard and should remain fail-closed.
7. Snapshot/lease memory grows with selected retained-line count, but row-level
   integer descriptors are acceptable compared with text comparison.
8. Session-only proof is not enough; model-side validation is the final stale
   span guard.
9. Primary repaint recovery can reconstruct rows that look identical but lack
   proven identity.
10. Dirty rows are still useful render damage metadata but must not remain the
    semantic source of selection truth.
11. Manual app validation is still required for the original Codex streaming
    scenario because automated tests cannot fully prove interactive visual
    timing and clipboard behavior in the deployed host.
12. Phase 5 viewport projection is default-off; manual testers must not treat
    default non-projection across user viewport movement as a regression unless
    the explicit opt-in gate is enabled.
13. Hover/display metadata such as row modification time remains future work and
    must not be used as a correctness proof.

## Rejected alternatives

### Wall-clock timestamps as proof

Rejected. They do not prove identity or content stability.

### Full selected-text comparison per snapshot

Rejected. It is O(selected range) under streaming output and reintroduces the
performance risk this design is avoiding.

### Relaxing viewport mapping checks

Rejected as a durable fix. It can reduce false detaches but weakens stale-span
protection and does not solve synchronized-output dirty-row ambiguity.

### Dirty-row proof improvements only

Rejected as the main architecture. Dirty rows are render damage metadata, not
stable retained-line identity.

### Per-row-index mutation epoch without retained-line identity

Rejected. Physical row indexes and storage slots are reused and shifted; this
cannot prove identity across scrollback growth, scroll regions, or zero
scrollback reuse.

### Cell/cluster provenance in the first phase

Rejected for initial implementation. Row-level provenance is enough to fix the
observed failure and is substantially less invasive.

### Renderer-side selection identity

Rejected. The renderer must remain passive.

### Surface-side row text comparison as durable proof

Rejected. Existing surface text comparison can remain as a conservative legacy
validation during transition, but new span preservation must be based on
retained-line provenance.

## Success criteria

The plan is successful when:

1. Manual Codex active-output selection no longer vanishes while selected
   retained lines remain unchanged and retained.
2. Selected retained-line mutations still detach visual spans.
3. Copy payload remains available after visual detachment.
4. Zero-scrollback reuse and scrollback eviction cannot stale-highlight.
5. Synchronized-output release preserves spans only when retained-line
   provenance matches.
6. No large text comparisons are introduced into streaming snapshot paths.
7. The implementation is covered by focused tests with before/after evidence for
   behavior-changing phases.
8. Phase 6 documentation identifies the implemented architecture, final phase
   outcomes, manual validation checklist, and residual manual-test expectations.

