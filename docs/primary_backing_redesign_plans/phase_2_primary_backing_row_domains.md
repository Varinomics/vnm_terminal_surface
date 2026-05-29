# Phase 2 primary backing row domains

This is the durable Phase 2 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 2 is a no-behavior production refactor. It introduces explicit row-domain
types and conversion helpers inside `Terminal_screen_model` so later read and
write phases cannot silently mix active-grid rows, primary-backing rows,
viewport rows, and snapshot rows.

## Scope

Phase 2 owns only:

1. Private lightweight row-domain wrappers in `Terminal_screen_model`.
2. Private conversion helpers between active-grid, primary-backing, viewport,
   and snapshot row domains.
3. Routing existing read-only arithmetic through those helpers where the
   behavior is field-equivalent.
4. Explicit primary and alternate active-grid row access helpers over the
   current storage layout.

## Non-goals

Phase 2 does not introduce a storage owner, a second row producer, viewport
panning, a nonzero viewport origin, write-path migration, selection policy
changes, public projection behavior, resize policy changes, or recovery-policy
restructuring.

## Phase 2 gate table

| Gate entry | Phase 2 value |
| --- | --- |
| Scope | Private row-domain wrappers and conversion helper routing in `Terminal_screen_model`. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; core backing tests remain recovery-disabled through Phase 0A helpers. |
| Evidence | Existing Phase 1 model/backend/transcript gates plus field-equivalent render, selection, retained-line lookup, and viewport tests. |
| Baseline outcome | Focused gate passed on 2026-05-29: static guard, `git diff --check`, build, and `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_transcript`. No public API or runtime behavior changed. |
| Exit predicate | Duplicated visible/backing arithmetic touched by this phase routes through named helpers, alternate reads stay separate, and focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Revert the named helper routing and private wrapper declarations. |
| Deletion gate | Phase 10 keeps stable typed conversion boundaries and removes only temporary compatibility helpers that become obsolete. |

## Recovery boundary

Phase R supersedes the earlier temporary-compatibility framing described by
`recovery_baseline_correction.md`. Phase 2 helpers must not call recovery code,
inspect repaint evidence, or treat recovered rows as proof of storage
correctness.
