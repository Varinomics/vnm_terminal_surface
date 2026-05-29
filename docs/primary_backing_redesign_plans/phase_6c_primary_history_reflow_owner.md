# Phase 6C primary-history reflow owner

This is the durable Phase 6C gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 6C is a no-behavior storage-mutator extraction. It removes the mutable
model-level retained-history accessor and routes retained-history resize/reflow
iteration through `Primary_backing_buffer` while preserving model-owned
hyperlink reference accounting, wide-cell repair, content-generation updates,
backing deltas, viewport updates, and recovery compatibility behavior.

## Scope

Phase 6C owns:

1. `Primary_backing_buffer::mutate_retained_history_rows`.
2. Removal of the mutable `Terminal_screen_model::primary_scrollback_rows`
   accessor.
3. Routing `Terminal_screen_model::resize_scrollback_rows` through the owner
   mutation API.
4. The code-search exit check that no mutable `primary_scrollback_rows()`
   accessor remains.

## Non-goals

Phase 6C does not change resize/reflow policy, retained-row repair semantics,
hyperlink accounting, active-grid resize behavior, viewport synchronization,
selection behavior, public projection, transcript/replay behavior, DSR behavior,
or recovery policy.

Phase R supersedes the earlier temporary-compatibility framing. This phase did
not add recovered provenance or a recovery-specific resize/reflow path; that
ownership now lives in the Phase R artifacts.

## Phase 6C gate table

| Gate entry | Phase 6C value |
| --- | --- |
| Scope | Route retained-history resize/reflow mutation through `Primary_backing_buffer`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for the mutable `primary_scrollback_rows()` accessor, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, and `vnm_terminal_transcript`. |
| Baseline outcome | Existing resize/reflow, hyperlink, scalar, delta, retained-line, and viewport behavior remains field-equivalent. |
| Exit predicate | No mutable `primary_scrollback_rows()` accessor remains; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore the mutable `primary_scrollback_rows()` accessor and direct iteration in `resize_scrollback_rows`. Phase 6C is incomplete while that mutable accessor remains. |
| Deletion gate | Later Phase 6 subphases can move more retained-history repair details into the owner only with separately named tests and gates. |
