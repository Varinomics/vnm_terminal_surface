# Flat Ring History Phase 2C Evidence: Public Projection Handle Resolution

## Phase 2C Scope

Phase 2C routes public projection retained-history anchoring through
`terminal_history_handle_t` resolution. It does not change selection policy,
viewport anchoring policy, the retained-history backend, producer storage,
resize projection, or later storage work.

## Stale Public Projection Policy

Stale public projection policy is release-time live-bound reconciliation.
Copied public projection rows remain the only row payload source while
synchronized output is held. A public projection scroll snapshot must not read
hidden live retained history.

On synchronized-output release, detached public anchors resolve by
`terminal_history_handle_t`:

1. `OK` resolution with unchanged geometry restores the exact anchored retained
   row and visual fragment.
2. `OK` resolution with changed geometry restores the same retained row as
   best-effort and records the geometry-change diagnostic.
3. Content-generation mismatch records the content-generation diagnostic and
   reconciles to the nearest surviving live retained row when available.
4. Stale row sequence or missing retained row records the not-retained
   diagnostic and reconciles to the nearest surviving live retained row when
   available.
5. If no anchored or nearby retained row survives, release falls back to the
   oldest live retained row, or tail when retained history is empty.
6. Sticky-tail intent remains sticky-tail and does not use a stale detached
   anchor.
7. Deferred public scroll intent keeps the existing deferred-offset behavior and
   does not use hidden live rows before release.

This preserves the established user-visible behavior from the public projection
baseline: public scrolling during a hold is isolated to copied safe rows, and
release reconciles the final live viewport explicitly instead of retaining a
stale hidden-history read.

## Phase Gate

| Gate | Phase 2C outcome |
|---|---|
| Scope | Public projection copied rows and release anchors carry retained-history handles; capture and release reconciliation use handle resolution. No selection mutation redesign, viewport anchoring policy change, ring backend, producer rewrite, resize migration, or storage cutover is implemented. |
| Behavior axis | Stale public anchors reconcile at release by exact handle, geometry best-effort, nearest live retained row, oldest live retained row, or tail. Public scroll snapshots during a synchronized-output hold remain copied-projection-only. |
| Recovery state | Recovered repaint rows keep existing provenance and retained-history lookup behavior. Phase 2C does not use recovery as storage evidence and does not change producer or recovery policy. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_backend_session --config Debug && ctest --test-dir build -R ""vnm_terminal_backend_session"" --output-on-failure"` passed on 2026-05-30; `vnm_terminal_backend_session` built and `1/1` focused tests passed. |
| Baseline outcome | Existing public projection behavior is preserved: synchronized-output hold/release defers hidden content, public scroll during hold can publish copied projection snapshots, hidden live scrollback growth does not expand the public range, and release applies documented reconciliation. |
| Exit predicate | Public projection detached anchors resolve through `terminal_history_handle_t`; stale policy coverage exercises exact, stale/not-retained, public scroll during hold, hidden live scrollback growth isolation, and release reconciliation. |
| Deletion ownership | Phase 2C did not remove the last eviction-delta consumer. Remaining consumers are selection row-origin generation in `Terminal_session::advance_selection_content_basis_for_model_result`, deferred synchronized-output release accounting in `Terminal_session::record_blocked_synchronized_row_origin_change` / `Terminal_session::model_result_with_deferred_synchronized_row_origins`, and producer/result fields in `Terminal_screen_model`; later storage phases own those fields. No public-projection-only eviction-delta helper was orphaned. |
| Rollback mechanism | Revert this Phase 2C change set: `terminal_public_projection.h`, `terminal_public_projection.cpp`, `terminal_session.cpp`, focused backend-session tests, this evidence artifact, and the README entry. |
| Split triggers | If public projection needs a broader storage read adapter, stop and create a separate no-behavior adapter phase. If resize projection migration, selection policy, viewport anchoring, or retained storage authority becomes necessary, split to the owning phase instead of expanding Phase 2C. |
