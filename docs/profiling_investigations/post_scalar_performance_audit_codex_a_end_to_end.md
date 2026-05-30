# Post-scalar performance audit: end-to-end pipeline

## Executive summary

The scalar-span profile changes the end-to-end performance picture. The earlier
`print_text` row-copy issue has largely been removed from the current capture:
`printable_ascii_row_copies=0`, and `Terminal_screen_model::apply_action::print_text`
is now 1.010 s total instead of the dominant tens-of-seconds cost seen in the
older profile. The remaining dominant costs are downstream.

Current measured top costs in `nelostie_profile_scalar_span.txt`:

| Rank | Pipeline area | Measured scope | Calls | Total | Mean | Evidence class |
| ---: | --- | --- | ---: | ---: | ---: | --- |
| 1 | Render-frame construction | `build_terminal_render_frame` | 662 | 26.128 s | 39.468 ms | Measured |
| 2 | Snapshot materialization | `render_snapshot::append_rows` | 1,498 | 10.597 s | 7.074 ms | Measured |
| 3 | QSG text synchronization | `sync_text_resource_nodes` | 662 | 8.044 s | 12.152 ms | Measured |
| 4 | Surface sync | `VNM_TerminalSurface::sync_from_session` | 1,771 | 2.475 s | 1.397 ms | Measured |
| 5 | Model ingest | `Terminal_screen_model::ingest` | 1,500 | 1.829 s | 1.220 ms | Measured |
| 6 | Printable text mutation | `apply_action::print_text` | 83,349 | 1.010 s | 12.122 us | Measured |

The most important audit finding is that full-grid work remains in multiple
stages even when dirty-row payloads are small. The current run publishes 1,498
full snapshots, visits and materializes 352,030 rows, scans 307.322 M cells for
snapshot construction, and then the render thread performs two full-cell passes
over 55.837 M cells. Dirty rows are measured and useful, but they mostly prune
QSG row reuse after full snapshot and render-frame construction have already
paid full-grid costs.

Do not treat snapshot append as the only problem. It is a major cost, but the
render-frame `cells` and `packed_data` passes together cost more than snapshot
append in this profile. QSG text synchronization is also still large, despite
substantial row reuse and ASCII coalescing.

## Measured evidence

### Workload geometry and dirty-row pressure

From `nelostie_profile_scalar_span.txt`:

| Metric | Value |
| --- | ---: |
| Rows | 235 |
| Columns | 873 |
| Full visible cell positions | 205,155 |
| Dirty mark requests | 4,380,137 |
| Duplicate dirty mark requests | 4,258,550 |
| Unique pending row marks | 121,587 |
| Dirty-row snapshot calls | 1,500 |
| Dirty-row snapshot rows | 14,379 |
| Published unique rows | 14,379 |
| `publish_pending_changes` calls | 174,953 |
| Snapshot publications | 1,498 |
| Rendered frames | 662 |

Derived observations:

| Observation | Value |
| --- | ---: |
| Duplicate dirty mark rate | 97.2% |
| Average dirty rows per dirty-row snapshot | 9.6 |
| Snapshot publications per rendered frame | 2.26 |
| Full snapshot cells emitted | 134.005 M |
| Render-frame cells considered | 55.837 M |
| Render-frame dirty rows | 11,096 |
| Full-dirty render rows | 2,115 |

Interpretation:

Dirty-row metadata says most publications are partial: 14,379 dirty visible rows
across 1,498 snapshots, with no full-repaint snapshot fallbacks. However,
snapshot construction still visits and materializes all 235 rows for each
snapshot, and render-frame construction still walks all cells for each rendered
frame.

### Backend output ingestion through model mutation

