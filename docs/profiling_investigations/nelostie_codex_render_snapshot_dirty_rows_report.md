# Nelostie render snapshot and dirty-row profiling investigation

## Scope

This investigation covers the stress-demo profile at
`C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`, with focus on render snapshot publication and dirty-row behavior in `vnm_terminal_surface`.

The workload is treated as intentional: a large grid and many dirty rows are the target case, not a reason to reject the profile.

## Executive summary

The main render-publication choke point is full visible-row snapshot construction. In the captured profile, `Terminal_session::publish_render_snapshot` accounts for 8.487 s over 1,252 calls, and `Terminal_screen_model::render_snapshot::append_rows` accounts for 7.797 s of that path. Dirty-row computation itself is not expensive: `Terminal_screen_model::render_snapshot::dirty_rows` accounts for 3.39 ms total over the same 1,252 calls.

The likely scaling issue is that dirty rows are used to describe what the renderer should update, but `Terminal_screen_model::render_snapshot` still serializes every visible row and scans every column on each snapshot. With the captured geometry of 233 rows by 871 columns, each snapshot can scan about 202,943 cell positions before the renderer gets a chance to use dirty ranges. Across 1,252 render snapshots, that is about 254 million row/column cell visits in `append_rows`.

Publication frequency also matters. The profile shows 1,252 backend render snapshot publications and hot timeline buckets with multiple full snapshot builds inside a 100 ms bucket. Surface-side dirty range coalescing exists, but it happens after snapshots have already been constructed, so it cannot reduce `append_rows` cost.

## Direct profile evidence

### Captured geometry

From `surface_geometry`:

| Metric | Value |
| --- | ---: |
| Rows | 233 |
| Columns | 871 |
| Cell positions per full visible grid | 202,943 |
| Surface size | 3051.4 x 1630.4 |
| Window size | 3065 x 1664 |
| Device pixel ratio | 1.25 |

### Dirty-row counters

From `dirty_rows`:

| Counter | Value |
| --- | ---: |
| `enabled` | true |
| `mark_requests` | 7,875,373 |
| `duplicate_mark_requests` | 7,224,579 |
| `out_of_bounds_mark_requests` | 0 |
| `unique_pending_row_marks` | 650,794 |
| `mark_all_dirty_calls` | 41 |
| `dirty_rows_snapshot_calls` | 1,298 |
| `dirty_rows_snapshot_rows` | 55,889 |
| `collect_synchronized_calls` | 0 |
| `publish_pending_calls` | 991,700 |
| `published_unique_rows` | 53,632 |
| `release_synchronized_calls` | 0 |
| `max_pending_dirty_rows` | 272 |
| `max_synchronized_dirty_rows` | 0 |

Interpretation from the counters:

- 91.7% of dirty-row mark requests were duplicates: 7,224,579 / 7,875,373.
- Dirty-row snapshots contained 55,889 rows over 1,298 calls, or about 43.1 rows per snapshot on average.
- Average dirty-row payload was much smaller than the full 233-row final geometry, but the snapshot builder still paid full-row construction cost.
- `publish_pending_calls` was extremely frequent at 991,700 calls, but the profiled time for this function was only 106.8 ms total, so it is not the primary time sink in this capture.

### Aggregate profiling scopes

Relevant aggregate scopes:

| Scope | Calls | Total | Mean | Self | Child | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `Terminal_session::publish_backend_render_snapshot` | 1,252 | 8.494 s | 6.785 ms | 7.19 ms | 8.487 s | 15.576 ms |
| `Terminal_session::publish_render_snapshot` | 1,252 | 8.487 s | 6.779 ms | 656.1 ms | 7.831 s | 15.569 ms |
| `Terminal_screen_model::render_snapshot` | 1,252 | 7.831 s | 6.255 ms | 8.32 ms | 7.823 s | 13.572 ms |
| `Terminal_screen_model::render_snapshot::append_rows` | 1,252 | 7.797 s | 6.228 ms | 7.797 s | 0 | 13.540 ms |
| `Terminal_screen_model::render_snapshot::dirty_rows` | 1,252 | 3.39 ms | 2.71 us | 3.39 ms | 0 | 45.7 us |
| `Terminal_screen_model::dirty_rows` | 1,285 | 1.57 ms | 1.22 us | 1.57 ms | 0 | 19.6 us |
| `Terminal_screen_model::publish_pending_changes` | 991,674 | 106.8 ms | 107 ns | 106.8 ms | 0 | 71.8 us |

