# Flat Ring History Phase 1 Evidence: History Handle Vocabulary

## Phase 1 Scope

Phase 1 introduces the canonical internal retained-history resolution vocabulary
without changing retained storage authority, public projection policy, viewport
policy, or selection lifetime policy.

Implemented vocabulary:

1. `terminal_history_handle_t`
2. `Terminal_history_resolution_status`
3. `terminal_history_handle_from_retained_identity`
4. `terminal_selection_line_lease_from_retained_identity`

The Phase 1 backing model derives handles from the existing retained-line
identity. `retained_line_id` maps to both the handle byte-sequence placeholder
and the row sequence. `record_bytes` is the Phase 1 placeholder record size.
The handle epoch is the retained-identity epoch.

Byte-sequence and record-size resolution statuses are therefore
placeholder-backed in Phase 1. The focused Phase 1 tests exercise those
statuses with synthesized handles until the flat-ring backend supplies real
byte sequences and record sizes.

## Phase 1 Owner Decision: Retained-Line Compatibility Identity

Retained-line compatibility identities survive during the migration as the
public, test, transcript, render-provenance, and public-projection compatibility
surface. The canonical internal resolution vocabulary is
`terminal_history_handle_t`.

This keeps existing visible contracts stable while internal proof paths stop
using raw deque indexes and resolve through a single handle shape. Phase 5C owns
retained lookup cache replacement in the screen-model lookup surface and its
selection/public-projection lookup consumers if the retained-line lookup surface
remains.

## Behavior Preservation Note

Selection line-lease validation preserves the pre-Phase 1 ambiguity guard. A
lease resolves only when exactly one live retained-history row has the requested
row sequence and that row's full handle resolves with `OK`. Duplicate retained
identity matches or generation/placeholder mismatches reject the visual proof.

## Phase Gate

| Gate | Phase 1 outcome |
|---|---|
| Scope | Internal history handle and status vocabulary added; no ring backend, producer rewrite, viewport policy change, selection policy change, or public projection policy change. |
| Behavior axis | Existing retained-line compatibility fields remain visible at public/test/provenance boundaries; internal lookup and selection lease proof use `terminal_history_handle_t`. |
| Recovery state | Recovered primary repaint rows keep existing provenance and resolve through the same handle-backed retained lookup seam. |
| Evidence | `cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build --target vnm_terminal_backend_session vnm_terminal_render_snapshot --config Debug && ctest --test-dir build -R "vnm_terminal_(backend_session\|render_snapshot)" --output-on-failure'` passed on 2026-05-30. |
| Baseline outcome | `vnm_terminal_render_snapshot` and `vnm_terminal_backend_session` passed; retained-line selection lease behavior preserves ambiguity rejection and payload/viewport behavior remains unchanged. |
| Exit predicate | One canonical internal handle vocabulary exists; retained-line lookup accepts handles; selection leases carry handles; stale/status vocabulary is covered by focused backend-session tests. |
| Deletion ownership | Phase 1 removed the raw retained-line lookup overload instead of leaving a parallel API. No new orphaned conversion helper, `_v2`, `_legacy`, production fallback, or permanent alias was introduced. |
| Rollback mechanism | Revert this Phase 1 change set: `selection_contract.h`, `terminal_screen_model.h`, `terminal_screen_model.cpp`, `terminal_session.cpp`, focused tests, and this evidence artifact. |
| Split triggers | No consumer conversion required a visible selection, viewport, or public projection behavior change. If future handle migration needs such a behavior decision, it belongs to Phase 2A, 2B, or 2C respectively. |