Measured aggregate GUI-thread scopes:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `Terminal_session::process_pending_commands` | 2,092 | 13.492 s | 6.449 ms | 185.503 ms |
| `Terminal_session::process_backend_output_command` | 1,500 | 13.389 s | 8.926 ms | 33.647 ms |
| `Terminal_session::ingest_backend_output_segment` | 1,500 | 13.270 s | 8.846 ms | 32.880 ms |
| `Terminal_session::model_ingest` | 1,500 | 1.831 s | 1.221 ms | 28.615 ms |
| `Terminal_screen_model::ingest` | 1,500 | 1.829 s | 1.220 ms | 28.614 ms |
| `Terminal_screen_model::parser_ingest` | 1,500 | 470.349 ms | 313.565 us | 4.047 ms |
| `Terminal_screen_model::apply_parser_actions` | 1,500 | 1.216 s | 810.681 us | 21.310 ms |
| `Terminal_screen_model::apply_action::print_text` | 83,349 | 1.010 s | 12.122 us | 774.300 us |
| `Terminal_screen_model::apply_action::control_sequence` | 20,284 | 143.401 ms | 7.069 us | 20.171 ms |
| `Terminal_screen_model::apply_action::style_mutation` | 67,289 | 4.953 ms | 73 ns | 30.700 us |

Current model counters:

| Counter | Value |
| --- | ---: |
| `print_text_calls` | 83,349 |
| `printable_ascii_span_calls` | 54,736 |
| `printable_ascii_span_characters` | 3,046,585 |
| `printable_ascii_row_copies` | 0 |
| `printable_ascii_local_cells_inspected` | 2,387,338 |
| `scalar_span_local_cells_inspected` | 1,388,543 |
| `row_content_generation_comparisons` | 1,241 |
| `row_content_generation_comparison_cells` | 1,083,393 |
| `dirty_marks_from_text_writes` | 164,208 |

Interpretation:

Model mutation is no longer the main bottleneck. `print_text` remains the main
model child, but it is now an order of magnitude below snapshot append and two
orders below render-frame construction plus QSG update. Parser ingest is also
not dominant. The residual model risk is local-cell inspection and remaining
content-generation comparisons, but that is an optimization candidate only after
downstream full-grid work is addressed or reprofiled.

### Snapshot publication

Measured aggregate scopes and counters:

| Scope / counter | Value |
| --- | ---: |
| `Terminal_session::publish_backend_render_snapshot` | 11.428 s over 1,498 calls |
| `Terminal_session::publish_render_snapshot` | 11.421 s over 1,498 calls |
| `Terminal_screen_model::render_snapshot` | 10.635 s over 1,498 calls |
| `Terminal_screen_model::render_snapshot::append_rows` | 10.597 s over 1,498 calls |
| `render_snapshot_rows_visited` | 352,030 |
| `render_snapshot_rows_materialized` | 352,030 |
| `render_snapshot_cells_scanned` | 307,322,190 |
| `render_snapshot_cells_emitted` | 134,005,265 |
| `render_snapshot_dirty_rows_requested` | 14,379 |
| `render_snapshot_dirty_rows_visible` | 14,379 |
| `render_snapshot_full_repaint_fallbacks` | 0 |
| `render_snapshot_zero_dirty_publications` | 0 |

Interpretation:

`append_rows` accounts for 99.6% of `Terminal_screen_model::render_snapshot` and
92.8% of `Terminal_session::publish_render_snapshot`. The source confirms why:
`Terminal_screen_model::render_snapshot` computes dirty ranges, then loops every
viewport row, obtains `viewport_row_cells`, appends snapshot cells, appends
hyperlink metadata, and appends visible-line provenance. Dirty rows are metadata
on a full snapshot; they do not prune snapshot materialization.

The source also shows that session dirty coalescing occurs after the full
snapshot exists. `Terminal_session::publish_render_snapshot` constructs a
snapshot with `m_screen_model->render_snapshot(request)` before it can call
`snapshot_with_coalesced_dirty_rows` for unrendered generations. Surface-level
coalescing in `VNM_TerminalSurface_render_bridge::set_render_snapshot` is also
after construction.

### Surface sync

Measured aggregate scope:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `VNM_TerminalSurface::sync_from_session` | 1,771 | 2.475 s | 1.397 ms | 6.238 ms |

Source evidence:

`sync_from_session` updates process/backend/selection/grid properties, checks
render snapshot generation, calls `set_render_snapshot` when generation changes,
marks the generation synced, updates viewport state from the snapshot, handles
IME state, drains notifications, and synchronizes the synchronized-output
recovery timer.

Interpretation:

This is not the largest bottleneck, but it is no longer negligible because it
runs 1,771 times. Some cost may be real property notification/QML work, while
some may be avoidable repeated syncs where no render generation changed. The
profile does not break this scope down, so any sub-bottleneck inside surface sync
is inferred.

### Render-frame construction

