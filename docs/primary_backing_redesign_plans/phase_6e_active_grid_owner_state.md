# Phase 6E active-grid owner state

This is the durable Phase 6E gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 6E is a no-behavior storage-owner cleanup. It routes primary and
alternate active-grid state access through owner methods instead of reaching
directly through `m_primary_backing.active_grid` or
`m_alternate_grid.active_grid`.

## Scope

Phase 6E owns:

1. `Primary_backing_buffer::active_grid_state`.
2. `Alternate_active_grid::active_grid_state`.
3. Mechanical routing of active-buffer save/restore, reset, resize,
   alternate-enter/leave, hyperlink retention, and active-grid read helpers
   through those owner methods.

## Non-goals

Phase 6E does not change active-grid height, primary/alternate save/restore
policy, resize/reflow policy, retained-history behavior, viewport
synchronization, selection behavior, public projection, transcript/replay
behavior, DSR behavior, or recovery policy.

Phase R supersedes the earlier temporary-compatibility framing. This phase only
changes how existing code reaches the same active-grid state.

## Phase 6E gate table

| Gate entry | Phase 6E value |
| --- | --- |
| Scope | Route owner active-grid state access through owner methods. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, and `vnm_terminal_viewport`. |
| Baseline outcome | Existing active-grid, alternate-screen, resize, snapshot, scalar, delta, and viewport behavior remains field-equivalent. |
| Exit predicate | No direct `m_primary_backing.active_grid` or `m_alternate_grid.active_grid` access remains outside owner method implementations; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore direct owner-field access. Phase 6E is incomplete while those direct accesses remain. |
| Deletion gate | Phase 10 may keep stable owner state APIs and remove only temporary compatibility helpers made obsolete by later phases. |
