# Flat Ring History Phase 5B Evidence: Retained Hyperlink Metadata Authority

## Scope

Phase 5B removes retained-history correctness dependence on scrollback-wide
hyperlink refcounts.

Implemented scope:

1. Retained render rows materialize hyperlink metadata from each retained row's
   row-local `hyperlink_identity_keys`.
2. Active and parser hyperlink state is owned by `m_active_hyperlink_ids` and
   pruned only against current active grids and the current OSC 8 hyperlink.
3. The scrollback hyperlink identity side map, scrollback hyperlink refcount
   map, append refcount update, clear refcount update, and eviction refcount
   update were deleted.
4. `Primary_backing_buffer::for_each_retained_history_row` was deleted after
   removing the last retained-history cleanup caller.

Out of scope:

1. No general storage cutover.
2. No style-policy migration.
3. No lookup cache replacement.
4. No resize projection, selection, viewport, public projection, or later
   storage policy change.

## Hyperlink Authority Policy

Retained rows are authoritative for their own hyperlink metadata. A retained
cell with a nonzero `hyperlink_id` resolves render metadata only through that
row's retained `hyperlink_identity_keys`.

Active rows use the active/parser OSC 8 map. That map is explicitly active
state: it maps live OSC 8 identity keys to cell hyperlink ids for the current
hyperlink and active grid cells. It is not retained-history authority.

No retained-history hyperlink cache was added. Eviction and clear remove
retained rows by removing row records; no pre-reclaim hyperlink cleanup is
needed for correctness.

## Evidence

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_terminal_modes vnm_terminal_render_snapshot vnm_terminal_screen_operations vnm_terminal_backend_session vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_history_row_materialization_parity --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_(terminal_modes|render_snapshot|screen_operations|backend_session)$|^vnm_terminal_history_(ring|row_record_codec|row_traversal|row_materialization_parity)$"" --output-on-failure"
```

Final gate output summary:

1. `vnm_terminal_history_ring` passed.
2. `vnm_terminal_history_row_record_codec` passed.
3. `vnm_terminal_history_row_traversal` passed.
4. `vnm_terminal_history_row_materialization_parity` passed.
5. `vnm_terminal_render_snapshot` passed.
6. `vnm_terminal_screen_operations` passed.
7. `vnm_terminal_terminal_modes` passed.
8. `vnm_terminal_backend_session` passed.
9. `100% tests passed, 0 tests failed out of 8`.

Coverage added or strengthened:

1. Retained hyperlink metadata resolves after active hyperlink state is gone.
2. Hyperlink-heavy retained output materializes row-local metadata.
3. Retained row records expose row-local hyperlink identity keys.
4. Scrollback-limit eviction leaves surviving retained hyperlinks
   self-contained.
5. Clear-history no longer needs retained hyperlink pre-reclaim cleanup for
   correctness.
6. Existing recovered-row materialization parity stayed green.

## Deletion And Orphan Audit

Deleted production state and helpers:

1. `m_scrollback_hyperlink_identity_keys`.
2. `m_scrollback_hyperlink_ref_counts`.
3. `add_scrollback_hyperlink_refs`.
4. `remove_scrollback_hyperlink_refs`.
5. `Primary_backing_buffer::for_each_retained_history_row`.

Code audit command:

```powershell
rg -n "\b(m_hyperlink_ids|m_scrollback_hyperlink_identity_keys|m_scrollback_hyperlink_ref_counts|add_scrollback_hyperlink_refs|remove_scrollback_hyperlink_refs|for_each_retained_history_row|hyperlink_metadata_for_cells|retain_referenced_hyperlink_ids|hyperlink_id_for_identity)\b" include src tests -g "*.h" -g "*.cpp"
```

Audit result:

1. No deleted hyperlink map/refcount/helper references remain in `include`,
   `src`, or `tests`; the exact-symbol audit returned no matches.
2. A broader planning-doc search before this artifact was added had one
   historical Phase 6D hit that recorded
   `Primary_backing_buffer::for_each_retained_history_row` as present during
   that earlier phase.
3. Active/parser ownership names remain:
   `m_active_hyperlink_ids`, `active_hyperlink_id_for_identity`,
   `active_hyperlink_identity_key`, and
   `retain_referenced_active_hyperlink_ids`.

No Phase 5B production scaffold was added using `_v2`, `_legacy`, fallback,
storage mirror, dual-write, or cutover behavior.

## Phase Gate Table

| Gate | Phase 5B result |
| --- | --- |
| Scope | Retained rows now materialize render hyperlink metadata from row-local retained data; remaining hyperlink map ownership is active/parser state. |
| Behavior axis | User-visible hyperlink rendering is preserved for active rows, retained rows, hyperlink-heavy scrollback, and retained rows after active hyperlink state is gone. |
| Recovery state | Recovery acceptance policy is unchanged. Recovered rows still enter retained history through the existing producer, and Phase 4D recovered hyperlink parity remains green. |
| Evidence | Focused OSC8, render snapshot, screen operation, backend session, ring, codec, traversal, and materialization parity gates passed through `vcvarsall` x64. |
| Baseline outcome | Retained rows no longer need scrollback-wide hyperlink refcounts or identity side maps to render hyperlink metadata. |
| Exit predicate | A retained row can render its hyperlink metadata from its own retained record after active hyperlink state is gone, and eviction/clear requires no retained hyperlink cleanup for correctness. |
| Deletion ownership | Phase 5B owns deletion of the scrollback hyperlink refcount/identity side maps, their append/clear/evict cleanup helpers, and the now-orphaned retained-history iteration helper. |
| Rollback mechanism | Revert the Phase 5B code and test changes, remove this evidence artifact and README entry, and restore the previous scrollback hyperlink aggregation only if row-local retained authority fails the gate. |
| Split triggers | Stop and split if active-grid hyperlink lifetime requires different cleanup semantics, if a rebuildable retained hyperlink cache is introduced, or if storage cutover, lookup replacement, resize projection, selection, viewport, or public projection policy must change. |
