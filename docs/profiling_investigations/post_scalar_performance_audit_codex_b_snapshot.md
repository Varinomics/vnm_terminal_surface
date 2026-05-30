# Focus B: render snapshot construction and publication audit

## Executive summary

The scalar-span profile shows that render snapshot construction is dominated by full visible-row materialization, not dirty-row bookkeeping. `Terminal_screen_model::render_snapshot::append_rows` accounts for 10.596 s of the 10.635 s spent inside `Terminal_screen_model::render_snapshot`, and it visits every visible row for every published content snapshot.

For the profiled 235 x 873 surface, each snapshot scans 205,155 grid cells even when the average visible dirty set is only about 9.6 rows. Across 1,498 snapshots, that is 352,030 visible rows and 307,322,190 cells scanned to emit 134,005,265 occupied snapshot cells.

Dirty rows are already effective as renderer invalidation metadata. They are not effective as snapshot construction limits because `render_snapshot` computes dirty ranges first, then still materializes the full visible grid. Session and surface coalescing only merge dirty ranges after a full snapshot has been constructed.

The lowest-risk improvements are instrumentation and publication-cadence reductions: skip provably redundant publications, tighten backend-output coalescing before model ingest/publish, and avoid extra handle churn when a snapshot generation has not changed meaningfully. The bigger wins require changing the snapshot contract so renderer-visible immutable content can be row-addressable, shared, or split from view state instead of copied into a full `Terminal_render_snapshot::cells` vector for each publication.

## Current snapshot contract

`Terminal_render_snapshot` is the single public renderer contract. The architecture document states that immutable snapshots flow through render frames into QSG, and that internal packing/cache choices do not create alternate renderer APIs. The snapshot owns grid size, viewport, color state, styles, positioned cells, visible-line provenance, dirty ranges, hyperlinks, cursor, IME preedit, selection spans, metadata, and modes.

The concrete structure reflects that contract:

- `Terminal_render_snapshot::cells` is a flat vector of positioned `Terminal_render_cell` values.
- `Terminal_render_snapshot::visible_line_provenance` is expected to contain one entry per visible row when provenance-dependent features are valid.
- `Terminal_render_snapshot::dirty_row_ranges` is metadata inside the same object, not a replacement for the full cell payload.
- `validate_render_snapshot` treats visible-line provenance as all-visible-row state and validates dirty ranges only after the complete snapshot object exists.

`Terminal_screen_model::render_snapshot` constructs that object in this order:

- Normalize/correct viewport state.
- Create an empty snapshot and copy metadata, styles, color state, cursor state, IME, and modes.
- Convert requested dirty rows into compact dirty row ranges.
- Run `append_rows`, which loops over every visible row, obtains row cells, appends all occupied cells for that row, appends hyperlink metadata for newly emitted cells, and appends line provenance.
- Adjust primary cursor visibility.
- Materialize selection spans if line provenance is valid.

The important audit point is that dirty rows are computed before `append_rows`, but `append_rows` does not consult them to restrict row materialization.

## Measured costs

Profile: `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`.

Surface geometry:

- Rows: 235.
- Columns: 873.
- Visible grid cells per snapshot: 205,155.

Snapshot/session counters:

- `render_snapshot_requests=1498`.
- `render_snapshots_constructed=1498`.
- `render_snapshot_publications=1498`.
- `content_snapshot_publications=1498`.
- `selection_snapshot_publications=0`.
- `geometry_snapshot_publications=0`.
- `public_projection_scroll_publications=0`.
- `dirty_coalescing_attempts=45` and `dirty_coalescing_applied=45`.
- `snapshots_superseded_before_render=112`.
- `snapshots_marked_rendered=1453`.
- `max_unrendered_snapshot_generations=11`.

Model snapshot work:

- `render_snapshot_rows_visited=352030`, exactly `1498 * 235`.
- `render_snapshot_rows_materialized=352030`, also every visited row.
- `render_snapshot_cells_scanned=307322190`, exactly `1498 * 235 * 873`.
- `render_snapshot_cells_emitted=134005265`, about 89,456 emitted cells per snapshot on average.
- `render_snapshot_dirty_rows_requested=14379`.
- `render_snapshot_dirty_rows_visible=14379`, about 9.6 visible dirty rows per snapshot on average.
- `render_snapshot_full_repaint_fallbacks=0`.
- `render_snapshot_viewport_fallbacks=0`.
- `render_snapshot_zero_dirty_publications=0`.
- `max_render_snapshot_rows_visited=235`.
- `max_render_snapshot_cells_emitted=103299`.