Measured aggregate render-thread scopes:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `VNM_TerminalSurface::updatePaintNode` | 662 | 39.623 s | 59.854 ms | 1.001 s |
| `build_terminal_render_frame` | 662 | 26.128 s | 39.468 ms | 78.034 ms |
| `build_terminal_render_frame::cells` | 661 | 13.281 s | 20.092 ms | 42.351 ms |
| `build_terminal_render_frame::packed_data` | 661 | 12.751 s | 19.290 ms | 39.099 ms |
| `Qsg_terminal_renderer::update_node` | 662 | 12.984 s | 19.613 ms | 950.255 ms |

Renderer counters:

| Counter | Value |
| --- | ---: |
| `frame_cell_pass_input_cells` | 55,837,192 |
| `frame_packed_pass_input_cells` | 55,837,192 |
| `frame_dirty_row_lookup_count` | 111,674,384 |
| `frame_cells_considered` | 55,837,192 |
| `frame_text_cells_rendered_as_text` | 32,952,136 |
| `frame_text_cells_rendered_as_graphic` | 22,885,056 |

Source evidence:

`build_terminal_render_frame` first loops over `snapshot->cells` in the `cells`
scope to classify cells, produce background rects, text runs, graphics,
decorations, cursor interactions, and stats. It then calls
`build_terminal_render_frame_packed_data`, which builds an explicit row table
from the same snapshot and loops rows/cells again to emit packed text and graphic
spans. Each pass calls dirty-row classification or records dirty-row lookup work.

Interpretation:

This is the highest-confidence current bottleneck. It is fully measured and its
source shape is clear: two full-cell passes over the same full snapshot. The
packed-data path is not ancillary; it is almost as expensive as the main cell
pass.

### QSG update and text synchronization

Measured aggregate scopes:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `sync_text_resource_nodes` | 662 | 8.044 s | 12.152 ms | 949.792 ms |
| `make_text_resource_node` | 7,906 | 4.320 s | 546.419 us | 7.391 ms |
| `append_batched_text_run_nodes` | 7,906 | 4.317 s | 546.046 us | 7.390 ms |
| `prepare_text_layout` | 483,907 | 1.400 s | 2.893 us | 349.500 us |
| `add_text_run_layout` | 483,907 | 2.670 s | 5.517 us | 1.320 ms |
| `text_resource_row_descriptor` | 93,517 | 1.060 s | 11.334 us | 644.600 us |
| `sync_text_resource_nodes::reparent_slots` | 294 | 554.292 ms | 1.885 ms | 43.836 ms |
| `sync_text_resource_nodes::replace_cache_entry` | 4,513 | 487.734 ms | 108.073 us | 4.164 ms |
| `sync_text_resource_nodes::remove_stale_entries` | 662 | 411.428 ms | 621.492 us | 123.234 ms |

Renderer counters:

| Counter | Value |
| --- | ---: |
| `text_runs_considered` | 32,952,136 |
| `text_resource_runs_before_coalescing` | 4,378,068 |
| `text_resource_runs_after_coalescing` | 483,907 |
| `text_content_rebuilds` | 7,906 |
| `text_content_reused` | 85,611 |
| `text_clean_reuse_skips` | 83,853 |
| `text_resource_descriptor_reuses` | 1,758 |
| `text_cache_entries_created` | 3,393 |
| `text_cache_entries_replaced` | 4,513 |
| `text_leaf_nodes_created` | 73,082 |
| Slow text layouts over 10 ms | 0 |

Interpretation:

QSG text synchronization is improved compared with older evidence, but still a
major cost. Existing clean-row reuse is effective: 83,853 clean rows skip cache
rebuild. ASCII coalescing is also effective: 4.378 M resource runs become
483,907 layout runs. The remaining measured cost is concentrated in dirty-row
rebuilds, text-node construction, and many small `QTextLayout` additions, not in
isolated slow layout outliers.

Graphic row synchronization is also non-trivial: `sync_graphic_rect_row_layer`
is 2.351 s, including `rect_layer_groups` at 1.163 s and `row_local_rects` at
560.090 ms. This likely follows from 22.885 M cells rendered as graphics in the
current scalar-span profile.

## Pipeline map

1. Backend output command drain.

