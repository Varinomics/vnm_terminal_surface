# Phase R1 recovery provenance and resize guard

This is the first Phase R gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase R1 preserves the existing primary repaint recovery heuristic while moving
accepted recovered rows onto explicit retained-row provenance and a named
acceptance helper. It also fixes one reproduced recovery false positive:
resize-adjacent shifted repaints no longer synthesize retained history.

## Scope

Phase R1 owns:

1. Adding `Terminal_retained_line_provenance_source` to retained-row
   provenance, with `RECOVERED_PRIMARY_REPAINT` for accepted recovery rows.
2. Routing accepted primary repaint recovery rows through
   `accept_primary_repaint_recovery_proposal`.
3. Preserving the distinct shifted-repaint true-positive path covered by the
   existing heuristic.
4. Adding recovery-enabled model tests for repeated-row ambiguity, blank-only
   displaced rows, and resize-adjacent repaint suppression.
5. Adding a bounded post-resize guard that suppresses repaint recovery
   candidates during the resize repaint window.

## Non-goals

This batch does not remove the repaint recovery heuristic, change production
recovery defaults, change transcript schema, expose recovered provenance in
public render snapshots, move the observer out of the model, change public
projection release policy, or change core storage behavior with recovery
disabled.

## Phase R1 gate table

| Gate entry | Phase R1 value |
| --- | --- |
| Scope | Preserve existing primary repaint recovery while stamping accepted recovered rows with retained-row provenance and suppressing the reproduced resize-adjacent false positive. |
| Behavior axis | Recovery policy only: recovery-enabled resize-adjacent shifted repaint no longer appends recovered scrollback. |
| Recovery state | Production defaults unchanged. Recovery-enabled tests exercise Phase R behavior; core storage gates remain recovery-disabled. |
| Evidence | Before production fix, `vnm_terminal_screen_operations` failed on `Phase R recovery suppresses resize-adjacent repaint shifts`. Passed on 2026-05-29: independent read-only review after the direct-action guard symmetry fix, Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, direct-owner access search, cleanup-orphan search, focused build and CTest for `vnm_terminal_screen_operations`, `vnm_terminal_render_snapshot`, `vnm_terminal_transcript`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_viewport`, `vnm_terminal_backend_session`, and `vnm_terminal_windows_conpty_backend`, plus expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Distinct shifted repaint recovery already appended the displaced row, but accepted rows did not carry retained-row recovery provenance and resize-adjacent shifted repaint could be misclassified as recovered history. |
| Exit predicate | Distinct shifted repaint appends exactly one recovered row with `RECOVERED_PRIMARY_REPAINT`; repeated-row, blank-only, and resize-adjacent repaint fixtures append no recovered rows; recovery-disabled core fixtures remain passing. |
| Manual gate | `none`; this batch is covered by deterministic model tests. |
| Rollback mechanism | Remove the provenance source field, `accept_primary_repaint_recovery_proposal`, the resize recovery guard, and the Phase R1 tests. Recovery policy must remain explicitly decided by `recovery_baseline_correction.md`. |
| Deletion gate | Completed by Phase R2, which removed the `RECOVERY_COMPATIBILITY` delta source and scalar viewport fallback. |

## Compatibility notes

The old heuristic remains active when
`recover_scrollback_from_primary_repaints` is enabled. Phase R1 changes only the
acceptance boundary and one false-positive window. Phase R2 owns removing the
temporary recovery delta-source quarantine after recovered retained-row
provenance is available.
