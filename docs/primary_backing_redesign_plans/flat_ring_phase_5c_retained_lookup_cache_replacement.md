# Flat Ring History Phase 5C Evidence: Retained Lookup Cache Replacement

## Phase 5C Scope

Phase 5C replaces retained-line scan correctness with rebuildable lookup and
ordinal caches over live retained-history handles. The implementation remains
current-storage-backed until the authoritative ring cutover.

Non-goals preserved:

1. No ring authority switch.
2. No public retained-line contract removal.
3. No selection, viewport, public projection, resize, hyperlink, or storage
   cutover policy change.

## Cache Policy

Retained lookup caches are disposable. Primary and alternate lookup caches map
live row sequences to logical rows and logical row ordinals to
`terminal_history_handle_t` values. Cache rebuild walks the current live rows,
but every cache hit validates the current row's live handle identity before
returning an exact match, mismatch status, ordinal handle, or nearest live row.

Dropping the caches clears only acceleration state. Retained-history content
stays in the current backing storage and is recovered by rebuilding the caches
from live rows.

## Phase Gate

| Gate | Phase 5C outcome |
|---|---|
| Scope | `Terminal_screen_model` retained-line lookup and ordinal handle lookup now resolve through mutable rebuildable caches. Selection lease resolution uses the same lookup cache. Existing public/test/provenance retained-line identities remain visible because Phase 1 kept that compatibility surface. |
| Behavior axis | Exact lookup, content-generation mismatch, stale row sequence, nearest successor, nearest predecessor, cache drop/rebuild, and ordinal lookup are covered. Cache entries validate live handle identity before use and rebuild after invalidation. |
| Recovery state | Recovery behavior is unchanged. Recovered primary repaint rows keep existing provenance and continue to resolve through the handle-backed retained lookup seam. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_backend_session vnm_terminal_render_snapshot vnm_terminal_viewport vnm_terminal_history_row_traversal --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_(backend_session\|render_snapshot\|viewport\|history_row_traversal)$"" --output-on-failure"` passed on 2026-05-30. |
| Baseline outcome | Existing Phase 1 retained-line handle statuses remain intact. Existing Phase 2A selection payload-only stale policy, Phase 2B detached viewport clamp policy, and Phase 2C public projection release reconciliation continue to pass through the cache-backed lookup seam. |
| Exit predicate | Retained-line lookup no longer performs scan-only correctness resolution. Row ordinal handle lookup is rebuildable from live rows. Focused retained-line, selection, viewport, public projection, render-snapshot, backend-session, and traversal gates pass through the MSVC x64 environment. |
| Deletion ownership | The scan-only selection descriptor lookup path was removed; descriptor resolution now uses `retained_line_lookup`. No `_v2`, `_legacy`, storage mirror, fallback correctness path, public contract removal, or ring cutover path was introduced. |
| Rollback mechanism | Revert this Phase 5C change set: `terminal_screen_model.h`, `terminal_screen_model.cpp`, focused backend-session tests, this evidence artifact, and the README entry. |
| Split triggers | If retained-line compatibility identity is later removed, return to the Phase 1 public/test identity decision. If authoritative storage, resize projection, hyperlink cache ownership, or selection/viewport/public projection policy changes become necessary, split to the owning phase instead of expanding Phase 5C. |