`VNM_TerminalSurface` drains backend callbacks and calls
`Terminal_session::process_pending_commands`. That loop pops queued commands,
optionally slices large backend output under a deadline, dispatches to
`process_command`, records results, and updates queue/backpressure state.

2. Backend output pre-scan and segmentation.

`Terminal_session::process_backend_output_command` records output, handles
transcript capture, combines pending prescan bytes, searches for incomplete CSI
and synchronized-output set/reset sequences, and calls
`ingest_backend_output_segment` for each segment.

3. Model ingest and mutation.

`Terminal_session::ingest_backend_output_segment` captures viewport/release
context, calls `m_screen_model->ingest(bytes)`, stores the result, handles parser
actions, syncs viewport state, and conditionally publishes a render snapshot.
Inside `Terminal_screen_model::ingest`, parser bytes are converted to actions,
each action is applied, dirty changes are either collected under synchronized
output or published into the ingest publication, and the final dirty-row result
is produced.

4. Snapshot publication.

When model results or metadata warrant publication,
`Terminal_session::publish_render_snapshot` builds a full
`Terminal_render_snapshot` by calling `Terminal_screen_model::render_snapshot`.
The model computes dirty row ranges, but then appends every visible row into the
snapshot. Session dirty-row coalescing can happen only after this full snapshot
has already been constructed.

5. Surface sync.

`VNM_TerminalSurface::sync_from_session` observes the session generation, pulls
the latest snapshot handle through the render bridge, updates surface viewport
state, marks the snapshot generation synced, updates IME and notification state,
and requests a render update. If a render update is already pending,
`set_render_snapshot` may coalesce dirty rows between old and new snapshots, but
again only after the new snapshot has already been built.

6. Render-frame construction.

`VNM_TerminalSurface::updatePaintNode` builds a `Terminal_render_frame` from the
current immutable snapshot. The frame builder performs a full `cells` pass over
`snapshot->cells`, then a second full `packed_data` pass over an explicit row
table derived from the same cells.

7. QSG update.

`Qsg_terminal_renderer::update_node` syncs rectangle, graphic, text, cursor, and
overlay layers. Text sync groups frame text runs by row, uses dirty-row metadata
and retained row identity for reuse, coalesces ASCII runs, builds text resource
keys, creates or replaces QSG text resource nodes for dirty rows, removes stale
entries, and reparents row slots when ordering changed.

## Bottleneck ranking

### 1. Duplicated full-cell render-frame construction

Evidence: measured.

`build_terminal_render_frame` is 26.128 s. Its two dominant children are
`cells` at 13.281 s and `packed_data` at 12.751 s. Counters show both passes
consume 55.837 M input cells, and dirty-row lookup count is about twice that.

Why it ranks first:

This is the largest current measured cost and has a clear source-level shape:
two full passes over the same full snapshot. Reducing only snapshot append leaves
this render-thread bottleneck intact for every rendered frame.

### 2. Full visible-row snapshot materialization before dirty-row pruning

Evidence: measured plus source-confirmed.

`render_snapshot::append_rows` is 10.597 s and accounts for nearly all model
snapshot construction. The current profile has only 14,379 visible dirty rows,
but snapshot construction visits and materializes 352,030 rows.

Why it ranks second:

This is the largest GUI-thread cost after scalar-span changes. It also feeds the
render-frame full-cell work. However, changing snapshot semantics is higher risk
than fusing render-frame passes because public/debug/transcript consumers expect
full snapshots.

### 3. QSG text-resource rebuild and many-small-layout work

Evidence: measured.

`sync_text_resource_nodes` is 8.044 s. The main child is `make_text_resource_node`
through `append_batched_text_run_nodes`, with 483,907 layout additions. Existing
row reuse and ASCII coalescing are effective but not sufficient.

Why it ranks third:

This is smaller than render-frame construction and snapshot append, but still a
major render-thread cost. It also has a 950 ms max outlier in the aggregate
`Qsg_terminal_renderer::update_node` / text sync path, so tail latency needs
attention even though slow layout tracking shows no individual >10 ms layout.

### 4. Surface sync frequency and property/snapshot handoff cost

Evidence: measured, root cause inferred.

`VNM_TerminalSurface::sync_from_session` is 2.475 s over 1,771 calls. The scope
is not subdivided, so likely causes include repeated property setters, snapshot
handle handoff, notification draining, viewport updates, and render update
scheduling.

