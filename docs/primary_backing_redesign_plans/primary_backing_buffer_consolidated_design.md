# Primary Backing Buffer Consolidated Design

## Status and repository scope

This document is the consolidated design plan for retrying the primary backing
buffer redesign in `C:\plms\varinomics\vnm_terminal_surface`. This worktree
copy is authoritative for implementation and review; sibling-checkout copies
are not authoritative.

Historical round reports, Claude outputs, and evaluation artifacts in this
directory are evidence only. Their recommendations are superseded by this
consolidated design and by the phase gate documents. In particular, older
recommendations to permanently delete the repaint-recovery policy surface are
not current guidance: recovery is deferred to Phase R for restructuring on the
canonical backing model.

It uses `round2_agent_01_storage_invariants.md` as the architectural baseline
because it gives the cleanest invariant model, concept separation, and
sequencing away from the failed broad implementation.

It imports `round2_agent_02_incremental_tests.md` for phase gates, test-first
discipline, and rollback mechanics. It imports
`round2_agent_03_failure_modes.md` for failure-mode taxonomy, non-goal
discipline, and resize/viewport/selection edge cases.

The corrected round-3 evaluations support this split. Agent 01 and the
corrected Claude evaluation explicitly choose report 01 as the baseline and
recommend adding report 02's gate format. Agent 02 conditionally prefers report
02 only for implementation readiness, while warning that its instrumentation and
mirror phases must be trimmed. This plan therefore uses report 01 for
architecture and report 02 for gating, not for production mirror storage.

`recovery_baseline_correction.md` is a top-level amendment to this plan. It must
be closed before Phase 0A/0B/1 work is treated as behavior-preserving, because
the repository baseline at `d1d0637` removed the old recovery implementation
while the current product direction preserves recovery as a policy to be
restructured in Phase R.

## Core principle

Separate concepts before changing behavior.

The first batches introduce vocabulary, row domains, invariants, read adapters,
and evidence tests while preserving current runtime behavior. Behavior changes
come only after storage, viewport, selection, render snapshot, and public
projection semantics are separated enough that a regression can be attributed to
one concept.

Phase 0 is behavior-preserving. It records and preserves current production
behavior and configuration defaults, including
`recover_scrollback_from_primary_repaints`; it does not change those defaults
or remove the existing repaint-recovery heuristic.

`recover_scrollback_from_primary_repaints` is an existing recovery policy, not a
foundation for this design. Core tests may explicitly disable recovery to prove
storage semantics, but production defaults are unchanged until a later named
behavior phase intentionally changes them. Recovery is a deferred policy layer:
it must be restructured after storage, viewport, and publication semantics are
clean, not treated as deleted or as proof for the core backing model.

## Objectives

1. Define one primary row universe: retained primary history followed by exactly
   the active primary grid tail.
2. Make active-grid, primary-backing, viewport, snapshot, selection, public
   projection, and recovery domains explicitly separate.
3. Preserve structural blank rows as real retained rows with identity and
   provenance.
4. Make all primary read paths agree on one canonical row source before changing
   write ownership.
5. Move primary storage mutation behind named operations and named backing
   deltas.
6. Migrate behavior one semantic family at a time, each with evidence, exit
   criteria, manual gates when needed, and rollback.
7. End with one primary storage owner, one primary row producer, no permanent
   mirror/fallback path, and no hidden recovery dependency.

## Non-goals

1. Do not change production recovery defaults or remove the existing
   repaint-recovery heuristic in Phase 0.
2. Do not fix Codex-like repaint recovery in foundational phases.
3. Do not base storage correctness on rendered text, render snapshots, public
   projections, transcript replay, or repaint heuristics.
4. Do not introduce taller backing, nonzero viewport origin, panning, DSR
   coordinate changes, resize policy changes, selection-anchor redesign, public
   scroll behavior changes, transcript diagnostics, and recovery in one batch.
5. Do not use a production dual-written mirror as the default migration path.
   Temporary debug/test comparison is allowed only with a deletion or promotion
   gate.
6. Do not route public projection through live hidden rows during synchronized
   output.
7. Do not treat normal-height success as proof that heights `0`, `1`, `2`,
   resize, chunk seams, or detached viewport cases are safe.
8. Do not leave `_legacy`, `_v2`, compatibility scalars, source switches, old
   row producers, or recovery bypasses after their migration purpose ends.

## Concepts that must stay separate

1. Primary backing storage: retained primary history plus active primary grid
   tail.
2. Active primary grid: the cursor-addressable tail, exactly `grid_size.rows`
   rows high.
3. Retained primary history: rows that left the active grid through terminal
   scroll semantics and remain addressable until eviction or explicit clear.
4. Alternate active grid: a separate active grid with no primary history.
5. Saved inactive state: buffer state saved across primary/alternate transitions,
   converted explicitly at save/restore boundaries.
6. Live viewport: the model/session view over live storage.
7. Public viewport: the user-visible view over safe public content, especially
   during synchronized output.
8. Render snapshot: immutable renderer input in snapshot-local coordinates.
9. Selection anchors and payloads: selection state bound to an explicit domain,
   never silently converted through visible indexes.
10. Backing deltas: named storage effects such as append, eviction, clear,
    resize/reflow, and mode transition.
11. Recovery policy: separately gated repaint inference, external to storage
    ownership.

## Required invariants

### Storage invariants

1. In primary mode, the primary row universe is ordered as retained history rows
   followed by exactly `grid_size.rows` active primary grid rows.
2. `scrollback_rows` means retained primary history rows. It is not visible row
   count, vector capacity, or inferred rendered text.
3. Cursor addressing is valid only inside the active primary grid.
4. Full-primary scroll is the normal producer of retained history.
5. Partial scroll regions append primary history only when terminal semantics
   explicitly cross the full-primary boundary.
