# Phase R3 recovery proposal metadata

This is the third Phase R gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase R3 introduces an explicit internal recovery proposal record while
preserving the existing repaint recovery heuristic. The proposal is still built
inside `Terminal_screen_model`; this is intentional. The current heuristic
depends on active-grid rows, retained-line identity repair, cursor-hidden finish
timing, and ambiguity state. Moving it to a separate translation unit before
the proposal shape is pinned would create a wider behavior-risk batch.

## Scope

Phase R3 owns:

1. Adding `terminal_recovery_proposal_t` with reason, status, provenance
   source, candidate-row count, recovered-row count, and ambiguity metadata.
2. Adding `Terminal_screen_model_result::recovery_proposals` for accepted
   recovery proposals.
3. Splitting primary repaint recovery into proposal construction
   (`primary_repaint_recovery_proposal`) and proposal acceptance
   (`accept_primary_repaint_recovery_proposal`).
4. Renaming the old shift inference helper to
   `primary_repaint_recovery_shift_rows`.
5. Proving accepted proposal metadata exists, false-positive paths emit no
   proposals, and proposal metadata does not leak into later non-recovery
   results.

## Non-goals

This batch does not remove the repaint recovery heuristic, change production
recovery defaults, extract recovery into a separate translation unit, change
transcript schema, expose recovery proposal metadata in public render snapshots,
or change the recovery true-positive/false-positive behavior covered by Phase
R1 and Phase R2.

## Phase R3 gate table

| Gate entry | Phase R3 value |
| --- | --- |
| Scope | Add explicit internal accepted-proposal metadata and split proposal construction from acceptance. |
| Behavior axis | `none`; this is a structure/metadata batch that preserves the Phase R1/R2 behavior. |
| Recovery state | Production defaults unchanged. Recovery remains separately enabled/disabled; core storage gates remain recovery-disabled. |
| Evidence | Passed on 2026-05-29: independent read-only scout, Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, direct-owner access search, cleanup-orphan search, R2 orphan-symbol search, focused build and CTest for `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_viewport`, plus expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Phase R1/R2 recovered rows through a named acceptance helper, but no durable proposal metadata existed in the model result. |
| Exit predicate | Accepted shifted repaint recovery emits exactly one proposal with `PRIMARY_REPAINT_SHIFTED_VISIBLE_ROWS`, `ACCEPTED`, `RECOVERED_PRIMARY_REPAINT`, four candidate rows, one recovered row, and ambiguity metadata; repeated-row, blank-only, and resize-adjacent false positives emit no proposals; a later non-recovery ingest emits no stale proposals. |
| Manual gate | `none`; deterministic model tests cover this batch. |
| Rollback mechanism | Remove `terminal_recovery_proposal_t`, `Terminal_screen_model_result::recovery_proposals`, proposal construction, and proposal metadata tests while keeping the prior Phase R1/R2 acceptance behavior. |
| Deletion gate | A later Phase R extraction batch may move proposal construction behind a helper or translation unit once this metadata shape is stable. |

## Compatibility notes

The old heuristic remains active when
`recover_scrollback_from_primary_repaints` is enabled. Phase R3 makes the
proposal boundary observable to internal tests without publishing it through
transcript or render snapshot contracts.