Why it ranks fourth:

It is smaller than the top three, but large enough to instrument. Its call count
is higher than snapshot publication count, so no-op or low-value syncs may exist.

### 5. Residual model mutation and parser work

Evidence: measured.

`Terminal_screen_model::ingest` is 1.829 s. `print_text` is 1.010 s, parser
is 470 ms, and control sequences are 143 ms. Row copies are zero, and local-cell
inspection counters are now the better model-level diagnostic.

Why it ranks fifth:

The previous hot path has been substantially improved. Further model changes
should be guided by new child counters or profiles after downstream full-grid
costs are reduced.

### 6. Snapshot producer/consumer mismatch

Evidence: partially measured, savings inferred.

There are 1,498 snapshot publications and 662 rendered frames. Session stats
report `snapshots_superseded_before_render=112`, `snapshots_marked_rendered=1453`,
and `max_unrendered_snapshot_generations=11`.

Why it ranks below append/render-frame work:

The mismatch exists, but the current measured superseded count is only 112, not
hundreds. It may still matter for burst latency and max unrendered generations,
but it does not explain most of the 10.597 s snapshot append cost in this run.

## Non-obvious hypotheses

### Hypothesis 1: `packed_data` now deserves equal attention with visual cell classification

Status: measured.

The packed-data pass is 12.751 s, nearly equal to the main `cells` pass. Any
optimization plan that only improves text/graphic visual emission while leaving
packed data as a second full pass will preserve roughly half of render-frame
construction cost.

### Hypothesis 2: dirty-row lookup itself may be a hidden full-frame tax

Status: inferred from counters and source.

`frame_dirty_row_lookup_count=111,674,384`, which matches two full-cell passes.
The measured scopes do not isolate dirty-row lookup time. Since dirty ranges are
small and sorted, each lookup may be cheap, but the count is high enough to
instrument before dismissing it.

### Hypothesis 3: snapshot append emits fewer cells than it scans, but still materializes every row

Status: measured and source-confirmed.

The model scans 307.322 M potential cells and emits 134.005 M snapshot cells.
That means sparse/omitted cells reduce output size, but the row-level loop still
scales with every visible row and full row width through row retrieval, metadata,
and hyperlink/provenance handling.

### Hypothesis 4: surface sync may be doing unnecessary work even when render generation does not change

Status: inferred.

`sync_from_session` has 1,771 calls for 1,498 snapshot publications. The function
updates several properties before checking snapshot generation. The profile does
not say whether repeated setter calls short-circuit cheaply or trigger expensive
Qt notifications. Add subscopes/counters before changing behavior.

### Hypothesis 5: QSG text cache identity is mostly working; the issue is dirty-row rebuild pressure plus many layout additions

Status: measured.

The current renderer reports 85,611 text content reuses and 83,853 clean cache
skips. The frame cache key is stable context, not text content. The remaining
cost comes from 7,906 rebuilt text resources and 483,907 layout additions, not
from wholesale cache invalidation of all rows.

### Hypothesis 6: graphic geometry is now an additional downstream pressure point

Status: measured, root cause inferred.

The current profile has 22.885 M cells rendered as graphics and 2.351 s in
`sync_graphic_rect_row_layer`. This was not the headline in the older text-heavy
profile, but scalar-span changes may have shifted more work into graphic
geometry and packed graphic paths.

### Hypothesis 7: max-latency outliers are not explained by slow individual text layouts

Status: measured gap.

`slow_text_layouts` reports zero calls over 10 ms, but `updatePaintNode` max is
1.001 s and `sync_text_resource_nodes` max is 949.792 ms. The outlier is likely
aggregate row/node churn, deletion/reparenting, or scene-graph synchronization
rather than a single `QTextLayout` call.

## Recommended next investigations

1. Add render-frame subcounters for avoidable duplicate work.

Track row-table construction time, dirty-row lookup time, style-attribute lookup
time, classification time, packed text emission time, packed graphic emission
time, and per-pass rows/cells skipped by dirty-row eligibility.

2. Add snapshot append child scopes.

Split `append_rows` into viewport row resolution, row cell retrieval/copy,
hyperlink metadata, visible-line provenance, and history/active-grid lookup.
This should decide whether to optimize representation, row lookup, hyperlink
metadata, or the full-snapshot contract.

