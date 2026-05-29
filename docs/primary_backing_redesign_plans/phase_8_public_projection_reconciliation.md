# Phase 8 public projection reconciliation

This is the durable Phase 8 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 8 makes publication behavior explicit after the storage, viewport, wheel,
and selection-domain batches. The current Phase 8 batch is regression-only: it
adds exact backend-session coverage for synchronized-output holds where hidden
live content mutates, public projection scrolls locally, and release reconciles
back to live content.

## Scope

Phase 8 owns:

1. Public projection capture from the latest safe live-content basis at
   synchronized-output entry.
2. Public scroll snapshots that source visible rows, styles, hyperlinks,
   cursor/mode fields, selection spans, and row-origin metadata from copied
   public projection data while live publication is blocked.
3. Protection of `latest_content_render_snapshot` from public scroll snapshots.
4. Release reconciliation from public projection state back to the live
   viewport using retained-row identity or the named fallback diagnostics.
5. Regression coverage for a combined hold that rewrites active rows, grows
   hidden scrollback, performs public scroll, and releases hidden content
   exactly once.

## Non-goals

Phase 8 does not change core storage mutation behavior, viewport delta
production, wheel-boundary policy, selection-domain policy, resize/reflow
policy, transcript schema, surface event delivery, or recovery policy.

## Phase 8 gate table

| Gate entry | Phase 8 value |
| --- | --- |
| Scope | Add Phase-8-named regression coverage for public projection field sourcing, non-advancing public scroll publication, and synchronized-output release reconciliation. |
| Behavior axis | Publication/public-projection reconciliation. This batch is regression-only; no production behavior changed. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, focused build and CTest for `vnm_terminal_backend_session`, and expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Existing public projection machinery already captured safe public rows, kept public scroll snapshots separate from live-content publication, and reconciled release through retained-row identity. Phase 8 adds exact combined coverage so this remains reviewable. |
| Exit predicate | Hidden live text, styles, hyperlinks, cursor/mode changes, and row-origin changes do not leak into public scroll snapshots; public scroll does not advance the latest live-content basis; release publishes live content once and clears public projection lifecycle state. |
| Manual gate | Synchronized-output fixture that rewrites active rows, grows hidden scrollback, scrolls public view, and releases. |
| Rollback mechanism | Remove the Phase 8 regression tests and this artifact. If a later Phase 8 behavior fix is needed, rollback only that named publication subphase. |
| Deletion gate | `none`; these are durable publication regression tests. |

## Compatibility notes

Public projection remains a publication layer, not storage. Public scroll
snapshots use `Terminal_render_snapshot_basis::PUBLIC_PROJECTION` and do not
replace the latest safe live-content basis. Release snapshots return to
`Terminal_render_snapshot_basis::LIVE_CONTENT` and may advance the live-content
basis exactly once when hidden content becomes publishable.
