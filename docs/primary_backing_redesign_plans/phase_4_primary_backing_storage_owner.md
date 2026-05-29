# Phase 4 primary backing storage owner

This is the durable Phase 4 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 4 is a no-behavior production refactor. It names the current primary and
alternate storage owners without changing active-grid height, viewport behavior,
resize policy, selection policy, public projection, or recovery behavior.

## Scope

Phase 4 owns:

1. `Primary_backing_buffer`, containing the saved primary active grid state plus
   retained primary history.
2. `Alternate_active_grid`, containing only alternate active grid state and no
   history vector.
3. Removal of the standalone active-grid member from `Terminal_screen_model`;
   active rows are accessed through `active_grid_rows()` and the active owner.
4. Removal of the standalone scrollback deque from `Terminal_screen_model`;
   retained history lives inside `Primary_backing_buffer`.
5. Mechanical owner routing for resize, alternate enter/leave, scrollback
   append, eviction, clear, retained-line lookup, render snapshots, selection
   reads, hyperlink retention, and recovery compatibility code.

## Non-goals

Phase 4 does not change row counts, viewport panning, DSR coordinates, resize
policy, public projection behavior, selection behavior, write semantics, or
recovery policy. The old repaint recovery heuristic is only mechanically routed
through active-grid and retained-history accessors; it remains quarantined for
Phase R.

## Phase 4 gate table

| Gate entry | Phase 4 value |
| --- | --- |
| Scope | Mechanical storage owner extraction inside `Terminal_screen_model`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; core backing tests remain recovery-disabled through Phase 0A helpers. Recovery compatibility code is not extended. |
| Evidence | Focused gate passed on 2026-05-29: static guard, `git diff --check`, build, and `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_transcript`. Expanded Phase 4 gate also passed: `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, and `vnm_terminal_capture_replay_conformance`. |
| Baseline outcome | Field-equivalent behavior. `m_rows`, standalone `m_scrollback`, `m_primary_buffer`, and `m_alternate_buffer` are gone from the model source/header outside owner names and accessors. |
| Exit predicate | A reviewer can identify the primary owner and alternate owner; alternate storage has no retained-history vector; focused gates pass; no temporary comparator or mirror exists. |
| Manual gate | `none`. |
| Rollback mechanism | Revert the storage-owner extraction batch and restore direct member access. The phase is incomplete while direct standalone storage members are restored. |
| Deletion gate | Phase 10 keeps the final owner API and removes only temporary compatibility helpers whose callers have been migrated. |

## Recovery boundary

Phase 4 does not move recovery into the owner API as a policy. Phase R now owns
accepted recovered-row appends through normal storage APIs with recovered
provenance.
