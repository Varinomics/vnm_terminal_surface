# Phase R5 recovery disable switch

This is the fifth Phase R gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase R5 adds an explicit runtime switch for the optional primary repaint
recovery policy while keeping the surface as the owner of platform defaults.
The host application may disable the policy for a session, but it does not
define a second platform default.

## Scope

Phase R5 owns:

1. Adding `VNM_TerminalSurface::primary_repaint_recovery_enabled` as the public
   surface property and forwarding live changes through `Terminal_session` to
   `Terminal_screen_model`.
2. Cancelling pending resize and primary repaint recovery candidates when the
   policy is disabled.
3. Keeping transcript replay recovery disabled unless the transcript explicitly
   records the recovery flag.
4. Updating the Phase 0B static guard allowlist for the new Phase R5-owned
   recovery policy lines.

## Non-goals

This batch does not change the default surface policy, the recovered-row
matching heuristic, recovered-row provenance, backing-delta structure, or core
storage behavior. Ordinary scrollback must remain testable with recovery
disabled.

## Phase R5 gate table

| Gate entry | Phase R5 value |
| --- | --- |
| Scope | Public disable switch, session/model propagation, pending-candidate cancellation, transcript replay default consistency, and guard/doc refresh. |
| Behavior axis | Optional recovery policy can now be disabled at launch or at runtime. |
| Recovery state | Surface owns the platform default; the application only passes an explicit disable override. Core storage gates remain recovery-disabled. |
| Evidence | Toggle tests in screen-model and backend-session coverage, parser/apply tests in the host application, Phase 0B diff and full-tree guard, and focused transcript replay config coverage. |
| Baseline outcome | Phase R1-R4 restored and structured the recovery policy, but the host application had no explicit disable flag and replay inferred recovery differently depending on whether `session_config` existed. |
| Exit predicate | The disable flag reaches the live model, disabling cancels pending candidates, replay does not infer recovery from missing config, and Phase 0B guard passes. |
| Manual gate | Launch `vnm_terminal --disable-primary-repaint-recovery` and verify the session runs without primary repaint recovery. |
| Rollback mechanism | Remove the public surface property, session/model setters, app flag, parser test, toggle tests, and this guard allowlist amendment. |
| Deletion gate | A later Phase R extraction may replace the public boolean with a richer recovery-policy object only if it preserves an explicit disable path. |
