# Phase R4 recovery shift helper extraction

This is the fourth Phase R gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase R4 moves the text-matching shifted-repaint calculation out of
`Terminal_screen_model` into a dedicated internal recovery helper. The model
still captures candidate/current rows, owns retained-row acceptance, and
preserves the existing heuristic triggers. This keeps the extraction narrow and
avoids changing the recovery policy while making the matching logic separately
testable.

## Scope

Phase R4 owns:

1. Adding `terminal_repaint_recovery_shift_input_t` and
   `primary_repaint_recovery_shift_rows` in
   `terminal_repaint_recovery.h/.cpp`.
2. Routing `Terminal_screen_model::primary_repaint_recovery_shift_rows`
   through the helper using row text vectors.
3. Adding direct helper tests for distinct shifted repaint acceptance and
   repeated-row, blank-only, and explicit non-home false-positive suppression.

## Non-goals

This batch does not remove the repaint recovery heuristic, change production
recovery defaults, move candidate capture out of `Terminal_screen_model`, move
retained-row acceptance out of the model, change transcript schema, expose
recovery metadata in public render snapshots, or change any Phase R1/R2/R3
observable behavior.

## Phase R4 gate table

| Gate entry | Phase R4 value |
| --- | --- |
| Scope | Extract the shifted-repaint text-matching calculation into a dedicated internal recovery helper. |
| Behavior axis | `none`; this is a structure/testability batch preserving existing recovery behavior. |
| Recovery state | Production defaults unchanged. Recovery remains separately enabled/disabled; core storage gates remain recovery-disabled. |
| Evidence | Passed on 2026-05-29: independent final read-only review, Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, direct-owner access search, cleanup-orphan search, focused build and CTest for `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_viewport`, plus expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Phase R3 built explicit internal proposals, but the shifted-repaint matching calculation still lived directly in `Terminal_screen_model`. |
| Exit predicate | The helper returns the same accepted and suppressed shifts as the Phase R model fixtures; Phase R1/R2/R3 model tests still pass. |
| Manual gate | `none`; deterministic helper and model tests cover this batch. |
| Rollback mechanism | Remove `terminal_repaint_recovery.h/.cpp`, restore the shift calculation body in `Terminal_screen_model`, and remove direct helper tests. |
| Deletion gate | A later Phase R extraction batch may move candidate capture or proposal construction out of the model after their required row/cursor/identity seams are explicit. |

## Compatibility notes

The old heuristic remains active when
`recover_scrollback_from_primary_repaints` is enabled. The helper operates on
row text vectors only; it does not mutate storage, active rows, viewport state,
selection state, or publication state.