6. Insert/delete line, erase-in-line, erase-display, cursor-home repaint,
   synchronized redraw, and resize do not synthesize history by themselves.
7. Blank retained rows are structural rows with independent identity and
   provenance.
8. Scrollback clear removes retained history only through a named clear
   operation.
9. Scrollback eviction removes oldest retained history only, never active-grid or
   alternate rows.
10. Alternate rows never become primary history.

### Coordinate invariants

1. Active-grid row, primary-backing row, viewport row, and snapshot row are
   distinct domains.
2. Conversions live in named helpers or narrow adapters, not duplicated ad hoc
   arithmetic.
3. Visible row indexes are valid only in `[0, visible_height)`.
4. Snapshot rows are local to one immutable snapshot and are not persistent
   storage coordinates.
5. Selection anchors state whether they bind to backing identity, public
   projection coordinates, viewport coordinates, or finalized payload.
6. `offset_from_tail == 0` means the viewport follows the live primary tail.
7. Detached viewport anchors remain valid after append/evict/clear/resize or are
   explicitly clamped, invalidated, or converted by policy.

### Projection and publication invariants

1. Render snapshots are read-only consumers of storage or public projection.
2. Public projection is a publication layer, not storage.
3. During synchronized output, hidden live storage may mutate, but public
   snapshots are sourced only from public data until release.
4. Public projection capture is read-only and happens at safe publication
   boundaries.
5. Public and live viewport state are separately testable.

### Recovery invariants

1. Current production recovery defaults remain unchanged until a named
   behavior phase explicitly changes them.
2. Core storage, viewport, resize, selection, and publication tests run with
   repaint recovery disabled.
3. Disabling recovery must not break ordinary CRLF/newline scrollback, blank-row
   retention, clear, eviction, or viewport clamping.
4. Recovered rows, if ever accepted, carry explicit recovered provenance.
5. Recovery never mutates core storage directly and never masks coordinate,
   resize, viewport, selection, or publication defects.

## Failure modes this design prevents

1. Coordinate mixing between visible rows, backing rows, scrollback rows,
   snapshot rows, public projection rows, and selection rows.
2. Selection drift caused by visible-row anchors instead of stable backing or
   explicit payload domains.
3. Scroll drift caused by reconstructing operation intent from scalar counts.
4. Wheel boundary errors caused by clamping against storage capacity instead of
   the active public projection.
5. Repeated rows caused by projection rebuilds or resize paths being mistaken
   for terminal scroll events.
6. Blank-line loss caused by treating empty text as absence of a row.
7. Small-height resize instability at heights `0`, `1`, and `2`.
8. Synchronized-output leaks where hidden live mutations become publicly visible
   before release.
9. Recovery false positives from cursor-home repaint, EL/ED variants, repeated
   redraw, resize-adjacent repaint, or blank-only shifts.
10. Permanent migration scaffolding where old and new row producers remain alive
    indefinitely.

## Phase gate format

Every implementation batch must publish a gate table before review. The table
must contain these entries:

1. Scope: the phase or subphase id and the bounded implementation surface.
2. Behavior axis: `none` for no-behavior work, or exactly one user-visible
   behavior axis.
3. Recovery state: production default unchanged, core-test override disabled,
   or the exact named behavior change that intentionally changes recovery.
4. Evidence: exact test, fixture, characterization, static guard, or written
   regression-only classification.
5. Baseline outcome: current result before production behavior changes.
6. Exit predicate: objective assertions that prove the phase is complete.
7. Manual gate: `none` for no-behavior phases; otherwise exact focused
   scenarios and expected observations.
8. Rollback mechanism: clean revert, named flag, named source switch, or named
   adapter window.
9. Deletion gate: the phase that removes any temporary scalar, comparator,
   fallback, source switch, adapter, or compatibility path introduced here.

Every behavior-changing phase must name exactly one behavior axis and either an
exact failing test or an explicit regression-only classification. A test that
already passes is a regression gate, not proof of a fixed bug.

No phase may exit with an active legacy fallback, source switch, or restored old
path unless the gate table names the temporary owner and deletion gate. If
rollback activates a fallback, the phase is incomplete and no later phase may
begin until the fallback is removed again or the plan is amended after
independent review.

Reliable failure-reproducing tests and characterizations are evidence, not
rollback targets. If a test seam is faulty, roll back the seam; keep reliable
characterization evidence.

Gate terms:

1. `Field-equivalent` means the named observable fields in the gate table match
   the recorded baseline for the named fixtures.
2. `Affected path` means the exact read, write, projection, viewport, or
   selection path named in the gate table.
3. `Failed slice` means one named phase/subphase scope item with its evidence
   and rollback mechanism, not an open-ended area of code.
4. `Harmless temporary diagnostic` means diagnostic output or assertions that
   cannot change release behavior and have a named deletion gate.

## Failure ledger

Phase 0A creates and maintains a durable failure ledger. Each entry must record:

1. Scenario name.
2. Exact test, fixture, or characterization name once created.
3. Input stream and chunking mode when relevant.
4. Recovery setting, including whether production default or core-test-disabled
   recovery was used.
5. Current baseline outcome: pass, fail, hidden by recovery, or not yet covered.
6. Owning phase or subphase.
7. Intended product contract.
8. Classification: failure-before-fix or regression-only.

Later phases may add entries, but they may not reclassify a known failure
without updating this ledger and naming the reason in the phase gate.

## Phase plan

The first gate is Recovery Baseline Correction. It resolves the mismatch between
the `d1d0637` repository baseline, which removed the old recovery
implementation, and the current product direction, which defers recovery for
Phase R restructuring rather than permanently rejecting it.

