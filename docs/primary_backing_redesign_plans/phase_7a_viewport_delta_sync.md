# Phase 7A viewport delta sync

This is the durable Phase 7A gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 7A moves live viewport synchronization from independent scalar inference
to the named backing-delta stream for retained primary history and mode
transitions. It is intended to be field-equivalent for existing behavior.

## Scope

Phase 7A owns:

1. A small internal adapter that converts `Terminal_screen_model_result`
   backing deltas into viewport sync inputs.
2. Primary-history delta consumption for retained-history row count and
   row-origin movement:
   append, eviction, clear, discard, and explicit no-op boundaries.
3. Mode-transition delta consumption for active-buffer destination, while the
   published active-buffer-change gate remains the existing scalar result flag.
4. Session viewport synchronization through the adapter.
5. Tests that intentionally make scalar fields disagree with backing deltas,
   proving the viewport adapter prefers deltas for migrated families.
6. A temporary source tag on backing deltas. Phase R later removed that tag
   after accepted recovered rows gained explicit provenance.

## Non-goals

Phase 7A does not change storage mutation behavior, wheel/public-projection
bounds, selection anchor policy, synchronized-output publication policy,
resize/reflow policy, or recovery policy.

Phase R supersedes the earlier compatibility framing. Recovery-enabled append
deltas now use the canonical primary-history delta path only after Phase R
acceptance; Phase 7A tests do not use recovery as viewport evidence.

## Phase 7A gate table

| Gate entry | Phase 7A value |
| --- | --- |
| Scope | Add `terminal_backing_delta_viewport_sync_t`, route session viewport sync through it, and prove delta precedence for primary-history row sync plus mode-transition destination. |
| Behavior axis | `none`; existing viewport, selection, synchronized-output, and publication behavior remains field-equivalent. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Existing scalar-compatible row counts and row-origin movement are preserved for no-delta results and for delta families whose scalar fields are still published for compatibility. |
| Exit predicate | Viewport sync inputs for migrated primary-history fixtures and mode-transition destination come from backing deltas; no-delta and active-grid-only results keep a named scalar fallback; focused and expanded gates pass. |
| Manual gate | `none` for this field-equivalent adapter migration. |
| Rollback mechanism | Restore `Terminal_session::sync_viewport_from_model_result` to consume the named scalar fields directly and remove the adapter/tests from this phase. |
| Deletion gate | Later cleanup removes scalar viewport-sync fallback after all model results needed by viewport sync carry complete backing deltas. |

## Compatibility fallback

The adapter keeps `scrollback_rows`, `evicted_scrollback_rows`, and
`active_buffer_changed` scalar fallback for result objects that do not contain a
usable primary-history or mode-transition delta. This is a temporary
compatibility path, not a second source of truth. Tests pin it narrowly so
active-grid resize/reflow deltas cannot masquerade as retained-history changes.
Mode-transition deltas intentionally do not override the published
`active_buffer_changed` gate, because synchronized-output holds can carry hidden
mode-transition deltas before publication release.

Recovery-compatibility primary-history deltas were tagged in Phase 7A and
forced scalar fallback. Phase R1 added recovered retained-row provenance, and
Phase R2 removed the temporary delta source/fallback so accepted recovered rows
can drive viewport sync through the canonical backing-delta path.
