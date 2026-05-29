# Phase 1 primary backing pre-behavior tests

This is the durable Phase 1 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 1 depends on Recovery Baseline Correction, Phase 0A, and Phase 0B. It is
test-only. It adds characterization and regression gates for retained-history
growth, no-synthesis cases, chunk-split invariance, blank-row provenance, and
small-grid behavior before any storage-owner or coordinate-domain production
refactor begins.

## Scope

Phase 1 owns the current recovery-disabled core backing tests in:

1. `tests/screen_operations/model_ops_tests.cpp`
2. `tests/backend_session/backend_session_tests.cpp`

The model tests use the Phase 0B test-only observer seam in
`tests/helpers/primary_backing_observation.h`. Backend-session tests use the
Phase 0A recovery-disabled session configuration helper through the
backend-session factory path.

## Evidence

Current Phase 1 evidence includes:

1. `test_scrollback_growth_observer_seam`
2. `test_recovery_disabled_non_scroll_sources_do_not_grow_retained_history`
3. `test_recovery_disabled_chunk_split_invariance_for_non_scroll_sources`
4. `test_recovery_disabled_scrollback_limit_changes_do_not_grow_retained_history`
5. `test_primary_crlf_blank_rows_are_retained_in_scrollback`
6. `test_primary_crlf_blank_rows_are_chunk_boundary_invariant`
7. `test_empty_backend_output_chunk_does_not_synthesize_blank_line`
8. `test_cursor_home_line_repaint_does_not_synthesize_primary_scrollback`
9. `test_cursor_home_blank_row_partial_repaint_does_not_synthesize_primary_scrollback`
10. `test_public_projection_phase1_storage_stays_viewport_bounded_after_scrollback_growth`

These tests are regression-only unless a future run records a current failure
and updates `primary_backing_failure_ledger.md`.

## Phase 1 gate table

| Gate entry | Phase 1 value |
| --- | --- |
| Scope | Focused pre-behavior storage-shape, growth-source, no-synthesis, chunk-split, blank-row, and small-grid regression tests. |
| Behavior axis | `none`. |
| Recovery state | Core tests use explicit recovery-disabled helpers; production recovery defaults remain unchanged. |
| Evidence | The named model and backend-session tests above, plus `primary_backing_failure_ledger.md`. |
| Baseline outcome | Focused gate passed on 2026-05-29: `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_transcript`. No production behavior is changed by this phase. |
| Exit predicate | The named tests pass, the ledger maps each motivating Phase 1 scenario to exact tests or a remaining future owner, and the Phase 0B guard passes. |
| Manual gate | `none`. |
| Rollback mechanism | Remove only the faulty test seam or faulty regression test. Reliable characterizations remain in the ledger. |
| Deletion gate | Phase 10 may remove or promote Phase 1-only helpers after stable storage, viewport, selection, publication, and Phase R recovery gates exist. |

## Non-changes

Phase 1 deliberately does not add production storage, typed row domains,
viewport panning, public projection behavior, selection policy, resize policy,
DSR changes, or recovery-policy restructuring.
