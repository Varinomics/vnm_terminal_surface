# Phase 9 scrollback-limit viewport shrink

This is a Phase 9 behavior-axis artifact for
`primary_backing_buffer_consolidated_design.md`.

This Phase 9 batch handles one narrow axis: detached live viewport behavior when
the scrollback limit shrinks below the current retained-history depth. The
intended product contract is to preserve the detached viewport as far as
possible, clamping to the new oldest available retained row instead of snapping
to tail.

## Scope

Phase 9 owns:

1. Tightening the backend-session scrollback-limit shrink assertion from
   `offset_from_tail <= 2` to `offset_from_tail == 2`.
2. Recording that the existing implementation already satisfies the intended
   detached-viewport preservation contract.

## Non-goals

This batch does not change storage mutation behavior, retained-history
eviction, selection policy, public projection behavior, resize/reflow policy,
transcript schema, or recovery policy.

## Phase 9 gate table

| Gate entry | Phase 9 value |
| --- | --- |
| Scope | Assert exact detached viewport offset after shrinking live scrollback limit to `2`. |
| Behavior axis | Detached viewport preservation across scrollback-limit shrink. This batch is regression-only; no production behavior changed. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, focused build and CTest for `vnm_terminal_backend_session`, and expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | The previous test accepted any offset up to the new scrollback limit. The implementation already preserves the detached viewport at the new top (`offset_from_tail == 2`). |
| Exit predicate | Shrinking live scrollback limit while detached clamps to the new oldest available retained row and publishes exactly one updated snapshot. |
| Manual gate | `none`; backend-session coverage directly exercises the session contract. |
| Rollback mechanism | Restore the broader `offset_from_tail <= 2` assertion. |
| Deletion gate | `none`; this is durable regression coverage. |
