# Phase 5 backing deltas

This is the durable Phase 5 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 5 exposes storage-level deltas from the backing model without migrating
viewport or publication consumers yet. It keeps existing scalar consumers
unchanged while adding an audit trail for retained-history append,
eviction, clear, zero-limit discard, active-grid resize, column reflow,
mode-transition, and explicit storage no-op boundaries.

## Scope

Phase 5 owns:

1. A `terminal_backing_delta_t` result vector on
   `Terminal_screen_model_result`.
2. A narrow source tag on `terminal_backing_delta_t` that distinguishes normal
   terminal-storage deltas from temporary recovery-compatibility observations.
3. Primary retained-history deltas for terminal-scroll append, limit eviction,
   ED3 clear, zero-limit discard, and scrollback-limit no-op boundaries.
4. Active-grid resize, column reflow, and alternate mode-transition deltas from
   existing operations.
5. Test helper capture of backing deltas at the primary-backing observer seam.
6. Legacy `evicted_scrollback_rows` result derivation from emitted deltas for
   migrated retained-history mutation families.
7. Recovery-disabled tests that reconcile backing deltas with the existing
   scalar result fields and retained-line identity checks.

## Non-goals

Phase 5 does not change viewport controller updates, public projection,
selection repair, synchronized-output publication, resize/reflow policy,
alternate-transition behavior, or recovery policy. The old repaint recovery
heuristic remains compatibility code and is not interpreted as evidence for
primary backing correctness.

## Phase 5 gate table

| Gate entry | Phase 5 value |
| --- | --- |
| Scope | Add a model-result backing delta vocabulary and emit retained-history, active-grid resize, column-reflow, and mode-transition deltas. |
| Behavior axis | `none` for existing consumers; new test-only observations assert the same storage mutations already represented by scalar fields. |
| Recovery state | Production recovery defaults unchanged; Phase 5 tests use recovery-disabled model helpers. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, focused build for `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, `vnm_terminal_transcript`, and `vnm_terminal_capture_replay_conformance`, focused CTest for the same four tests, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, and `vnm_terminal_transcript`. The expanded gate was rerun after active-grid resize, column-reflow, and mode-transition delta emission was added, after ingest-driven CSI text-area resize deltas were routed through `apply_grid_resize`, after legacy `evicted_scrollback_rows` result derivation was moved onto emitted deltas, and after the recovery-compatibility source tag was added; the later rerun included `vnm_terminal_viewport` and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Existing scalar consumers remain field-equivalent. `scrollback_rows` still reports the owner count, and legacy `evicted_scrollback_rows` is derived from emitted deltas for migrated retained-history mutation families. |
| Exit predicate | Append, eviction, clear, discard, active-grid resize, column reflow, mode transition, and scrollback-limit no-op boundaries have backing deltas; focused gates pass; no viewport/publication consumer has been migrated to deltas. |
| Manual gate | `none`. |
| Rollback mechanism | Remove `terminal_backing_delta_t`, result-vector plumbing, delta recording helpers, and the delta-specific test assertions. |
| Deletion gate | Later publication/viewport phases can replace remaining scalar-only wiring once delta consumers are complete. |

## Recovery boundary

Phase R supersedes the earlier temporary-compatibility framing recorded in
`recovery_baseline_correction.md`. Recovery-generated retained rows now route
through canonical backing APIs with recovered provenance. Core backing delta
tests and future delta consumers must still not treat recovery-enabled append
deltas as terminal-scroll storage evidence.