Hierarchical profiler aggregate:

- `Terminal_session::process_backend_output_command`: 13.389 s total, 1,500 calls, 8.926 ms mean.
- `Terminal_session::publish_backend_render_snapshot`: 11.428 s total, 1,498 calls, 7.629 ms mean.
- `Terminal_session::publish_render_snapshot`: 11.421 s total, 1,498 calls, 7.624 ms mean.
- `Terminal_screen_model::render_snapshot`: 10.635 s total, 1,498 calls, 7.099 ms mean.
- `Terminal_screen_model::render_snapshot::append_rows`: 10.596 s total, 1,498 calls, 7.074 ms mean.
- `Terminal_screen_model::dirty_rows`: 0.529 ms total, 1,500 calls, 0.352 us mean.
- `Terminal_screen_model::publish_pending_changes`: about 20.1 ms total across 174,953 calls.

Dirty-row profile:

- `mark_requests=4380137`.
- `duplicate_mark_requests=4258550`.
- `unique_pending_row_marks=121587`.
- `mark_all_dirty_calls=16`.
- `dirty_rows_snapshot_calls=1500`.
- `dirty_rows_snapshot_rows=14379`.
- `publish_pending_calls=174953`.
- `published_unique_rows=14379`.
- `max_pending_dirty_rows=235`.

Renderer-side costs related to full snapshot shape:

- Render-thread `build_terminal_render_frame`: 26.128 s total over 662 calls, 39.468 ms mean.
- `build_terminal_render_frame::cells`: 13.281 s total.
- `build_terminal_render_frame::packed_data`: 12.751 s total.
- Last renderer stats show `frame_cell_pass_input_cells=102975`, `frame_packed_pass_input_cells=102975`, and `frame_dirty_row_lookup_count=205950`, so the frame builder made two full passes over emitted snapshot cells even though `frame_dirty_rows=75`.

## Full-grid work sources

1. `Terminal_screen_model::render_snapshot::append_rows` loops from row 0 to `m_config.grid_size.rows - 1` for every snapshot. It calls `viewport_row_cells`, appends all occupied cells in the row, scans the newly appended row cells for hyperlink IDs, and appends line provenance.

2. `viewport_row_cells` returns a copied `std::vector<Cell>` for both alternate rows and primary backing rows. Primary scrollback rows can require retained-history materialization before the copy. This means snapshot construction pays row-copy/materialization overhead before scanning columns.

3. `append_snapshot_cells_from_row` then scans every column in the current geometry, skipping only unoccupied cells. The profile counter `render_snapshot_cells_scanned=307322190` exactly matches the full visible grid size over 1,498 snapshots.

4. Dirty rows are carried as `Terminal_render_snapshot_request::dirty_rows`, compacted with `compact_dirty_row_ranges`, and stored in `snapshot.dirty_row_ranges`. They do not prune the `append_rows` loop.

5. Session dirty coalescing happens after `m_screen_model->render_snapshot(request)`. `snapshot_with_coalesced_dirty_rows` merges the previous and current dirty ranges, or forces a full dirty range if row identity changed. It does not reduce model construction work.

6. Surface-side dirty coalescing in `VNM_TerminalSurface_render_bridge::set_render_snapshot` similarly copies the current snapshot and merges dirty ranges when an update is already pending. This preserves render invalidation but happens after the full snapshot handle already exists.

7. Renderer frame construction builds row tables and packed data from the full `snapshot.cells` vector. Dirty rows are consulted per cell for classification/packing, but the frame builder still scans the full emitted cell vector in both the cells pass and packed-data pass.

8. Public projection full-row capture can amplify the same pattern. `capture_primary_full_rows_from_safe_model` calls `safe_model.render_snapshot` repeatedly over offset-from-tail windows to copy primary projection rows, so it currently reuses the full snapshot materialization path for each capture window.

## Low-risk options