`append_rows` is 99.6% of `Terminal_screen_model::render_snapshot` time and 91.9% of `Terminal_session::publish_render_snapshot` time in this profile.

### Timeline evidence

The dirty-row timeline has 100 ms buckets and 1,071 buckets. Selected hot buckets around 46.5 s show a high mark rate with multiple snapshot builds:

| Bucket | Dirty mark requests | Duplicate marks | Unique pending marks | Dirty snapshot calls | Dirty snapshot rows | Publish pending calls | Published unique rows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 46.5-46.6 s | 406 | 240 | 166 | 2 | 25 | 265 | 25 |
| 46.6-46.7 s | 6,327 | 3,901 | 2,426 | 2 | 181 | 4,102 | 181 |
| 46.7-46.8 s | 5,536 | 3,426 | 2,110 | 3 | 148 | 3,604 | 148 |

The scope timeline in the same region contains repeated `append_rows` and `publish_render_snapshot` samples around 6-8 ms per snapshot. Examples from the profile include:

| Scope sample | Calls in bucket | Total | Mean | Min | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Terminal_screen_model::render_snapshot::append_rows` | 5 | 32.776 ms | 6.555 ms | 5.639 ms | 8.163 ms |
| `Terminal_session::publish_render_snapshot` | 5 | 35.256 ms | 7.051 ms | 6.218 ms | 8.184 ms |
| `Terminal_screen_model::render_snapshot::append_rows` | 2 | 13.561 ms | 6.780 ms | 6.320 ms | 7.241 ms |
| `Terminal_session::publish_render_snapshot` | 2 | 14.659 ms | 7.329 ms | 6.917 ms | 7.741 ms |
| `Terminal_screen_model::render_snapshot::append_rows` | 5 | 36.178 ms | 7.236 ms | 6.354 ms | 8.323 ms |
| `Terminal_session::publish_render_snapshot` | 5 | 39.059 ms | 7.812 ms | 6.873 ms | 8.861 ms |

This supports the conclusion that publication cost can consume a substantial fraction of a 100 ms bucket when several snapshots are published close together.

## Source evidence

### Snapshot construction always appends all visible rows

In `Terminal_screen_model::render_snapshot`, dirty ranges are computed before row appending:

- `snapshot.dirty_row_ranges = compact_dirty_row_ranges(viewport_dirty_rows(...), ..., request.viewport_changed)`.
- Then `append_rows` reserves full-grid capacity and loops from row 0 to `m_config.grid_size.rows - 1`.
- For each row it calls `viewport_row_cells`, appends cells for the whole row, appends hyperlink metadata, and records visible-line provenance.

The dirty-row list does not bound the `append_rows` loop. Dirty rows are metadata on the completed full snapshot, not a filter for snapshot materialization.

### Row appending scans every column for materialized rows

`append_snapshot_cells_from_row` asks for row cells in current geometry, then loops from column 0 to `m_config.grid_size.columns - 1`. It skips unoccupied cells, but the scan still visits every column in every materialized visible row.

For the captured 233 x 871 grid, that is up to 202,943 cell-position checks per snapshot. This matches the profile shape: `append_rows` dominates, while dirty-row compaction is effectively invisible.

### Dirty rows are stored as sets and published very frequently

The model stores pending dirty rows in `std::set<int> m_dirty_rows`, with a one-row fast duplicate guard through `m_last_dirty_row`. `publish_pending_changes` inserts pending dirty rows into an `ingest_publication_t` set and clears the model dirty state. `dirty_rows()` later copies the set into a vector.

The profile shows this machinery is called frequently, but it is not currently the large time sink:

- `publish_pending_changes`: 991,674 calls, 106.8 ms total.
- `dirty_rows`: 1,285 calls, 1.57 ms total.
- `render_snapshot::dirty_rows`: 1,252 calls, 3.39 ms total.

This makes dirty-row data structure optimization secondary for the captured profile, although the call counts are high enough that it remains worth considering after the row-snapshot bottleneck.

### Coalescing happens after full snapshots are built

There are two relevant coalescing paths:

- `Terminal_session::publish_render_snapshot` may coalesce dirty rows with the previous session snapshot after calling `m_screen_model->render_snapshot(request)`.
- `VNM_TerminalSurface_render_bridge::set_render_snapshot` may coalesce dirty rows when a render update is already pending.

Both paths preserve dirty range correctness for the renderer, but both happen after a full `Terminal_render_snapshot` object has already been constructed. They cannot reduce `Terminal_screen_model::render_snapshot::append_rows` time.

### Renderer consumes dirty ranges later

`qsg_terminal_renderer.cpp` carries `snapshot->dirty_row_ranges` into the render frame and has helper checks such as `snapshot_row_is_dirty` and `row_is_dirty`. This supports the model that dirty ranges are useful downstream, but they do not currently reduce the upstream cost of snapshot publication.

## Source-based inference

### Inference: the current snapshot contract is full-content oriented

`Terminal_render_snapshot` contains sparse occupied cells, visible-line provenance, styles, hyperlinks, cursor, modes, dirty ranges, and selection spans. Selection extraction helpers treat missing cells as spaces. The existing contract reads as a complete visible content snapshot plus dirty metadata.

Because of that contract, simply omitting unchanged rows from `snapshot.cells` would be unsafe unless the renderer and any other consumers know how to combine a delta snapshot with a previous full snapshot. Missing cells currently mean blank cells, not unchanged cells.

### Inference: dirty rows currently optimize rendering more than publication

The profile and source together indicate that dirty rows can reduce renderer-side work, but not snapshot construction. Under the stress workload, the expensive part is before renderer dirty-row filtering has any leverage.

### Inference: publication frequency amplifies the full-snapshot cost

The average `publish_render_snapshot` cost is 6.78 ms. At 1,252 calls, even moderate publication frequency accumulates into seconds of CPU time. Hot buckets with 5 publications in 100 ms spend roughly 30-40 ms of that bucket in snapshot publication alone.

## Actionable improvement options

### 1. Add a row-cache or delta-aware snapshot publication path

Proposal:

Introduce an internal publication path that reuses previously materialized visible rows when the viewport row identity space is stable. On each model result, materialize only dirty viewport rows, update cached row cells/provenance/hyperlink metadata, and publish a snapshot or render-frame input assembled from cached rows plus updated dirty ranges.

Expected benefit:

High. Average dirty-row snapshot payload was about 43 rows against a 233-row grid, so a stable-viewport row cache could avoid materializing most visible rows for many publications. The theoretical upper bound from average dirty rows is roughly an 80% reduction in row materialization work, though real benefit will be lower because viewport changes, full repaints, selections, cursor updates, and hyperlink metadata still need handling.

Risk:

High. This touches snapshot semantics, row identity, retained history materialization, hyperlink metadata, selection spans, cursor visibility, and public projection interactions. Incorrect reuse can produce stale text, stale hyperlinks, wrong selection text, or invalid provenance.

Suggested validation:

- Add an A/B snapshot-equivalence harness that compares the existing full snapshot with the cached/delta path across transcript replay fixtures.
- Include cases with scrollback, alternate screen, wide characters, hyperlinks, selections, viewport changes, synchronized output release, and geometry changes.
- Track new profile counters for materialized rows, reused rows, materialized cells, and full-repaint fallbacks.
- Re-run the nelostie stress profile and require `append_rows` or its replacement to scale with dirty rows under stable viewport identity.

### 2. Move coalescing before full snapshot construction where semantics allow

Proposal:

When multiple backend/model results are pending before a GUI render update can consume them, coalesce model-result dirty rows and metadata first, then construct at most one live-content snapshot for the latest state. Preserve immediate publication only for events that require a distinct externally visible snapshot sequence.

Expected benefit:

High when bursts produce multiple snapshots before the renderer can consume them. The surface already coalesces dirty rows during `set_render_snapshot`, but by that point every superseded snapshot has already paid the full `append_rows` cost.

Risk:

Medium to high. Snapshot-ready notifications, transcript capture, selection visual leases, synchronized output release behavior, and sequence semantics may depend on publication boundaries. The design must explicitly identify which publications are coalescible and which are externally observable.

Suggested validation:

- Add counters for model results received, render snapshots constructed, snapshots superseded before render, and coalesced pre-publication results.
- Compare notification sequences and transcript output before and after coalescing.
- Validate interactive latency for cursor, selection, bell, mouse reporting mode, and resize-related updates.
- Re-run the stress profile and check that snapshot calls per hot bucket decrease before evaluating row-construction speed.

### 3. Split full snapshots from render deltas for the surface renderer

Proposal:

Keep full `Terminal_render_snapshot` as the stable public/debug/transcript representation, but add a renderer-facing internal delta object for normal live updates. The delta would carry dirty rows, updated row cells/provenance for those rows, cursor/mode/style changes, and invalidation flags. The surface renderer would maintain its own last full render state and apply deltas.

Expected benefit:

High for steady-state dirty-row updates. This avoids forcing every render update through a full snapshot object solely to satisfy consumers that need complete visible content.

Risk:

High. It introduces a second representation and stateful renderer-side reconstruction. Bugs can appear as stale rows or missed invalidations. The representation boundary must be deliberately owned and heavily tested.

Suggested validation:

- Keep full snapshot generation available behind test/debug paths and compare renderer deltas against full snapshots in replay tests.
- Add forced full-resync paths on viewport identity mismatch, grid resize, active-buffer transition, public projection mismatch, and memory pressure.
- Record delta apply failures loudly and fall back to full repaint, with counters so fallback frequency is visible.

### 4. Optimize dirty-row collection after publication cost is addressed

Proposal:

Replace `std::set<int>` dirty-row storage with a representation that matches bounded grid rows, such as a generation-marked vector, bitset, or small sorted vector with bitmap membership. Keep the one-row duplicate fast path, but avoid tree inserts for repeated bounded row marks.

Expected benefit:

Low to medium for this specific profile, because `publish_pending_changes`, `dirty_rows`, and `render_snapshot::dirty_rows` are small compared with `append_rows`. It may still reduce CPU and allocator pressure under heavier mark rates and make dirty-row stats less costly.

Risk:

Low to medium. The behavior is bounded by grid size, but resize, alternate screen, synchronized output, and publication identity rules must preserve sorted unique rows and stable mutation identity flags.

Suggested validation:

- Add focused microbenchmarks for millions of repeated dirty marks across same-row, alternating-row, full-grid, and random-row patterns.
- Preserve existing dirty-row timeline counters and compare mark/publish timings before and after the data-structure change.
- Confirm no change in `dirty_rows_snapshot_rows`, `published_unique_rows`, and dirty range outputs on transcript replay.

### 5. Add more targeted profiling counters

Proposal:

Add counters that explain snapshot scaling directly:

- Snapshot rows visited.
- Snapshot rows materialized from active grid vs retained history.
- Snapshot cells scanned.
- Snapshot cells emitted.
- Hyperlink metadata lookups per snapshot.
- Dirty rows requested vs rows actually materialized.
- Full-repaint dirty range fallbacks due to row identity mismatch.
- Snapshots constructed but superseded before render.

Expected benefit:

Medium. This does not fix the cost, but it makes validation precise and prevents future regressions from hiding behind aggregate `append_rows` time.

Risk:

Low if profiling-only and compiled under existing profiling guards. Avoid noisy logging in hot paths.

Suggested validation:

- Confirm counters sum coherently on small deterministic fixtures.
- Re-run the nelostie profile and use the counters to verify whether changes reduce materialized rows/cells, not only wall-clock time.

## Recommended order of work

1. Add targeted profiling counters for row/cell materialization and superseded snapshots.
2. Implement pre-publication coalescing for clearly coalescible backend/model updates, because it can reduce full snapshot count without changing row representation first.
3. Prototype a row-cache or renderer-delta path behind a testable internal switch and compare it against full snapshots.
4. Optimize dirty-row storage once the larger publication cost is under control, unless new counters show dirty-row storage has become the next bottleneck.

## Files inspected

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`

## Report file

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_render_snapshot_dirty_rows_report.md`
