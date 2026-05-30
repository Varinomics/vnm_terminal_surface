# Flat Ring History Phase 5A Evidence: Resize As Visual Projection

## Scope

Phase 5A makes retained-history resize behavior a visual projection and layout
generation concern before immutable ring authority cutover. The implementation
does not move ring authority, public projection policy, hyperlink authority,
lookup cache ownership, selection stale policy, or retained storage authority.

Implemented scope:

- Retained row resize no longer mutates retained row cell storage.
- Resize invalidates visual layout through grid reflow generation only.
- Render snapshot and selection text materialization generate current-geometry
  row projections on demand.
- Selection visual leases continue to use handle resolution and grid reflow
  generation.
- Orphaned in-place retained resize helpers were deleted.

Out of scope for this phase:

- Ring authority switch.
- Hyperlink authority migration.
- Lookup cache replacement.
- Public projection policy changes.
- Selection stale policy changes beyond existing geometry generation and
  handle resolution.
- Storage format or recovery authority changes.

## Geometry Generation Policy

Retained records remain content records. Resize is represented by geometry
generation, not content generation.

- `Terminal_screen_model::resize` still publishes a grid reflow event when the
  grid size changes.
- Retained rows preserve their stored cells and source metadata across resize.
- Current-width visual rows are generated at materialization time for render
  snapshots and selection text extraction.
- Generated visual rows repair wide-cell edge cases in the projected geometry.
- Growth beyond stored row width exposes blank projected cells without writing
  them into retained storage.
- Content generation is not advanced by retained row geometry projection.

This keeps content generation and geometry generation separate: row handles and
content leases describe retained content, while visual leases additionally
depend on grid reflow generation and current geometry.

## Behavior Axis

The intended user-facing behavior is preserved:

- Height resize can expose detached retained scrollback through the existing
  snapshot materialization path.
- Width shrink projects retained rows into the narrower visual geometry.
- Width growth projects stored retained cells and provides trailing blanks as
  visual cells.
- Wide spans at the projected right edge are repaired in the visual projection.
- Selection after resize resolves retained handles and materializes text from
  the current visual geometry.

No broad behavior migration is included in this phase. If future work changes
wrap/reflow semantics, public projection stale policy, or selection stale
policy, that work must be split from Phase 5A.

## Recovery State

Recovery ownership is unchanged. The recovery path still produces retained
records through the existing accepted producer path and is not used as a resize
escape hatch. Resize does not require mutable retained records during recovery.

## Evidence

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_screen_operations vnm_terminal_backend_session vnm_terminal_render_snapshot --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_(screen_operations|backend_session|render_snapshot)$"" --output-on-failure"
```

Final gate output summary:

- `vnm_terminal_render_snapshot` passed.
- `vnm_terminal_screen_operations` passed.
- `vnm_terminal_backend_session` passed.
- `100% tests passed, 0 tests failed out of 3`.

Coverage added in the focused screen operation test:

- Resize height and width scenarios.
- Detached scrollback materialization after resize.
- Selection after resize through retained handle resolution.
- Wide span projection at a narrowed right edge.
- Trailing blank projection after width growth.
- Retained content cells, content generation, and source width remain unchanged
  across resize.

## Deletion And Orphan Audit

Deleted implementation helpers:

- `Terminal_screen_model::resize_scrollback_rows`
- `Primary_backing_buffer::mutate_retained_history_rows`

Audit command:

```powershell
rg -n "resize_scrollback_rows|mutate_retained_history_rows" include src tests docs/primary_backing_redesign_plans -g "*.h" -g "*.cpp" -g "*.md"
```

Audit result:

- No remaining `include`, `src`, or `tests` references.
- Remaining mentions are historical later-phase planning notes only.

No Phase 5A production scaffold was added using `_v2`, `_legacy`, fallback,
mirror, resize escape hatch, or storage authority language.

## Phase Gate Table

| Gate | Phase 5A result |
| --- | --- |
| Scope | Retained resize is implemented as visual projection and geometry generation for render and selection materialization only. |
| Behavior axis | Documented visible resize behavior is preserved; no broad wrap/reflow behavior migration is included. |
| Recovery state | Recovery remains on the existing retained record producer path and does not gain mutable resize storage. |
| Evidence | Focused render snapshot, screen operation, and backend session gates passed through `vcvarsall` x64. |
| Baseline outcome | Retained rows are not resized in place; content generation and source metadata remain stable across resize. |
| Exit predicate | Current-geometry render and selection output are generated without retained content mutation, while visual leases use geometry generation plus existing handle resolution. |
| Deletion ownership | Phase 5A owns deletion of orphaned in-place retained resize helpers and their direct call sites. |
| Rollback mechanism | Revert the Phase 5A code changes, the focused screen operation test, this evidence artifact, and the README entry. |
| Split triggers | Stop and split work if ring authority, public projection stale policy, selection stale policy, hyperlink authority, lookup cache, storage format, or broad resize behavior semantics must change. |
