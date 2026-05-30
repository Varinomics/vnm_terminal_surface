# Flat Ring History Phase 2B Evidence: Viewport Handle Resolution

## Phase 2B Scope

Phase 2B routes live primary detached viewport anchoring through
`terminal_history_handle_t` resolution. It does not change selection policy,
public projection behavior, the retained-history backend, producer storage,
resize projection, or later storage work.

## Stale Detached Viewport Policy

Stale detached viewport policy is oldest-live clamp. When the retained row that
anchors a live primary detached viewport no longer resolves, the viewport clamps
to the oldest live retained row. If no retained rows survive, it clamps to tail
with `offset_from_tail == 0`.

This preserves the established behavior recorded before Phase 2B: appends keep a
detached viewport anchored on the same retained row, scrollback-limit eviction
keeps the viewport inside live retained bounds, and clear-history returns the
viewport to tail. Phase 2B changes the proof mechanism from eviction-coordinate
repair to handle resolution plus this explicit clamp policy.

## Phase Gate

| Gate | Phase 2B outcome |
|---|---|
| Scope | Live primary detached viewport anchoring now captures the top retained row as `terminal_history_handle_t` before model mutation and resolves it after model mutation; no selection policy, public projection, ring backend, producer rewrite, resize projection, or storage migration was implemented. |
| Behavior axis | Exact live handles keep the detached retained row at the same viewport row across retained-history append. Stale live handles clamp to the oldest live retained row, or tail when retained history is empty. Tail-following remains offset `0`; resize keeps the existing projection baseline without migration. |
| Recovery state | Recovered repaint rows keep existing provenance and retained-history lookup behavior. Phase 2B does not use recovery as storage evidence and does not change producer or recovery policy. |
| Evidence | `cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build --target vnm_terminal_backend_session vnm_terminal_viewport --config Debug && ctest --test-dir build -R "vnm_terminal_(backend_session\|viewport)" --output-on-failure'` passed on 2026-05-30. |
| Baseline outcome | Existing baseline behavior kept detached viewport anchors stable across append, clamped them after eviction/shrink, cleared them to tail after clear-history, preserved tail-following, and preserved retained text after resize. Phase 2B preserves retained-history mutation outcomes through handle resolution and leaves resize projection behavior unchanged. |
| Exit predicate | Live primary detached viewport sync no longer consumes viewport eviction arithmetic. Focused backend-session coverage exercises detached append, clear, eviction/scrollback-limit shrink, resize, and tail-following. Focused viewport coverage exercises controller bounds without the deleted backing-delta adapter. |
| Deletion ownership | Phase 2B deletes `terminal_backing_delta_viewport_sync.*` and removes `Terminal_viewport_controller::sync_scrollback_rows`. Remaining eviction-delta consumers are selection row-origin generation in `Terminal_session::advance_selection_content_basis_for_model_result` and deferred synchronized-output release accounting in `Terminal_session::record_blocked_synchronized_row_origin_change` / `model_result_with_deferred_synchronized_row_origins`; Phase 2C owns public projection policy, later storage phases own producer/result fields. |
| Rollback mechanism | Revert this Phase 2B change set: `viewport_contract.h`, `viewport_contract.cpp`, `terminal_screen_model.h`, `terminal_screen_model.cpp`, `terminal_session.h`, `terminal_session.cpp`, focused viewport/backend-session tests, `CMakeLists.txt`, this evidence artifact, and the README entry. |
| Split triggers | No public projection bounds or synchronized-output release behavior changed. If future viewport evidence requires changing public projection release, hidden public viewport scrolling, or synchronized-output release reconciliation, split that work into Phase 2C or the owning later publication phase. |
