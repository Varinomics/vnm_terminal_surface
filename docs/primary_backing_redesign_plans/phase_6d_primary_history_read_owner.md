# Phase 6D primary-history read owner

This is the durable Phase 6D gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 6D is a no-behavior storage-owner cleanup. It removes the remaining
model-level retained-history deque accessor and routes retained-history size,
empty checks, row lookup, and const iteration through `Primary_backing_buffer`.

## Scope

Phase 6D owns:

1. `Primary_backing_buffer::retained_history_empty`.
2. `Primary_backing_buffer::retained_history_size`.
3. `Primary_backing_buffer::retained_history_row`.
4. `Primary_backing_buffer::for_each_retained_history_row`.
5. Removal of `Terminal_screen_model::primary_scrollback_rows`.

## Non-goals

Phase 6D does not change retained-history mutation policy, append, clear,
eviction, resize/reflow behavior, viewport synchronization, selection behavior,
public projection, transcript/replay behavior, DSR behavior, or recovery policy.

Phase R supersedes the earlier temporary-compatibility framing. This phase only
changes how existing code reads current retained-history state.

## Phase 6D gate table

| Gate entry | Phase 6D value |
| --- | --- |
| Scope | Route retained-history read access through `Primary_backing_buffer`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for `primary_scrollback_rows()` and direct retained-history container access outside the owner implementation, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, and `vnm_terminal_transcript`. |
| Baseline outcome | Existing retained-history count, row lookup, clear, recovery-guard, snapshot, scalar, delta, and viewport behavior remains field-equivalent. |
| Exit predicate | No `primary_scrollback_rows()` call remains; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore the const `primary_scrollback_rows()` accessor and direct retained-history reads. Phase 6D is incomplete while that accessor remains. |
| Deletion gate | Phase 10 may keep stable owner read APIs and remove only temporary compatibility helpers made obsolete by later phases. |
