# Phase 6A primary-history append owner

This is the durable Phase 6A gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 6A is a no-behavior storage-mutator extraction. It makes retained-history
append a named operation on `Primary_backing_buffer` while preserving every
caller, scalar result field, backing delta, retained-line identity, hyperlink
reference, viewport update, and recovery compatibility behavior.

## Scope

Phase 6A owns:

1. `Primary_backing_buffer::append_retained_history_row`.
2. Routing `Terminal_screen_model::append_scrollback_row` through that owner API
   after the row has been converted to `scrollback_row_t` and hyperlink refs
   have been added.
3. The code-search exit check that direct retained-history `push_back` is gone
   outside the owner API.

## Non-goals

Phase 6A does not change scroll-region behavior, CRLF/newline behavior,
explicit clear, erase, eviction, scrollback-limit enforcement, resize/reflow,
alternate-screen transitions, viewport synchronization, selection behavior,
public projection, transcript/replay behavior, DSR behavior, or recovery
policy.

Phase R supersedes the earlier temporary-compatibility framing. Accepted
recovery rows now enter retained history through the same append owner path with
recovered provenance.

## Phase 6A gate table

| Gate entry | Phase 6A value |
| --- | --- |
| Scope | Extract retained-history append into `Primary_backing_buffer`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; no recovery-policy API or recovered provenance is introduced. |
| Evidence | Passed on 2026-05-29: Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, code-search exit check for retained-history `push_back`, expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, and `vnm_terminal_transcript`. |
| Baseline outcome | Existing append, scalar, delta, retained-line, and viewport behavior remains field-equivalent. |
| Exit predicate | No direct retained-history `push_back` remains outside `Primary_backing_buffer::append_retained_history_row`; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore the prior direct retained-history append in `Terminal_screen_model::append_scrollback_row`. Phase 6A is incomplete while that direct append remains. |
| Deletion gate | Later Phase 6 subphases move clear, eviction, scrollback-limit, resize/reflow, and alternate mutation families behind owner APIs one family at a time. |
