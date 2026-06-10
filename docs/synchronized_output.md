# Synchronized Output

This document describes how the surface implements DEC synchronized output
(private mode 2026): how a hold starts and ends, what is deferred during a
hold, the scroll policies that apply while a hold is active, and the
stale-hold recovery path.

## Mode 2026 In The Screen Model

`Terminal_screen_model` tracks the mode in
`Terminal_mode_state::synchronized_output`
(`src/terminal_screen_model.cpp`, `set_synchronized_output_mode`):

- Set (`CSI ? 2026 h`): pending changes are published first, then the hold
  begins. Content written during the hold accumulates without publication.
- Reset (`CSI ? 2026 l`): the changes accumulated during the hold are
  collected and released as one publication, so the host repaints once with
  the complete frame the application composed.
- `DECRQM` for mode 2026 reports 1 (set) or 2 (reset).

## Session Publication Gate

`Terminal_session` does not publish render snapshots while the model holds
(`src/terminal_session.cpp`; the publication predicate checks
`mode_state().synchronized_output`). The session also scans incoming backend
bytes for mode 2026 boundaries so command processing chunks at them: the next
set sequence is scanned unconditionally
(`next_synchronized_output_set_sequence`), and the next reset sequence is
scanned only while a hold is active under the immediate-public-projection
policy (`next_synchronized_output_reset_sequence`). The session latches the
scroll policy when a hold begins
(`latch_synchronized_output_scroll_policy_for_new_hold`).

## Scroll Policies During A Hold

`Terminal_synchronized_output_scroll_policy`
(`include/vnm_terminal/internal/render_snapshot.h`) has two values:

- `DEFER_UNTIL_CONTENT_PUBLICATION` (default): viewport scrolling during a
  hold is deferred. A wheel scroll records a deferred intent (the scroll
  diagnostics report `Scroll_action::DEFERRED_INTENT_RECORDED`, and no-op
  causes `SYNCHRONIZED_OUTPUT_DEFERRED` / `SYNCHRONIZED_OUTPUT_PUBLISHED`),
  and the intent is applied when the hold releases.
- `IMMEDIATE_PUBLIC_PROJECTION`: scrolling is served immediately from a
  retained public projection of the last published content. These snapshots
  carry `basis = PUBLIC_PROJECTION` and `purpose = SCROLL` (see
  `render_snapshot_contract.md`), use the primary buffer only, and never
  reveal unpublished hold content.

The policy in effect for a hold is the policy latched at hold entry:
`effective_synchronized_output_scroll_policy()` returns the latched value
until release. Changing the configured policy mid-hold takes effect for the
next hold and records
`Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD` in the
public scroll diagnostics. The first-party app exposes the policy as
`--synchronized-output-scroll-policy defer|immediate-public`
(`vnm_terminal/src/app_cli.cpp`).

## Release And Reconciliation

When a hold releases (mode reset, or forced release), the accumulated
changes publish as one snapshot, and any public-projection scroll state
reconciles against the newly published content. The outcome is recorded in
`Terminal_release_reconciliation_result` (sticky tail, exact anchor,
retained-id best effort, nearest successor/predecessor, oldest available
live, deferred offset, incompatible buffer) inside
`Terminal_public_scroll_diagnostics`, which also carries the effective
policy, the public and live viewports before and after release, and the
projection disable reason when a projection had to be dropped
(geometry invalidated, memory pressure, projection invalidated, unsupported
buffer).

## Stale-Hold Recovery

A hold is application-controlled, so a misbehaving program could hold
forever. The surface arms a single-shot recovery timer whenever the session
reports blocked render publication
(`VNM_TerminalSurface::sync_synchronized_output_recovery_timer`,
`src/vnm_terminal_surface.cpp`). When the timer fires, the surface first
drains pending backend callback events within a bounded budget, giving a
late mode reset the chance to release the hold naturally; if publication is
still blocked after the drain, it calls
`Terminal_session::force_release_synchronized_output_without_backend_drain`,
which releases the model hold and publishes the accumulated content. The
timeout is the public `synchronizedOutputStaleTimeoutMs` property
(default 1000 ms). The timer stops as soon as publication unblocks.

## Diagnostics And Tests

The wheel and scroll paths report hold interaction through the typed scroll
diagnostics on `VNM_TerminalSurface` (`Scroll_noop_cause`, `Scroll_action`)
and, in transcript-enabled builds, through the transcript scroll and
wheel-trace events. The behavior is pinned by the backend-session and
surface-host suites (grep `synchronized` under `tests/`), and the repro tool
`vnm_terminal/tools/synchronized_output_scroll_policy_repro.ps1` drives a
deterministic hold/scroll/release sequence against a real app build.