1. Add explicit counters for avoidable full-grid work.

   Track `render_snapshot_clean_rows_materialized`, `render_snapshot_dirty_rows_materialized`, `render_snapshot_non_dirty_cells_emitted`, and `render_snapshot_append_rows_retained_history_rows`. This does not change behavior, but it makes future diffs safer by distinguishing dirty-row savings from full-grid materialization that remains.

2. Skip no-op content publications before snapshot construction.

   `make_render_snapshot_request` already centralizes dirty rows, viewport changes, metadata, and purpose. A conservative preflight can reject content publication when dirty rows are empty, viewport is unchanged, visual bell is inactive, no relevant metadata changed, and no selection/IME/cursor publication reason exists. The profile has no zero-dirty publications in this run, so this is not the main win here, but it is low risk and prevents regression.

3. Coalesce backend output earlier in the owner-thread path.

   Callback-lifetime coalescing merges adjacent pending backend-output callbacks before session processing, but the profile still has 1,500 backend output commands and 1,498 snapshots. If multiple output commands are already queued when `process_pending_commands` drains, process adjacent backend-output commands as one ingest segment where ordering barriers allow it. This reduces snapshot cadence without changing the snapshot contract.

4. Add a bounded publication throttle for backend output only.

   When the GUI has unrendered snapshot generations, allow model ingest to continue but defer content snapshot construction until either a time budget expires, a non-coalescible command appears, synchronized-output boundaries require publication, or input/selection needs a coherent published basis. The existing `m_render_snapshot_synced_generation`, `snapshots_superseded_before_render`, and surface generation sync already provide most of the observability.

5. Avoid surface-side snapshot copies for dirty-range coalescing when possible.

   `set_render_snapshot` copies the entire current snapshot to merge dirty ranges if a render update is pending. A small wrapper around a snapshot handle plus extra pending dirty ranges would avoid copying cell vectors in the GUI handoff path. This is low-to-medium risk because it keeps the public `Terminal_render_snapshot` stable if the wrapper is private to the bridge.

6. Precompute row dirty membership once per snapshot/frame.

   Both model and renderer repeatedly derive dirty membership from range vectors. This is minor compared with full cell scans, but a row bitset or compact row mask can remove per-cell dirty-range walks in frame construction without changing semantics.

7. Reserve based on previous emitted cell count instead of full grid size.

   `render_snapshot` reserves `rows * columns` cells even though emitted cells averaged about 89k and maxed at 103,299 in this profile. Reusing a high-water estimate or reserving a bounded previous count could reduce allocation pressure. This is low reward unless allocator churn appears in heap profiles, but it is behaviorally safe.

## Architectural options

1. Row-store-backed full snapshots.

   Keep `Terminal_render_snapshot` as the renderer-facing object initially, but replace the full cell vector construction with immutable per-row payloads owned by a row store. A snapshot would carry row references plus dirty ranges; only dirty rows need fresh row materialization. Renderer code can still build a full frame when necessary, but clean rows can reuse row-local descriptors and packed resources by identity.

2. Delta snapshots over a retained base.

   Publish an initial full content snapshot and then publish deltas containing changed row payloads, viewport changes, cursor/IME/selection changes, and metadata. The surface or renderer owns the latest materialized view. This targets the exact profile mismatch: 9.6 dirty rows per snapshot versus 235 visible rows currently materialized.

3. Split content snapshots from view snapshots.

   Publish content row changes separately from viewport/cursor/selection state. This aligns with the existing immediate public scroll architecture option: synchronized output blocks content publication, not view changes. It also avoids treating viewport updates as reasons to rebuild all content rows. This is invasive because many consumers currently expect one `Terminal_render_snapshot`.

4. Renderer-owned content cache with snapshot row identities.

   Preserve immutable snapshot handles but add stable row identity/generation metadata so the renderer can skip rebuilding row tables, text resources, and packed data for unchanged rows. This reduces render-thread work even if model snapshots remain full for a while.

5. Dirty-row-limited model snapshot plus base snapshot handle.

   Add a snapshot form that includes a base snapshot handle and materialized dirty rows only. Selection and text extraction either resolve through the base plus overrides or force a full materialization path only for APIs that need it. This directly reduces model-side work, but it needs careful lifetime and validation rules.