3. Subdivide `VNM_TerminalSurface::sync_from_session`.

Measure property updates, generation check, snapshot bridge handoff, viewport
state update, notification replay, IME update, and recovery timer sync. Also
count sync calls with no new snapshot generation.

4. Investigate render-thread max outliers.

Add per-frame reporting for rows rebuilt, nodes destroyed, nodes created,
wrapper reparent count, stale text entries removed, graphic row rebuilds, and
scene graph deletion counts. Correlate outlier frames with those counters.

5. Compare current scalar profile against `nelostie_profile_span_local.txt` and
`nelostie_profile_hardened.txt` only for trend validation.

The older profiles should be used as context, not as current ranking evidence.
The current profile already proves the dominant rank has shifted.

6. Confirm whether packed render data is required for the same consumer path on
every frame.

If packed data is diagnostic or optional under some runtime paths, its 12.751 s
cost may be avoidable without a complex fusion. If it is required, fusion with
the main cell pass becomes the more plausible candidate.

## Implementation candidates

These are candidates only; this audit did not implement changes.

### Candidate 1: Fuse render-frame `cells` and `packed_data` passes

Target: `build_terminal_render_frame::cells` and `build_terminal_render_frame::packed_data`.

Fix shape:

Emit packed text and graphic spans while the main cell pass already has the
cell, style attributes, classification result, cursor/IME coverage, dirty state,
and row/provenance context. Avoid building a separate explicit row table unless
ordering or packed row finalization requires it.

Expected benefit: high.

Risk: medium. The frame builder has many interactions: cursor, IME, simple
content classification, graphic geometry, text runs, background rects,
decorations, and packed row boundaries.

### Candidate 2: Precompute dirty-row membership per frame

Target: repeated `snapshot_row_is_dirty` / `row_is_dirty` style lookups.

Fix shape:

Build a compact row dirty mask or row generation table once per frame and use O(1)
lookup in both render-frame passes and QSG row sync.

Expected benefit: unknown until dirty lookup is separately measured.

Risk: low to medium. The correctness risk is low if the mask is derived from the
same dirty ranges, but the performance benefit may be small relative to other
work.

### Candidate 3: Reduce full snapshot append constant factors

Target: `Terminal_screen_model::render_snapshot::append_rows`.

Fix shape:

After subprofiling, reduce row retrieval/copy and metadata work without changing
the public full-snapshot contract. Possible directions include row views for
active-grid rows, avoiding optional vector copies, caching viewport-to-backing
row resolution within the append loop, and fast paths for rows with no hyperlink
metadata.

Expected benefit: medium.

Risk: low to medium if output remains bit-equivalent.

### Candidate 4: Renderer-facing delta or retained row-frame cache

Target: full-grid scaling across snapshot, frame, and QSG stages.

Fix shape:

Keep full snapshots for public/debug/transcript consumers, but add a
renderer-facing retained row cache or delta stream keyed by row identity and
content generation. Recompute only dirty rows, cursor/IME affected rows,
selection affected rows, geometry changes, and rows whose viewport identity
changed.

Expected benefit: high under partial-dirty workloads.

Risk: high. This creates state-retention semantics that must have explicit full
fallbacks and a full-snapshot oracle.

### Candidate 5: Improve QSG text rebuild path for dirty but descriptor-identical rows

Target: `sync_text_resource_nodes`.

Fix shape:

The current profile already has 1,758 descriptor reuses and zero key-match
reuses. Investigate whether more dirty rows can reuse descriptors before key
build and node rebuild, especially rows whose retained identity changed only in
ways irrelevant to text layout.

Expected benefit: medium.

Risk: medium. Must preserve cursor text, IME, selection, row identity, and clip
semantics.

### Candidate 6: Reduce graphic row-layer churn

Target: `sync_graphic_rect_row_layer`, `rect_layer_groups`, and `row_local_rects`.

Fix shape:

Use the packed graphic spans or row-local graphic resources generated during
frame construction to avoid regrouping and re-localizing large graphic rect
lists in QSG sync.

Expected benefit: medium in the current scalar profile.

Risk: medium. Graphic geometry has software/hardware scene graph fallback paths
and row-cache behavior.

### Candidate 7: Gate or defer low-value surface sync work

