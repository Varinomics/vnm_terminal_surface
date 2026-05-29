# Phase 6B primary-history clear and eviction owner

This is the durable Phase 6B gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 6B is a no-behavior storage-mutator extraction. It makes retained-history
oldest-row eviction and full clear named operations on `Primary_backing_buffer`
while preserving caller behavior, scalar fields, backing deltas, hyperlink
reference accounting, viewport updates, and recovery compatibility behavior.

## Scope

Phase 6B owns:

1. `Primary_backing_buffer::take_oldest_retained_history_row`.
2. `Primary_backing_buffer::clear_retained_history`.
3. Routing `Terminal_screen_model::evict_oldest_scrollback_rows` through the
   owner take API while keeping hyperlink reference removal in the model.
4. Routing ED3 retained-history clear through the owner clear API after existing
   hyperlink reference removal.
5. The code-search exit check that direct retained-history `pop_front` and
   `clear` mutations are gone outside the owner API.

## Non-goals

Phase 6B does not change scrollback-limit policy, ED3 semantics, resize/reflow,
alternate-screen transitions, viewport synchronization, selection behavior,
public projection, transcript/replay behavior, DSR behavior, or recovery policy.

Phase R supersedes the earlier temporary-compatibility framing. This phase did
not add recovered provenance or a recovery-specific mutation path; that
ownership now lives in the Phase R artifacts.

## Phase 6B gate table

| Gate entry | Phase 6B value |
| --- | --- |
| Scope | Extract retained-history oldest-row eviction and full clear into `Primary_backing_buffer`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for retained-history `push_back`, `pop_front`, and `clear`, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, and `vnm_terminal_transcript`. |
| Baseline outcome | Existing clear, eviction, scalar, delta, hyperlink, retained-line, and viewport behavior remains field-equivalent. |
| Exit predicate | No direct retained-history `pop_front` or `clear` remains outside `Primary_backing_buffer`; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore the prior direct retained-history `pop_front` and `clear` mutations in `Terminal_screen_model`. Phase 6B is incomplete while those direct mutations remain. |
| Deletion gate | Later Phase 6 subphases move scrollback-limit, resize/reflow, and alternate mutation families behind owner APIs one family at a time. |