After that gate closes, the first implementation batch is Phase 0A only. It is
bounded to documentation, recovery-default audit notes, failure-ledger/test
configuration, and test/debug-only invariant scaffolding. It must not change
production runtime behavior, production recovery defaults, storage ownership,
resize behavior, public projection, render snapshot behavior, selection policy,
DSR behavior, or public scroll behavior.

### Recovery Baseline Correction: policy surface and baseline reconciliation

Purpose: make the recovery policy baseline explicit before any phase relies on
"production defaults unchanged" as a no-behavior claim.

Artifact:

1. `recovery_baseline_correction.md` records the required decision, evidence,
   exit predicate, and rollback rule.

Scope:

1. Record HEAD, dirty recovery-related files, and whether those changes are
   kept, removed, or split into a named recovery-baseline batch.
2. Decide whether the policy/config surface exists before Phase R, including
   session config, screen model config, transcript schema, replay inference, and
   public Windows default.
3. Quarantine old model-internal recovery implementation pieces from
   foundational phases unless they are owned by a named behavior-changing
   recovery-baseline batch.
4. Amend Phase 0A/0B docs if their recovery-default audit does not match the
   decided baseline.

Exit:

1. The recovery policy surface that exists before Phase 0A is explicit.
2. Old direct storage-writing heuristic pieces are either absent, split into a
   named recovery-baseline batch, or recorded as a known product gap owned by
   Phase R.
3. Core tests cannot be silently made to pass through recovery defaults.

### Phase 0A: recovery-default audit, vocabulary, and baseline characterization

Purpose: make the baseline measurable while preserving current runtime behavior.

Phase 0A artifacts:

1. `phase_0a_primary_backing_baseline.md` records the vocabulary, recovery
   default audit, named recovery-disabled test configuration, and gate table.
2. `primary_backing_failure_ledger.md` records motivating scenarios, current
   baseline classification, owning phase, intended contract, and
   failure-before-fix or regression-only status.

Scope:

1. Document the row universe, coordinate domains, publication boundary, and
   recovery boundary near the implementation plan or future storage types.
2. Audit the current production default and propagation path for
   `recover_scrollback_from_primary_repaints`.
3. Preserve the audited production default and existing repaint-recovery
   heuristic.
4. Add or identify a core-test configuration that disables repaint recovery for
   storage, viewport, resize, selection, and publication tests.
5. Create the failure ledger and record motivating scenarios as pass, fail,
   hidden by recovery, or not yet covered.
6. Add only documentation, test configuration, characterization fixtures, and
   test/debug-only invariant scaffolding.

Non-goals:

1. No runtime behavior change.
2. No production recovery-default change.
3. No heuristic removal.
4. No backing storage owner yet.
5. No viewport, resize, selection, public projection, transcript, DSR, or
   recovery behavior change.
6. No operation-trace framework unless a later phase gate names it.

Evidence:

1. The phase gate names the recovery-disabled test configuration or fixture.
2. The phase gate records the audited production default for
   `recover_scrollback_from_primary_repaints`.
3. The failure ledger records baseline outcomes for the motivating scenarios.

Exit:

1. The audited production recovery default is recorded and unchanged.
2. Core tests have a named recovery-disabled configuration.
3. The failure ledger exists and assigns each motivating scenario to an owning
   phase or regression-only classification.
4. Any invariant scaffolding is test/debug-only and cannot alter release
   behavior.

Manual:

1. None. A focused Codex-like resize/repaint scenario may be recorded as
   observation only, but it is not a phase exit gate.

Rollback:

1. Cleanly revert the Phase 0A documentation, ledger, test configuration, or
   faulty test/debug-only scaffolding named in the gate table. Production
   recovery defaults remain as they were before Phase 0A.

### Phase 0B: vocabulary guards and minimal no-behavior observations

Purpose: make the no recovery-as-storage-evidence foundation reviewable without
changing behavior.

Scope:

1. Add the smallest useful read-only observations at stable boundaries such as
   ingest, resize, alternate enter/leave, scrollback clear, and
   scrollback-limit change.
2. Add a minimal scrollback-growth observer or equivalent test seam if the
   phase gate needs it to prove which operations grow retained history.
3. Add an enforceable review guard or allowlist for recovery references,
   repaint inference, transcript replay, text-matching cliffs, and source
   switches outside the Phase R recovery-policy area.
4. Add a review guard for non-extraction legacy storage mutation during
   no-behavior phases.

Non-goals:

1. No production behavior change.
2. No production recovery-default change.
3. No storage owner, row-domain wrapper rollout, viewport behavior, resize
   behavior, selection policy, public projection behavior, DSR behavior, public
   scroll behavior, or renderer behavior change.
4. No broad tracing framework unless a later phase gate names it.

Evidence:

1. Each observer or guard has a named test, static check, or review predicate.
2. Each observer is classified as read-only, test-only, or debug-only.
3. Guard violations point to the Phase R recovery-policy area or the owning future
   phase.

Exit:

1. The no recovery-as-storage-evidence foundation rule is documented and
   guarded.
2. Observers and invariant checks cannot change release behavior.
3. Any temporary guard or observer has a deletion or promotion gate.

Manual:

1. None.

Rollback:

1. Remove the named observer, guard, or faulty test seam from the gate table.
   Reliable characterization entries remain in the failure ledger. The phase is
   incomplete until the gate passes without the faulty seam.

### Phase 1: focused pre-behavior tests

Purpose: prevent accidental history creation and blank-row loss before storage is
refactored.

Scope:

1. Add storage-shape invariant coverage for fresh primary state.
2. Add scrollback-growth source tests for LF, IND, NEL, CRLF overflow, and
   full-primary scroll.
3. Add no-synthesis tests for cursor-home repaint, EL, ED variants, resize
   no-op cases, alternate enter/leave, synchronized output on/off, cursor
   visible/hidden, and empty chunks.