6. Public projection from immutable retained rows, not repeated render snapshots.

   `capture_primary_full_rows_from_safe_model` currently walks scrollback by repeatedly calling `safe_model.render_snapshot`. A projection-specific row copier that reads retained row records directly would avoid full visible-window snapshots during projection capture and avoid constructing throwaway `Terminal_render_snapshot` objects.

7. Packed snapshot contract.

   Change the renderer contract from positioned cells to row-local packed text/graphic spans. This removes one conversion layer from `build_terminal_render_frame`, but it is the most invasive option because selection, validation, transcript, and tests currently reason about cells.

## Correctness risks

1. Dirty rows are not enough to reconstruct visible state.

   Clean visible rows are still needed for rendering, selection, transcript snapshots, validation, cursor interactions, IME overlay, hyperlinks, and public projection. Any dirty-row-limited snapshot must retain a base identity or row store that can answer clean-row queries.

2. Row identity changes require full dirty fallback.

   Existing coalescing forces full dirty ranges when snapshots do not share row identity space. Any partial/delta scheme must preserve this behavior for viewport jumps, scrollback eviction, alternate-screen transitions, reflow, row-origin changes, and public-projection release.

3. Selection depends on full visible-line provenance.

   `render_snapshot_visible_line_provenance_is_valid` requires one provenance entry per visible row; selection spans are suppressed without valid provenance. Partial snapshots must either carry complete provenance or define a base-plus-delta validation rule.

4. Hyperlink metadata is globally snapshot-scoped.

   `append_hyperlink_metadata_for_cells` materializes hyperlink metadata for emitted cells. A row-delta snapshot must ensure hyperlink metadata for clean base rows remains visible, while metadata for changed rows is updated or removed without stale references.

5. Public projection must not read hidden live rows.

   Existing architecture requires public-projection snapshots to source every user-visible and validator-visible field from public data. Optimizing projection capture must not accidentally reconstruct projection rows from live synchronized-output state after the safe boundary.

6. Publication cadence is an ordering boundary.

   Coalescing backend output before publication must not cross DEC synchronized-output entry/release, resize, backend error/exit, terminal replies, input writes that depend on published mouse/reporting state, forced release, or transcript ordering boundaries.

7. Surface generation sync drives downstream coalescing.

   `mark_render_snapshot_synced` advances `m_render_snapshot_synced_generation`, and session dirty coalescing uses unsynced generations to merge dirty ranges. Publication throttling must preserve accurate generation accounting or dirty rows can be lost.

8. Renderer caches assume row-local invalidation but consume full cells.

   Moving to row stores or deltas requires cache invalidation keyed by row identity, viewport row, style table identity, font/metrics, cursor/IME overlays, selection spans, and dirty ranges. A row that is content-clean can still be visually dirty because cursor, IME, selection, or metadata overlays changed.

## Tests needed

1. Snapshot construction with one dirty row on a large grid should not materialize every visible row once a dirty-row-limited contract is introduced.

2. Full repaint fallback should still occur when viewport identity, row origin, active buffer, grid size, or visible-line provenance identity changes.

3. Backend output bursts queued before owner-thread processing should publish one coalesced content snapshot when no ordering barriers are present.

4. Backend output coalescing must split at DECSET 2026, DECRST 2026, forced synchronized-output release, resize, backend exit/error, terminal replies, and user-input barriers.

5. Selection spans and selected text should remain correct across dirty-row-only updates, viewport scroll, scrollback eviction, and retained-row identity reuse.

6. Hyperlink metadata should be added, updated, and removed correctly when only dirty rows are republished.

7. Public projection capture should not call live-model row reads after synchronized-output entry and should still expose copied public scrollback rows.

8. Surface-side pending-update coalescing should preserve dirty ranges and generation sync when multiple snapshots arrive before `updatePaintNode`.

9. Renderer frame building should reuse clean rows and rebuild only dirty rows under cursor, IME, selection, and text-style changes.

10. Transcript capture/replay should distinguish full content snapshots, deltas, selection-derived snapshots, geometry-derived snapshots, and public-projection scroll snapshots without inferring semantics from reason strings.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
- `C:\plms\varinomics\vnm_terminal_surface\docs\architecture.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\immediate_public_scroll_architecture.md`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_public_projection.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_session.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_public_projection.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