Target: `VNM_TerminalSurface::sync_from_session`.

Fix shape:

After subprofiling, avoid repeated property updates or notification/timer work
when the relevant session generation has not changed. Keep state transitions and
error notifications explicit; do not silently skip user-visible changes.

Expected benefit: medium to low.

Risk: medium. Qt property notification semantics and session state visibility are
user-facing.

## Validation gates

### Gate 1: Measurement gate before implementation

Required:

| Area | Required measurement |
| --- | --- |
| Render frame | Child scopes for row table, dirty lookup, classification, style lookup, packed text, packed graphics. |
| Snapshot append | Child scopes for row lookup/copy, hyperlink metadata, provenance, history traversal. |
| Surface sync | Generation-changed versus no-generation sync counters and child scopes. |
| QSG outliers | Per-frame node create/destroy/reparent/stale-removal counters correlated with max frames. |

Pass condition:

The new counters explain at least 80% of each target parent scope without
materially altering the profile shape.

### Gate 2: Render-frame pass fusion

Before implementation:

Prove packed data is required on the hot path and cannot simply be gated.
Document the equivalence contract between old packed rows/spans and fused output.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `build_terminal_render_frame::packed_data` | Near zero or removed as a separate full pass. |
| `build_terminal_render_frame::cells` | Up less than the removed packed-data cost. |
| `build_terminal_render_frame` total | Down substantially. |
| Packed row/span/cell counters | Equivalent. |
| Text/graphic/cursor/IME/selection frame counters | Equivalent. |

### Gate 3: Snapshot append reduction

Before implementation:

Classify whether the change is constant-factor full-snapshot optimization or a
contract change. If it is a contract change, read `varinomics_change_governance.md`
and split into governed batches.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `render_snapshot::append_rows` total | Down. |
| `render_snapshot_rows_visited` | Same unless contract changes. |
| `render_snapshot_rows_materialized` | Same unless contract changes. |
| `render_snapshot_cells_emitted` | Semantically equivalent. |
| Snapshot validation / replay oracle | Equivalent. |

### Gate 4: QSG text and graphic reuse changes

Before implementation:

Add reuse failure reason counters for text and graphic row resources. Confirm
which miss class dominates.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `sync_text_resource_nodes` | Down. |
| `make_text_resource_node` | Down. |
| `prepare_text_layout` / `add_text_run_layout` calls | Down for partial-dirty frames. |
| `text_content_reused` | Up or unchanged with lower cost. |
| `text_cache_entries_replaced` and stale removals | Down. |
| `sync_graphic_rect_row_layer` | Down if graphic candidate is implemented. |

### Gate 5: Surface sync changes

Before implementation:

Record how many `sync_from_session` calls have no render generation change, no
IME generation change, and no pending notifications.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `sync_from_session` total | Down. |
| Snapshot handoff count | Semantically equivalent. |
| Process/backend/selection/grid property notifications | No missing state transitions. |
| Render update requests | No lost renders. |

### Gate 6: End-to-end regression gate

Use the same workload geometry and compare:

| Metric | Expected direction |
| --- | --- |
| GUI-thread total | Down. |
| Render-thread total | Down substantially for render-frame work. |
| `updatePaintNode` max | Down or explained by new counters. |
| Snapshot publications | Same unless explicitly changed. |
| Rendered frames | Same order unless scheduling changed. |
| Dirty-row counters | Same semantic values. |
| Slow text layouts | Remains zero over 10 ms. |

## Files inspected

| File | Use |
| --- | --- |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md` | Local reporting/style constraints. |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md` | Local reporting/style constraints. |
| `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md` | Audit scope calibration. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt` | Current measured evidence. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_profile_final_consolidated_report.md` | Earlier-profile context and comparison. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp` | Backend output ingestion, model ingest, snapshot publication, session coalescing. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp` | Model ingest, action dispatch, dirty rows, render snapshot append. |
| `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp` | Surface sync, render bridge handoff, `updatePaintNode`. |
| `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp` | Render-frame construction, QSG update, text and graphic row sync. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\vnm_terminal_surface.h` | `updatePaintNode` declaration and surface fields found during symbol search. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_render_frame.h` | Render-frame builder declaration found during symbol search. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h` | Snapshot type reference found during symbol search. |