4. Add chunk-split invariance tests for representative scroll, repaint, resize,
   and synchronized-output streams.
5. Add blank retained row identity/provenance checks.
6. Include small visible heights where cheap, especially `1` and `2`, without
   turning this into the full resize phase.

Non-goals:

1. No production fixes unless a test hook requires a narrow debug-only seam.
2. No recovery-based passing condition.
3. No replay corpus requirement unless deterministic fixtures already exist.

Evidence:

1. Known wrong behavior gets failure-before-fix tests.
2. Already-correct behavior is labeled regression-only.

Exit:

1. Tests distinguish protocol scrollback growth from repaint/non-growth.
2. Structural blank retained rows have independent identity.
3. Chunking does not create or remove rows for deterministic streams.

Manual:

1. None beyond any optional Phase 0A observation.

Rollback:

1. Remove the faulty test seam named in the gate table. Reliable
   characterization tests stay as evidence unless the characterization itself is
   proven invalid.

### Phase 2: typed row domains, active/saved boundary, and conversions

Purpose: make coordinate misuse reviewable without changing behavior.

Scope:

1. Introduce lightweight row-domain wrappers or unmistakably named helper types
   for active-grid rows, primary-backing rows, viewport rows, and snapshot rows.
2. Centralize conversions such as active-to-backing, backing-to-active,
   backing-to-viewport, viewport-to-backing, and snapshot-local mapping.
3. Split active mutable state from saved inactive state, or add explicit
   conversion functions at save/restore boundaries.
4. Route duplicated arithmetic through helpers.
5. Keep alternate conversions separate from primary conversions.

Non-goals:

1. Do not wrap every local loop if the coordinate domain is obvious.
2. Do not add arithmetic operators that make wrappers act like raw integers.
3. Do not introduce `viewport_origin_row`; defer the field until the behavior
   phase that needs nonzero panning.
4. Do not change storage ownership or runtime behavior.

Evidence:

1. Conversion and save/restore tests are regression-only unless they expose an
   existing disagreement.

Exit:

1. Duplicated `scrollback_rows - offset_from_tail + row` style arithmetic is
   removed or quarantined behind helpers.
2. Alternate state cannot accidentally report primary scrollback as alternate
   history.
3. Existing behavior is field-equivalent to baseline.

Manual:

1. None.

Rollback:

1. Revert the named helper routing or state-boundary extraction from the gate
   table without changing public behavior. The phase is incomplete until the
   no-behavior routing gate passes again.

### Phase 3: canonical read adapter over current storage

Purpose: make read paths agree before write ownership changes.

Scope:

1. Add a read-only primary row source over current retained history plus active
   grid.
2. Route primary render snapshot lookup through the row source.
3. Route retained-line lookup, read-only selection payload/source equivalence
   checks, and test provenance lookup through the same row source.
4. Keep alternate reads separate.
5. Keep dirty ranges in snapshot coordinates.
6. Treat public projection capture as a boundary, not a behavior migration.

Non-goals:

1. No second storage owner.
2. No production mirror.
3. No write-path migration.
4. No selection persistence/invalidation redesign.
5. No selection anchor, visual-span, or selection-policy migration.
6. No synchronized-output release behavior change.

Evidence:

1. Field-equivalence tests for snapshots, retained-line lookup, selection
   payload extraction, and viewport-visible rows.

Exit:

1. There is one producer for primary logical row reads.
2. Snapshot, selection, retained-line lookup, and test provenance agree for the
   same backing coordinate.
3. Public behavior remains unchanged.

Manual:

1. None.

Rollback:

1. Restore the named read path from the gate table to legacy lookup while
   keeping tests that describe the intended shared-source contract. The phase is
   incomplete until the legacy lookup is removed again.

### Phase 4: primary and alternate storage owner extraction without tallness

Purpose: name the storage owners without changing row counts, viewport behavior,
or resize policy.

Scope:

1. Extract a `Primary_backing_buffer` or equivalent owner for retained primary
   history plus active grid tail.
2. Extract an `Alternate_active_grid` or equivalent owner with no history vector.
3. Keep active grid height equal to `grid_size.rows`.
4. Move active-grid writes, retained-history append, eviction, and clear behind
   named owner methods as mechanical extraction permits.
5. Use temporary debug/test comparison only if needed, never as a production
   alternate source of truth.
6. Delete or demote legacy helpers in the same batch that removes their last
   caller.

Non-goals:

1. No backing capacity growth.
2. No nonzero viewport origin or panning.
3. No DSR coordinate change.
4. No resize policy change.
5. No public projection or selection behavior change.
6. No new recovery-policy hook; existing audited recovery defaults remain
   unchanged.

Evidence:

1. Regression-only extraction tests plus invariant checks for owner row counts,
   alternate isolation, blank-row identity, and saved-state non-aliasing.

Exit:

1. A reviewer can identify the primary storage owner and alternate storage
   owner.
2. Alternate storage cannot inherit primary history.
3. No public behavior changed.
4. Any temporary comparator has an owner and removal gate.

Manual:

1. None for purely mechanical extraction. If the gate table includes
   user-facing routing changes, it must also name the exact ordinary primary
   output, primary scrollback, alternate enter/leave, and return-to-primary
   smoke scenarios before the batch starts.

Rollback:

1. Cleanly revert the named extraction batch or restore the named direct-access
   path from the gate table. The phase is incomplete until that path is removed
   again.

### Phase 5A: backing-delta vocabulary and tests

Purpose: name storage effects before any production consumer relies on them.

Scope:

1. Define a narrow `Backing_delta` or equivalent vocabulary for history append,
   eviction, full clear, active-height resize, column reflow, mode transition,
   and no-op mutation.
2. Add tests or fixtures that distinguish those effects.
3. Do not wire production viewport behavior to the delta model.

