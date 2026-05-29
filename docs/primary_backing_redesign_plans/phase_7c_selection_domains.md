# Phase 7C selection domains

This is the durable Phase 7C gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 7C makes selection anchor domains explicit and names the selection-facing
backing event used by primary scrollback eviction. It preserves current copy
payload behavior: when backing identity is no longer safe to project, selection
visuals detach and the finalized payload remains copyable.

## Scope

Phase 7C owns:

1. `Terminal_selection_anchor_domain` on committed selection state, published
   selection source identities, and visual leases.
2. Session access to the current selection anchor domain for internal
   regression tests and diagnostics.
3. A named `terminal_selection_backing_event_t` path for primary scrollback
   eviction, replacing the session's raw eviction-count mutation at the
   selection boundary.
4. Regression coverage for primary backing selections, alternate active-grid
   selections, alternate transitions, primary scrollback eviction, ED3
   scrollback clear, destructive resize/reflow detachment, and payload-only
   detachment.

## Non-goals

Phase 7C does not change core storage mutation behavior, viewport or wheel
public-bound behavior, render snapshot row proof, resize/reflow storage policy,
synchronized-output release reconciliation, or recovery policy.

The active-model selected-text fallback for compatible published sources remains
compatibility behavior in this phase. Phase 7C names the domain carried through
that fallback instead of removing it.

## Phase 7C gate table

| Gate entry | Phase 7C value |
| --- | --- |
| Scope | Add explicit selection anchor domains, stamp them into source identities and visual leases, expose current session selection domain, and route primary scrollback eviction through a named selection backing event. |
| Behavior axis | Selection domain/invalidation state only; finalized copy payload preservation remains unchanged. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, focused build and CTest for `vnm_terminal_backend_session`, and expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Previous selection state carried buffer, viewport, basis, and retained-line proof, but no named domain. Destructive events already detached visual spans while preserving payload in many paths; Phase 7C makes the state explicit and pins it with named tests. |
| Exit predicate | Primary selections report `PRIMARY_BACKING`; alternate selections report `ALTERNATE_ACTIVE_GRID`; detached preserved-payload selections report `PAYLOAD_ONLY`; cleared selections report `NONE`; scrollback eviction and ED3 clear do not leave stale visual leases. |
| Manual gate | Focused manual copy/paste from primary history, after alternate enter/leave, after oldest-row eviction, and after ED3 scrollback clear. |
| Rollback mechanism | Remove the anchor-domain fields/accessor, restore `sync_viewport_from_model_result` to apply primary scrollback eviction through the previous selection-boundary path, and remove the Phase 7C tests. |
| Deletion gate | `UNRESOLVED_ACTIVE_GRID` may be deleted later if all provisional/controller-only selections are stamped through session source identity before they become observable. |

## Domain policy

`PRIMARY_BACKING` covers primary-buffer logical selection ranges, including
primary active rows and retained primary history. `ALTERNATE_ACTIVE_GRID` covers
alternate-screen active-grid selections. `PAYLOAD_ONLY` means the selected text
remains copyable, but no backing row identity is safe to project. `NONE` means
there is no selection. `UNRESOLVED_ACTIVE_GRID` is limited to controller-local
provisional state before the session stamps a source identity.

Render snapshot row indexes remain presentation-local. They are not promoted to
persistent selection anchors in this phase.
