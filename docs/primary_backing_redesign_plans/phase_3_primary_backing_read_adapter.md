# Phase 3 primary backing read adapter

This is the durable Phase 3 gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 3 is a no-behavior production refactor. It makes primary logical row
reads go through one read-only adapter over the current storage layout: retained
primary history followed by the active primary grid tail. It does not introduce
new storage or migrate write ownership.

## Scope

Phase 3 owns:

1. The private `primary_backing_row` accessor over existing `m_scrollback` plus
   the current or saved primary active grid rows.
2. Read-only routing for render snapshot rows, retained-line lookup,
   retained-line provenance testing, selection payload row lookup, and
   selection visual row mapping.
3. Separate alternate active-grid reads through `alternate_active_row`.
4. Bounded and unbounded backing-to-viewport conversions, where snapshots need
   bounded visible rows but cursor metadata preserves an out-of-view coordinate
   while marking visibility false.

## Non-goals

Phase 3 does not add a second storage owner, a production mirror, write-path
migration, selection persistence or invalidation redesign, synchronized-output
release changes, public projection behavior changes, or recovery-policy work.

## Phase 3 gate table

| Gate entry | Phase 3 value |
| --- | --- |
| Scope | Read-only primary backing adapter and routing for existing snapshot, retained-line, selection, and test-provenance reads. |
| Behavior axis | `none`. |
| Recovery state | Production recovery defaults unchanged; core backing tests remain recovery-disabled through Phase 0A helpers. |
| Evidence | Focused gate passed on 2026-05-29: static guard, `git diff --check`, build, and `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_transcript`. |
| Baseline outcome | Field-equivalent behavior. A bounded cursor-row conversion was corrected during validation so transcript snapshots keep the prior nonnegative out-of-view cursor coordinate contract. |
| Exit predicate | Primary render snapshot reads, retained-line lookup, selected-text row lookup, selection visual mapping, and test provenance use the shared read adapter; alternate reads stay separate; focused gates pass. |
| Manual gate | `none`. |
| Rollback mechanism | Restore the named read path to direct current-storage lookup. The phase is incomplete while any restored direct primary read remains active. |
| Deletion gate | Phase 10 keeps the stable read adapter or promotes it into the final storage owner API. |

## Recovery boundary

Phase 3 does not inspect repaint evidence or accept recovered rows. The
recovery ownership is superseded by the Phase R artifacts.