Non-goals:

1. No production consumer migration.
2. No diagnostic reason strings as a substitute for invariants.
3. No synchronized-output/public projection policy change.
4. No new recovery-policy hooks; existing audited recovery defaults remain
   unchanged.

Evidence:

1. The gate table names tests or fixtures proving that the vocabulary can
   distinguish append, eviction, clear, resize/reflow, mode transition, and
   no-op.

Exit:

1. Each delta kind has a named meaning and test coverage.
2. No production behavior consumes the new vocabulary yet.
3. Any temporary test-only adapter has a deletion gate.

Manual:

1. None.

Rollback:

1. Cleanly revert the vocabulary/tests named in the gate table. The phase is
   incomplete until the vocabulary gate passes.

### Phase 5B: append, eviction, clear, and no-op delta emission

Purpose: emit deltas for non-resize storage effects while preserving current
consumers.

Scope:

1. Emit append, eviction, clear, and no-op deltas from existing storage
   operations.
2. Keep legacy scalar fields and existing consumers unchanged.
3. Add assertions that scalar compatibility values match the emitted deltas for
   the migrated operations.

Non-goals:

1. No viewport consumption of deltas.
2. No resize/reflow or mode-transition delta emission.
3. No storage behavior change.
4. No new recovery-policy hooks; existing audited recovery defaults remain
   unchanged.

Evidence:

1. The gate table names tests proving append, eviction, clear, and no-op effects
   emit distinct deltas and preserve current scalar outputs.

Exit:

1. The named operations emit exactly one classified delta sequence per test
   fixture.
2. Existing scalar outputs are unchanged for the named fixtures.
3. Any temporary scalar compatibility path has a removal owner.

Manual:

1. None.

Rollback:

1. Disable or remove delta emission for the named operation family only. The
   phase is incomplete until emission is restored or the plan is amended.

### Phase 5C: resize/reflow and mode-transition delta emission

Purpose: describe resize/reflow and mode-transition effects without changing
their behavior.

Scope:

1. Emit active-height resize, column reflow, and mode-transition deltas from
   existing operations.
2. Preserve current resize, alternate-screen, viewport, and public projection
   behavior.
3. Add assertions that resize/reflow and mode-transition deltas are distinct
   from append, eviction, clear, and no-op.

Non-goals:

1. No resize policy change.
2. No alternate-transition behavior change.
3. No viewport or public projection consumer migration.
4. No new recovery-policy hooks; existing audited recovery defaults remain
   unchanged.

Evidence:

1. The gate table names tests for active-height resize, column reflow,
   alternate enter/leave, and no-op transitions.

Exit:

1. Resize/reflow and mode-transition effects are distinguishable in tests.
2. Current scalar outputs and user-visible behavior are field-equivalent to the
   recorded baseline for the named fixtures.
3. Any temporary compatibility path has a removal owner.

Manual:

1. None unless the gate table explicitly includes user-facing routing changes.

Rollback:

1. Disable or remove emission for the named resize/reflow or mode-transition
   family only. The phase is incomplete until emission is restored or the plan
   is amended.

### Phase 5D: compatibility scalar derivation and removal ownership

Purpose: make old scalar publications temporary derived outputs instead of
independent truth.

Scope:

1. Derive legacy scalar fields such as total scrollback and evicted rows from
   named deltas where the previous subphases made that safe.
2. Mark each remaining compatibility scalar, comparator, source switch, or
   adapter with its owning deletion phase.
3. Add integration assertions only for already-migrated delta families.

Non-goals:

1. No new mutation families.
2. No viewport behavior change.
3. No public projection, selection, resize policy, or recovery behavior change.

Evidence:

1. The gate table names scalar-derivation tests and the deletion owner for each
   temporary compatibility item.

Exit:

1. Migrated scalar values are derived from deltas for the named fixtures.
2. Independent scalar writes remain only where their owning future phase is
   named.
3. No temporary compatibility item lacks a deletion gate.

Manual:

1. None.

Rollback:

1. Restore the named scalar publication for the failed compatibility item only.
   The phase is incomplete while that restored independent scalar remains.

### Phase 6: storage mutator migration by semantic family

Purpose: make the primary backing owner authoritative one mutation family at a
time.

Subphase order:

1. Full-primary scroll and CRLF/newline overflow.
2. Scroll regions plus insert/delete line.
3. Clear, erase, eviction, and scrollback limits.
4. Resize and reflow.
5. Alternate-screen transitions.

Rules for every subphase:

1. Add failure-before-fix tests only for behavior known wrong at the start.
2. Label already-correct behavior as regression-only.
3. Delete or demote legacy write paths for the family before moving on.
4. Keep core tests recovery-disabled and do not change production recovery
   defaults.
5. Do not migrate another semantic family in the same batch.

Subphase notes:

1. Full-primary scroll and CRLF/newline overflow preserve real blank rows and
   route all normal retained-history append through one producer.
2. Partial scroll regions do not append primary history unless terminal semantics
   make the operation a full-primary scroll.
3. EL, ED without explicit scrollback clear, cursor-home repaint, and resize do
   not synthesize retained history.
4. Explicit clear emits clear, not append or eviction.
5. Limit shrink evicts oldest retained rows and reports viewport/selection
   effects through named deltas.
6. Resize is split into active-height change, column reflow, and
   viewport/projection adjustment while preserving documented current behavior
   until a behavior phase changes it.
7. Alternate enter/leave preserves primary backing and never imports alternate
   rows into primary history.

Non-goals:

1. No multi-family behavior bundle.
2. No selection policy redesign during storage mutation migration.
3. No public projection release behavior change.
4. No recovery-policy work in this storage-mutation phase.

Evidence:

1. Each subphase names its failing tests or regression-only test set before
   production mutation changes.

Exit:

1. The subphase family no longer requires reverse sync from legacy storage.
2. Scrollback growth observer tests pass.
3. No behavior changed beyond exact failure-owned fixes.

Manual:

1. After resize/reflow, manually validate height `1`, height `2`, shrink/grow,
   width reflow, detached scrollback, wrapped rows, trailing blanks, and resize
   during long output.
2. After alternate transitions, manually validate primary history preservation
   across alternate enter/leave and selection presence.

Rollback:

1. Re-enable the prior mutation path for the failed family only. Do not roll back
   unrelated completed families. The subphase is incomplete while the prior
   mutation path is active, and no later mutation-family subphase may begin
   until it is removed again or the plan is amended.

### Phase 7A: viewport tail-following and detached anchoring on deltas

Purpose: move viewport synchronization from scalar inference to named deltas for
tail-following and detached anchoring only.

Scope:

1. Consume `Backing_delta` for tail-following and detached viewport anchor
   updates.
2. Preserve or clamp detached anchors using retained row identity or a typed
   fallback reason.
3. Leave wheel bounds and selection policy unchanged.

Non-goals:

1. No core storage mutation changes.
2. No wheel/public-projection boundary changes.
3. No selection anchor, invalidation, or payload policy changes.
4. No recovery-policy work in this viewport phase.

Evidence:

1. Tests for append while detached, eviction storm, full clear, resize/reflow,
   alternate enter/leave, and offset clamping.

Exit:

1. Tail-following and detached-anchor transitions are driven by typed deltas for
   the named fixtures.
2. Existing wheel scrolling and selection behavior are field-equivalent to the
   recorded baseline for the named fixtures.
3. Any scalar viewport sync fallback has a deletion gate.

Manual:

1. Focused manual scrollback, detached-output, clear, eviction, and resize
   checks when the gate table classifies this as behavior-changing.

Rollback:

1. Restore the named scalar viewport-sync path for this subphase only. The
   subphase is incomplete until the scalar fallback is removed again.

### Phase 7B: wheel and scroll bounds against public projection

Purpose: make wheel and scroll boundaries deterministic against the active
public projection and publication state.

Scope:

1. Clamp wheel scrolling against the active public projection and publication
   state, not internal storage capacity.
2. Cover live, detached, synchronized-output hold, and post-release states.
3. Leave storage mutation, viewport anchoring policy, and selection policy
   unchanged except for the named wheel-boundary behavior.

Non-goals:

1. No storage mutation changes.
2. No selection changes.
3. No synchronized-output release policy change.
4. No recovery-policy work in this wheel-boundary phase.

Evidence:

1. Tests for wheel boundaries in live, detached, synchronized-output, and
   post-release states with the recovery state named in the gate table.

Exit:

1. Wheel boundaries match the public projection contract for the named fixtures.
2. No storage-capacity clamp remains on the named wheel path.
3. Non-wheel viewport behavior is field-equivalent to baseline for the named
   fixtures.

Manual:

1. Focused manual scrollbar and wheel checks in live, detached, held, and
   post-release states.

Rollback:

1. Restore the named previous wheel-boundary path only. The subphase is
   incomplete until the old boundary path is removed again.

### Phase 7C: selection domains, invalidation, and payload preservation

Purpose: define selection behavior on explicit domains after viewport storage
semantics are stable.

Scope:

1. Define selection anchor domains and invalidation rules for clear, eviction,
   alternate transitions, and destructive resize/reflow.
2. Keep payload preservation explicit when backing identity is gone.
3. Avoid render snapshot indexes as persistent anchors.

Non-goals:

1. No core storage mutation changes.
2. No viewport or wheel-boundary behavior changes except those already complete
   in Phase 7A and Phase 7B.
3. No offset patches that bypass typed coordinate conversion.
4. No recovery-policy work in this selection phase.

Evidence:

1. Tests for selection across history/active boundary, selection during
   eviction, full clear, destructive resize/reflow, alternate enter/leave, and
   payload preservation when backing identity is gone.

Exit:

1. Selection either maps predictably, detaches with preserved payload, or is
   explicitly invalidated for each named destructive event.
2. Selection anchors state their domain.
3. No render snapshot row is used as a persistent selection anchor.

Manual:

1. Focused manual copy/paste from history, after eviction, after clear, and
   after destructive resize/reflow.

Rollback:

1. Restore the named previous selection policy only. The subphase is incomplete
   until the old policy path is removed again.

### Phase 8: public projection and synchronized-output reconciliation

Purpose: make publication a clean layer over the canonical row source.

Scope:

1. Capture public projections from the canonical row source at safe publication
   boundaries.
2. Source all visible fields from public data only during synchronized-output
   holds.
3. Ensure public-projection scroll snapshots do not advance the live-content
   basis.
4. Reconcile release using retained row identity where available.
5. Keep live viewport and public viewport state separately testable.

Non-goals:

1. No core storage changes except defects owned by earlier phases.
2. No recovery-policy work in this publication phase.
3. No public scroll behavior change without exact evidence.

Evidence:

1. Tests for hidden live row rewrites, hidden live scrollback growth, mode
   changes, cursor movement, detached public scroll, hold/release, and source
   alignment.

Exit:

1. Hidden live mutations do not leak before release.
2. Release publishes live content exactly once.
3. Public projection remains read-only and non-storage.

Manual:

1. Synchronized-output fixture that rewrites active rows, grows hidden
   scrollback, scrolls public view, and releases.

Rollback:

1. Restore the named previous public projection source or release path for the
   failed publication subphase only. The subphase is incomplete until the old
   path is removed again.

### Phase 9: behavior increments, one axis at a time

Purpose: fix remaining user-visible bugs only after concept separation is stable.

Candidate axes:

