# Phase 7B wheel public projection bounds

This is the durable Phase 7B gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 7B makes public scroll and wheel-like viewport movement clamp against the
published/public viewport while synchronized output is blocked. Hidden live
scrollback may grow during a hold, but local public scrolling must not use those
hidden rows as its boundary until content is released.

## Scope

Phase 7B owns:

1. `Terminal_session::scroll_viewport_lines_from_published_state` clamping
   blocked/default synchronized-output movement to the supplied published
   viewport depth.
2. Regression coverage for immediate public-projection holds, where
   `Terminal_public_viewport_controller` already clamps to the captured public
   projection.
3. Regression coverage for invalidated public projections, where deferred
   release intents stay bounded by public projection depth.
4. Regression coverage that live hidden scrollback becomes authoritative again
   only after synchronized output releases.

## Non-goals

Phase 7B does not change storage mutation behavior, viewport delta production,
selection anchor/invalidation policy, synchronized-output release
reconciliation, public projection capture, resize/reflow policy, or recovery
policy.

## Phase 7B gate table

| Gate entry | Phase 7B value |
| --- | --- |
| Scope | Clamp public scroll/wheel movement to published/public viewport bounds while publication is blocked. |
| Behavior axis | Public scroll bounds during synchronized-output holds. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Immediate public-projection paths were already bounded by the public projection controller. Default blocked synchronized-output wheel-style movement could consult hidden live bounds; Phase 7B fixes that path. |
| Exit predicate | Immediate, invalidated, and default blocked publication paths cannot scroll past published/public bounds during a hold; after release, live bounds are authoritative again; focused and expanded gates pass. |
| Manual gate | `none` for the backend-session boundary fix. Surface wheel coverage may be added as regression coverage if later surface testing shows a route mismatch. |
| Rollback mechanism | Restore the prior `scroll_viewport_lines_from_published_state` live-scroll call for the blocked/default path only. The phase is incomplete while hidden live bounds are used during blocked publication. |
| Deletion gate | `none`; this is the intended public-bound behavior, not temporary scaffolding. |
