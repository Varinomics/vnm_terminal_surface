# Phase 10A scaffold cleanup

This is the durable Phase 10A cleanup artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 10A removes obsolete migration scaffold after the selection-domain batch
introduced named backing events. This batch is intentionally narrow: it keeps
product behavior unchanged while deleting the old raw selection-eviction
convenience path and pruning unused test-observation vocabulary.

## Scope

Phase 10A owns:

1. Removing the raw `Selection_contract_controller::apply_scrollback_eviction`
   wrapper now that primary scrollback eviction has a named backing event.
2. Updating the remaining direct unit test caller to use
   `terminal_selection_backing_event_t`.
3. Removing the orphaned `Terminal_screen_model::rows_have_matching_text`
   helper left behind by earlier recovery-inference experiments.
4. Pruning unused `Primary_backing_observation_classification` and
   `Scrollback_delta_operation_annotation` enum values from the test helper
   vocabulary.
5. Updating stale rollback prose that still named the removed raw helper.

## Non-goals

This batch does not change storage mutation behavior, viewport or wheel
publication behavior, selection payload preservation, resize/reflow policy,
transcript schema, synchronized-output behavior, or recovery policy.

## Phase 10A gate table

| Gate entry | Phase 10A value |
| --- | --- |
| Scope | Delete obsolete scaffold left behind after Phase 7C and earlier recovery-inference experiments, and keep the named event path as the only selection eviction API. |
| Behavior axis | Cleanup only; no product behavior changed. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: focused orphan grep for the removed wrapper and pruned enum values, focused build and CTest for `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_viewport`; Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for direct active-grid owner field access, and expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Phase 7C routed session selection eviction through a named backing event, but the controller still exposed a raw integer wrapper for tests and rollback prose still mentioned it. |
| Exit predicate | No production or test caller uses `apply_scrollback_eviction`; no caller or declaration of `rows_have_matching_text` remains; no unused observation enum values remain; the focused and expanded gates pass. |
| Manual gate | `none`; this batch is mechanical cleanup covered by compile-time symbol removal and existing regression tests. |
| Rollback mechanism | Restore the raw wrapper declaration/definition and the direct test call only if the named backing event path is rolled back. |
| Deletion gate | Completed in this batch. |

## Cleanup notes

The named backing event is now the sole selection-boundary API for primary
scrollback eviction. Tests that exercise the controller directly construct the
same event payload used by session code, so they no longer keep an extra
integer-only API alive.