1. Full primary scroll and CRLF/newline blank-row retention.
2. Detached viewport anchoring across append, eviction, clear, and resize.
3. Resize policy changes where current behavior is documented as wrong.
4. Selection survival, payload preservation, or invalidation across eviction and
   destructive reflow.
5. Public projection source alignment and release policy.
6. Optional independent display viewport behavior.

Rules:

1. One behavior axis per subphase.
2. Exact failing test before implementation, or explicit regression-only status.
3. Manual validation compares with the intended product contract and Windows
   Terminal where that is the relevant oracle.
4. No recovery-policy result may be used to make the behavior pass; recovery
   evidence belongs in Phase R.

Evidence:

1. A named failing test or written regression-only classification for the axis.

Exit:

1. Only the named behavior changed.
2. Snapshot, viewport, selection, and public projection expectations changed only
   for the named axis.

Manual:

1. Focused manual validation for the specific behavior axis.

Rollback:

1. Disable or revert the named behavior switch or source change. Keep storage
   and invariant phases if they remain valid. The behavior subphase is
   incomplete while the reverted switch or old source remains active.

### Phase 10: scaffolding removal

Purpose: leave one architecture, not migration layers.

Scope:

1. Remove temporary adapters, compatibility scalars, debug comparators, source
   switches, old conversion helpers, old row producers, and migration comments
   after their consumers are gone.
2. Keep stable invariant checks and typed conversion boundaries.
3. Update architecture documentation to describe the final row universe,
   viewport deltas, public projection boundary, and Phase R recovery-policy
   boundary.
4. Confirm no `_legacy`, `_v2`, fallback source, or recovery bypass remains
   without a named external consumer.

Non-goals:

1. No behavior fixes in cleanup.
2. No recovery implementation in cleanup.
3. No hidden fallback path to old storage.

Evidence:

1. Cleanup is regression-only and references the phase that made each scaffold
   obsolete.

Exit:

1. One primary backing owner.
2. One canonical primary row producer.
3. Public projection and recovery remain separate from storage.
4. Cleanup diffs are behavior-neutral.

Manual:

1. Final smoke only if cleanup removes user-facing routing code.

Rollback:

1. Restore only the named scaffold needed for the failed cleanup item, with a
   new removal gate. Cleanup is incomplete while the restored scaffold remains.

### Phase R: external repaint recovery policy

Purpose: restructure repaint recovery after storage, viewport, and publication
semantics are clean, so the policy operates on the canonical backing model
instead of internal visible-grid heuristics.

Entry criteria:

1. Core phases are stable with recovery disabled.
2. There is evidence that correct storage/projection semantics still lose useful
   user-facing history under a documented policy case.
3. False-positive suppression tests exist before implementation.

Allowed shape:

1. Recovery observes stable before/after public projections or snapshots,
   terminal context, buffer identity, resize metadata, and synchronized-output
   state.
2. Recovery emits `Recovery_proposal` records with reason, confidence or
   classification, candidate rows, ambiguity status, and provenance source.
3. A narrow policy layer may accept proposals through normal storage APIs.
4. Recovery never writes active rows, retained history, public projection, or
   selection state directly.
5. Recovery diagnostics are separate from terminal scrollback diagnostics.
6. Recovery remains separately installable/disableable and disabled for core
   storage tests.

Required tests:

1. Recovery disabled: ordinary scrolling, CRLF blank rows, clear, eviction,
   resize, viewport clamping, and selection behavior still pass.
2. True-positive recovery: the exact documented repaint case appends only
   provable rows with recovered provenance.
3. False positives suppressed: cursor-home repaint, EL, ED2, blank-only shifts,
   partial-region repaint, resize-adjacent repaint, repeated identical repaint,
   alternate screen, and synchronized hidden output.
4. Recovery proposals do not advance public projection or latest live-content
   basis.

Rollback:

1. Uninstall or disable the observer. Core storage behavior must remain correct.

## Test strategy

### Storage tests

Storage tests assert primary row universe shape, active grid height, retained
history count, append paths, blank-row identity, clear, eviction, scrollback
limit behavior, alternate isolation, and saved-state non-aliasing. They always
run with recovery disabled.

### Coordinate and adapter tests

Coordinate tests assert valid conversions between active-grid, backing,
viewport, and snapshot rows. Adapter tests assert facade mapping, out-of-range
failure, blank/styled/wide/wrapped/hyperlink row preservation, and agreement
between snapshot, retained-line lookup, selection extraction, and test
provenance.

### Growth and no-synthesis tests

Growth tests in core phases prove that only terminal scroll semantics can grow
retained history. Phase R separately tests whether accepted recovery proposals
may append recovered rows through normal storage APIs. No-synthesis
tests cover cursor-home repaint, EL, ED variants, synchronized output, cursor
visibility, empty chunks, resize no-op paths, and repeated redraws.

### Chunk-split and property tests

Chunk-split tests compare whole input against deterministic split input for
scroll, repaint, resize, and synchronized-output streams. The property harness
should use a fixed seed, bounded stream length, a defined escape vocabulary, and
assert row identity/provenance before visible text.

### Viewport tests

Viewport tests consume named deltas and assert tail following, detached anchors,
append, eviction, clear, resize/reflow, alternate transitions, offset clamping,
wheel boundaries, and public/live separation.

### Selection tests

Selection tests assert payload extraction, visual span validity, backing identity
anchoring, explicit invalidation, detached payload preservation, selection across
history/active boundaries, and selection under eviction storms.

### Resize tests

Resize tests are mandatory in the resize/reflow phase. Cover height `0` if
supported, height `1`, height `2`, shrink/grow, width reflow, wrapped logical
lines crossing the history/active boundary, trailing blanks, detached viewport,
selection across history/active boundary, alternate around resize, synchronized
output hold/release, and repeated `2 -> 3 -> 2 -> normal` cycles with and
without output.

### Publication tests

Publication tests assert synchronized-output hold, hidden live mutation, hidden
scrollback growth, public projection row source, public scroll while held,
release reconciliation, and public-vs-live viewport separation.

### Recovery tests

Recovery tests live only in Phase R. They must include recovery-disabled core
semantics, true-positive recovery, false-positive suppression, chunk-split
invariance, and explicit recovered provenance.

## Manual validation gates

No-behavior phases have no mandatory manual gate. Optional smoke observations
may be recorded, but they cannot be the only exit predicate and they cannot
justify behavior changes.

1. Baseline: record the production recovery default and the recovery-disabled
   characterization outcome for motivating scenarios.
2. Storage owner extraction: if the gate table includes user-facing routing
   changes, exercise the named primary output, primary scrollback, alternate
   screen, and return-to-primary smoke scenarios.
3. Viewport/deltas: when a viewport behavior subphase is active, exercise the
   named append, eviction, clear, detached viewport, and scroll wheel scenarios.
4. Resize/reflow: exercise heights `1` and `2`, shrink, expand, width reflow,
   wrapped rows, trailing blanks, detached scrollback, and resize during output.
5. Selection: copy across history/active boundaries, after eviction, after clear,
   and after destructive resize/reflow.
6. Synchronized output: verify public scroll cannot reveal hidden live rewrites
   or hidden live scrollback growth before release.
7. Behavior phases: demonstrate the exact failing behavior with recovery
   disabled, or classify the scenario as regression-only.
8. Phase R recovery: review true positives and false positives independently;
   recovery must remain separately gated and external.
9. Final cleanup: independent review confirms one storage owner, one row
   producer, no orphaned helper, no silent fallback, and no hidden behavior
   change.

## Rollback criteria summary

1. Phase 0A: cleanly revert the named documentation, ledger, test
   configuration, or faulty test/debug-only scaffolding. Production recovery
   defaults remain unchanged.
2. Phase 0B: remove the named observer, guard, or faulty test seam. Reliable
   characterization entries stay in the failure ledger.
3. Phase 1: remove only the faulty test seam; keep reliable failure
   reproductions and regression characterizations.
4. Phase 2: revert the named helper routing or active/saved boundary extraction.
5. Phase 3: restore the named read path to legacy lookup until the adapter gate
   passes again.
6. Phase 4: revert the named owner extraction or restore the named direct-access
   path until extraction passes again.
7. Phase 5A-5D: revert or disable the named delta vocabulary, emission, or
   scalar-derivation item only.
8. Phase 6: re-enable the prior mutation path for the named semantic family
   only.
9. Phase 7A-7C: restore only the named viewport, wheel-boundary, or selection
   path for the failed subphase.
10. Phase 8: restore the named public projection source or release path for the
    failed publication subphase.
11. Phase 9: disable or revert the named behavior switch or source change.
12. Phase 10: restore only the named scaffold needed for the failed cleanup
    item, with a new removal gate.
13. Phase R: uninstall or disable the recovery observer.

Rollback means the phase or subphase is not complete. No later phase may begin
while a rollback fallback, source switch, old producer, independent scalar, or
temporary adapter remains active unless the design is amended after independent
review. Rollback must not change production recovery defaults in Phase 0 or
revert unrelated completed phases.

## What not to repeat from the failed attempt

1. Do not make backing height a substitute for visible height.
2. Do not let visible row indexes index primary backing storage without explicit
   conversion.
3. Do not introduce backing tallness, viewport origin/panning, private capacity,
   DSR coordinate changes, resize policy changes, selection rewrites, public
   projection changes, diagnostics, and recovery in one batch.
4. Do not use rendered text, snapshots, public projection, transcript replay, or
   recovery as storage synchronization sources.
5. Do not infer scrollback from repaint text before storage and projection are
   correct.
6. Do not add action budgets, text-matching cliffs, cursor-home heuristics, or
   resize guards to core storage.
7. Do not hide behavior changes inside rename, extraction, cleanup, or test
   scaffolding phases.
8. Do not leave old and new row producers alive indefinitely.
9. Do not let transcript/replay/surface defaults mask core storage tests. Core
   test configurations must name recovery state explicitly while Phase 0
   preserves production defaults.
10. Do not treat blank text as absence of a row.
11. Do not route public projection or render snapshot production back into
    storage mutation.
12. Do not allow alternate screen state to share or import primary history.
13. Do not use scalar scrollback counts where named deltas are required.
14. Do not accept normal-height success as evidence that height `0`, `1`, `2`,
    resize, or chunk-boundary cases are safe.

## Review checklist for each batch

1. Which single concept is being separated or migrated?
2. Which invariant becomes stronger?
3. Which behavior changes, if any?
4. If behavior changes, where is the exact failing test?
5. If tests already pass, are they labeled regression-only?
6. Does the gate table name scope, behavior axis, recovery state, evidence,
   baseline outcome, exit predicate, rollback mechanism, and deletion gate?
7. Is recovery disabled for core tests, and are production recovery defaults
   unchanged unless this is a named recovery behavior phase?
8. Is there exactly one producer for the row result touched by this batch?
9. Are coordinate conversions explicit at domain boundaries?
10. Did the batch avoid backing tallness, panning, DSR, resize policy, transcript
   diagnostics, public projection behavior, selection policy, and recovery unless
   that is the named scope?
11. Did the batch avoid production mirror storage or define a temporary
    debug/test comparator removal gate?
12. Did the batch delete or demote helpers it orphaned?
13. Is any rollback fallback, source switch, old producer, or independent
    compatibility scalar still active? If yes, the phase cannot close.
14. Can the next batch start from the written design without chat history?
