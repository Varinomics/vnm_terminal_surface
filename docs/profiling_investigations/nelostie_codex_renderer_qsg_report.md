# Nelostie QSG renderer and text-resource profiling investigation

## Scope

This report investigates the `vnm_terminal_surface` render-thread/QSG profile from the Nelostie stress demo captured at:

`C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`

The demo intentionally uses a very large terminal grid and many dirty rows. The recommendations below assume that workload is valid and focus on work that can scale better under it.

## Executive summary

Direct profile evidence points to three material costs:

1. GUI-side render snapshot publication copies the visible grid repeatedly. `Terminal_screen_model::render_snapshot::append_rows` accounts for 7.797 s across 1,252 backend snapshot publications, and `Terminal_session::publish_render_snapshot` accounts for 8.487 s in that path.
2. Render-thread frame construction walks the large snapshot again. `build_terminal_render_frame` accounts for 8.368 s over 293 rendered frames, split almost evenly between `build_terminal_render_frame::cells` at 4.229 s and `build_terminal_render_frame::packed_data` at 4.100 s.
3. QSG text synchronization dominates the QSG update. `Qsg_terminal_renderer::update_node` accounts for 11.133 s over 293 frames, and `sync_text_resource_nodes` accounts for 10.547 s of that. The largest child is `make_text_resource_node` at 6.470 s, mostly `prepare_text_layout` and `add_text_run_layout` over 516,835 layout calls each.

Direct profile evidence does not support treating the newly profiled flat-ring retained-history path as a material bottleneck in this run. The retained-history append paths visible in the GUI profile total about 14.620 ms across 458 appends, while the GUI root accounts for 48.299 s and the renderer-related GUI snapshot path accounts for seconds.

## Workload and dirty-row evidence

Direct profile evidence:

- Surface geometry was 233 rows by 871 columns, with a 3.5 px by 6.98438 px cell size and a 1.25 device pixel ratio.
- Dirty-row tracking recorded 7,875,373 mark requests, 7,224,579 duplicate mark requests, 650,794 unique pending row marks, and 41 `mark_all_dirty` calls.
- Dirty-row snapshots were taken 1,298 times and contained 55,889 total rows. That is about 43 rows per snapshot on average, but the renderer saw larger coalesced dirty sets: cumulative renderer stats report 28,167 text dirty rows over 293 frames, about 96 dirty rows per rendered frame.
- The final renderer stats show a full-height text update: `text_dirty_row_ranges=1`, `text_dirty_rows=233`, `text_runs_considered=102399`, `text_resource_runs_before_coalescing=102399`, and `text_resource_runs_after_coalescing=955`.
- Snapshot production outpaced rendering. The GUI profile shows 1,252 backend `Terminal_session::publish_render_snapshot` calls, plus 13 additional publish calls under `Terminal_session::process_pending_commands`, while the render thread painted 293 frames.

Source-based inference:

- Dirty-row coalescing is doing useful work, but it primarily limits retained QSG row replacement. It does not prevent the GUI from constructing full snapshots, and it does not prevent render-thread frame construction from scanning all cells in the current snapshot.

## GUI snapshot publication choke point

Direct profile evidence:

- `Terminal_session::publish_backend_render_snapshot`: 1,252 calls, 8.494 s total, 6.785 ms mean.
- `Terminal_session::publish_render_snapshot`: 1,252 calls, 8.487 s total, 6.779 ms mean.
- `Terminal_screen_model::render_snapshot`: 1,252 calls, 7.831 s total, 6.255 ms mean.
- `Terminal_screen_model::render_snapshot::append_rows`: 1,252 calls, 7.797 s total, 6.228 ms mean.
- A second smaller path under `Terminal_session::process_pending_commands` adds 13 `publish_render_snapshot` calls totaling 44.309 ms, with 40.258 ms in `Terminal_screen_model::render_snapshot` and 39.905 ms in `append_rows`.

Direct source evidence:

- `Terminal_screen_model::render_snapshot` reserves `rows * columns` cell capacity, then `append_rows` loops over every viewport row and appends row cells into `snapshot.cells`.
- `Terminal_session::publish_render_snapshot` builds the full snapshot before applying dirty-row coalescing against the latest unsynced snapshot.
- `VNM_TerminalSurface_render_bridge::set_render_snapshot` can coalesce dirty rows again when a render update is already pending, but this also happens after a full snapshot object already exists.

Source-based inference:

- The pipeline behaves like full-frame publication with dirty-row metadata, not an incremental row-delta publication. Under large-grid stress, this makes snapshot publication scale with visible cell count and publication count, even when the eventual render thread consumes far fewer frames.

Actionable improvement options:

- High benefit, medium-high risk: introduce an incremental render publication path that carries retained row identities plus changed row payloads, and lets the render side keep a retained full view. Dirty rows would drive which row payloads are copied rather than only which row QSG nodes are rebuilt. Validation should compare `Terminal_screen_model::render_snapshot::append_rows` total time and allocation volume before and after, using the same stress demo and at least one scrollback/selection/resize workload.
- Medium-high benefit, medium risk: if full snapshots remain the public render contract, defer full snapshot materialization until a render update is actually going to consume it. Multiple GUI publications already coalesce into 293 render calls, so avoiding construction of superseded snapshots would target the 1,265 publication calls directly. Validation should add counters for superseded snapshot publications and confirm that rendered sequence, selection behavior, cursor behavior, and synchronized-output behavior remain correct.
- Medium benefit, low-medium risk: split snapshot publication profiling into row-copy, retained-history row lookup, hyperlink metadata, and visible-line provenance subscopes. The current `append_rows` scope is already conclusive enough to prioritize it, but more detail would reduce implementation risk for an incremental snapshot design.

## Render-thread frame construction choke point

Direct profile evidence:

- `VNM_TerminalSurface::updatePaintNode`: 293 calls, 19.748 s total, 67.398 ms mean, 1.578 s max.
- `build_terminal_render_frame`: 293 calls, 8.368 s total, 28.561 ms mean, 89.951 ms max.
- `build_terminal_render_frame::cells`: 292 calls, 4.229 s total, 14.483 ms mean, 49.414 ms max.
- `build_terminal_render_frame::packed_data`: 292 calls, 4.100 s total, 14.042 ms mean, 44.889 ms max.
- Cumulative renderer stats report 17,364,899 cells considered/rendered across 293 frames, 14,730,881 text runs emitted, and 2,634,018 text cells rendered as graphic geometry.

Direct source evidence:

- `VNM_TerminalSurface::updatePaintNode` calls `build_terminal_render_frame` on the render thread before `Qsg_terminal_renderer::update_node`.
- `build_terminal_render_frame::cells` loops over every `snapshot->cells` entry, classifies simple content, computes geometry, style attributes, text runs, background rects, graphics, cursor interactions, and decoration state.
- `build_terminal_render_frame_packed_data` builds a row table from all `snapshot.cells`, stable-sorts each row, then loops rows and row cells to classify content again and append packed text/graphic spans.
- Both `build_terminal_render_frame::cells` and `packed_data` call `snapshot_row_is_dirty`, which scans dirty row ranges for each cell. The ranges are compact and usually small, but this still sits inside per-cell loops.

Source-based inference:

- Render-thread frame construction is effectively two full passes over the snapshot cell set. The packed path duplicates part of the simple-content classification done by the ordinary text/graphics path. Dirty rows do not avoid those passes.
- The row-table stable sort is probably not the largest cost when snapshot cells are already row-major, but it is still work proportional to every cell and row. The profile's near-even split between `cells` and `packed_data` makes duplicated per-cell work more important than any one small helper.

Actionable improvement options:

- High benefit, medium risk: build a retained render frame by viewport row. Recompute row outputs only for dirty rows, cursor/IME affected rows, selection affected rows, and rows whose retained identity changes after scroll. Reuse packed rows, text runs, graphic rects, and row-level descriptors for clean rows. Validation should target `build_terminal_render_frame` total time and separately track dirty-row count versus recomputed-row count.
- High benefit, medium risk: merge packed-data construction into the main cell pass. The main pass already classifies each cell and computes style/geometry; packed spans can be appended at the same time for fast-text and graphic-geometry cells. Validation should show a drop in `build_terminal_render_frame::packed_data` without increasing `build_terminal_render_frame::cells` by a similar amount.
- Medium benefit, low-medium risk: replace repeated dirty-range scans in per-cell classification with an O(1) row dirty bitmap or a row-state vector for the current frame. This is not the main issue by itself, but it is simple and supports the retained-row design. Validation should add a small scope or counter for dirty-row lookup if the effect is otherwise hard to isolate.
- Medium benefit, medium risk: avoid building and sorting an explicit row table when the snapshot cells are already emitted in viewport row order. A single streaming row accumulator would remove row-vector allocation and per-row stable sort. Validation should include a correctness fixture for out-of-order or sparse cells if that input shape is still a supported invariant.

## QSG text resource, cache, coalescing, and layout choke point

Direct profile evidence:

- `Qsg_terminal_renderer::update_node`: 293 calls, 11.133 s total, 37.998 ms mean, 1.503 s max.
- `sync_text_resource_nodes`: 293 calls, 10.547 s total, 35.995 ms mean, 1.502 s max.
- `make_text_resource_node`: 21,593 calls, 6.470 s total, 299.611 us mean.
- `append_batched_text_run_nodes`: 21,593 calls, 6.463 s total.
- `prepare_text_layout`: 516,835 calls, 2.201 s total, 4.259 us mean.
- `add_text_run_layout`: 516,835 calls, 3.898 s total, 7.542 us mean.
- `slow_text_layouts threshold_ns=10000000 slow_calls=0 stored_samples=0`, so the layout cost is cumulative many-small-layout work, not isolated pathological layouts over 10 ms.
- `text_run_groups_by_viewport_row`: 293 calls, 204.854 ms total.
- `text_resource_row_descriptor`: 32,112 calls, 698.923 ms total.
- `row_local_text_runs`: 21,597 calls, 345.266 ms total.
- `sync_text_resource_nodes::coalescing`: 21,597 calls, 553.832 ms total.
- `text_resource_key`: 21,597 calls, 137.050 ms total.
- `sync_text_resource_nodes::replace_cache_entry`: 15,336 calls, 988.250 ms total, including 545.192 ms in `clear_layer`.
- `sync_text_resource_nodes::remove_stale_entries`: 293 calls, 433.767 ms total, including 398.812 ms in `destroy_text_cache_entry`.
- `sync_text_resource_nodes::reparent_slots`: 284 calls, 517.688 ms total, with a 75.099 ms max.

Renderer cumulative stats:

- `text_runs_considered=14730881`.
- `text_coalescing_candidate_groups=20664` and `text_coalescing_enabled_groups=20664`.
- `text_resource_runs_before_coalescing=12660003` and `text_resource_runs_after_coalescing=516863`.
- `text_content_rebuilds=21593`, `text_content_reused=10519`, `text_clean_reuse_skips=6401`, `text_resource_descriptor_reuses=4114`.
- `text_cache_entries_created=6257`, `text_cache_entries_replaced=15336`, `text_content_removed=6094`.
- `text_leaf_nodes_created=227035`, `text_cache_entry_child_nodes_cleared_for_replacement=493431`, and `text_cache_entry_child_nodes_cleared_for_removal=249260`.

Direct source evidence:

- `sync_text_resource_nodes` groups text runs by viewport row, moves old row slots into an identity map, builds a row descriptor, optionally reuses a clean or descriptor-matching row, otherwise coalesces ASCII text runs, builds a text-resource key, and creates/replaces a row text resource node.
- Clean-row skip is available only when dirty rows are present and the frame-level text key matches. Dirty groups still must go through descriptor/key/coalescing/node creation unless descriptor/key reuse succeeds.
- Replacement deletes the old row text node and appends the new node under the existing clip. Removed stale rows destroy the wrapper/clip/node subtree. Slot order changes cause every new slot to be appended in order.

Source-based inference:

- Existing ASCII coalescing is effective: it reduces 12.660 million resource input runs to 516,863 resource runs. The remaining bottleneck is that dirty rows still cause many row text resources to be laid out and rebuilt, and each rebuilt resource can still contain many coalesced runs.
- The current cache is strongest for clean rows with stable retained provenance. It helps less when the stress workload dirties many rows per render or when viewport-row order/identity changes cause reparenting.
- Because `Terminal_render_text_run` is emitted per text cell before coalescing, the renderer pays large per-cell allocation/copy/classification costs even when coalescing later collapses the row into far fewer QSG text resources.

Actionable improvement options:

- High benefit, medium-high risk: add a fast ASCII row resource path that builds row text from compact spans rather than per-cell `Terminal_render_text_run` objects. The packed text spans already represent adjacent same-style simple text; the QSG text path could consume equivalent row spans directly. Validation should compare `text_runs_considered`, `row_local_text_runs`, `coalescing`, `prepare_text_layout`, and `add_text_run_layout` totals.
- High benefit, medium risk: retain text resources at fixed viewport-row slots, and update slot content in place instead of transferring by retained identity and reparenting wrappers on most frames. Retained line identity should remain part of content reuse, but wrapper order can be row-index stable. Validation should target `sync_text_resource_nodes::reparent_slots`, text wrapper order rebuild counts, and visual correctness during scrollback movement and alternate-screen transitions.
- Medium-high benefit, medium risk: increase descriptor reuse for dirty rows by separating row content identity from dirty status. If a row is dirty because of cursor/IME/metadata overlap but its text descriptor is unchanged, avoid rebuilding the text node and handle overlay/cursor separately. Validation should add counters for dirty-but-descriptor-identical rows and verify cursor block text inversion, IME preedit, and visual bell behavior.
- Medium benefit, medium-high risk: investigate whether row text nodes can be partially rebuilt or pooled rather than delete/recreate on replacement. The profile shows almost 1.0 s in replacement and 0.434 s in stale removal; pooling wrappers alone will not remove layout cost, but it can reduce QSG node churn. Validation should track `qsg_nodes_created`, `qsg_nodes_destroyed`, `text_leaf_nodes_created`, and maximum frame time.
- Medium benefit, low-medium risk: add profiling counters for per-row text-resource run count distribution after coalescing. Current totals prove the aggregate issue, but row distribution would identify whether a few very dense rows or many moderately dense rows dominate layout cost.

## Rect and graphic layers

Direct profile evidence:

- `sync_graphic_rect_row_layer`: 293 calls, 279.832 ms total, 955.056 us mean.
- `rect_layer_groups` under graphic rects: 134.477 ms total.
- `row_local_rects`: 4,954 calls, 61.092 ms total.
- `update_graphic_batched_rect_geometry`: 3,655 calls, 15.909 ms total.
- Cumulative stats report 2,634,013 rect-resource rects before coalescing and 57,320 after coalescing.

Source-based inference:

- Graphic rect work is visible but secondary compared with text layout and full-frame cell scanning. It is worth preserving the current coalescing path, but it should not be the first optimization target.

Actionable improvement option:

- Low-medium benefit, low risk: keep graphic rect work on the same retained-row frame path proposed above. It should then benefit naturally from dirty-row recomputation without a separate graphic-specific redesign.

## Retained-history flat ring

Direct profile evidence:

- Under `Terminal_screen_model::apply_action::print_text`, retained-history append has 117 calls totaling 3.465 ms, with 29.614 us mean and 124.400 us max.
- Under `Terminal_screen_model::apply_action::line_feed`, retained-history append has 341 calls totaling 11.156 ms, with 32.714 us mean and 100.300 us max.
- The visible retained-history append total in those two paths is about 14.620 ms across 458 calls.
- The lower-level ring operations are much smaller: for the 341-call path, `Terminal_history_ring::reserve_record` totals 83.300 us, `Terminal_history_ring::commit` totals 490.600 us, and `Terminal_history_ring::write_record_bytes` totals 335.200 us.

Direct source evidence:

- `Terminal_history_ring::reserve_record` allocates a record byte vector and writes a header/footer.
- `Terminal_history_ring::commit` makes room, validates the record bytes, and writes into the ring.

Source-based inference:

- The flat-ring retained-history path has possible future micro-optimizations, such as avoiding per-record temporary vector allocation or validation in trusted internal commits, but this profile does not justify prioritizing them for the Nelostie renderer problem. Its measured cost is tiny compared with GUI snapshot construction and render-thread QSG work.

## Recommended sequencing

1. Add or extend instrumentation before implementation: per-render recomputed-row count, clean-row reuse eligibility failure reasons, dirty-but-descriptor-identical rows, and row text-resource run distribution after coalescing.
2. Target render-thread full-frame duplication first: merge packed-data construction into the main cell pass or move to retained row-frame construction. This directly attacks 8.368 s of render-thread work and supports later QSG improvements.
3. Target text resource churn next: stabilize viewport-row wrapper slots and reduce dirty-row text rebuilds when descriptors are unchanged. This attacks the 10.547 s `sync_text_resource_nodes` cost without changing the public snapshot contract first.
4. Consider a larger snapshot-publication redesign after the render-side row-retention model is clear. Incremental publication has the largest architectural payoff, but it touches session/model/render contracts and should be governed as multi-batch work.

## Validation plan

For each candidate change, recapture the same Nelostie stress profile and compare at least these metrics:

- `Terminal_screen_model::render_snapshot::append_rows` total and mean time.
- `VNM_TerminalSurface::updatePaintNode` total, mean, max, and rendered frame count.
- `build_terminal_render_frame`, `build_terminal_render_frame::cells`, and `build_terminal_render_frame::packed_data` totals.
- `sync_text_resource_nodes`, `make_text_resource_node`, `prepare_text_layout`, and `add_text_run_layout` totals and call counts.
- `text_content_rebuilds`, `text_content_reused`, `text_clean_reuse_skips`, `text_resource_descriptor_reuses`, `text_cache_entries_replaced`, and `text_wrapper_order_rebuilds`.
- `qsg_nodes_created`, `qsg_nodes_destroyed`, `text_leaf_nodes_created`, and child-node clear counts.
- Dirty-row totals: published rows, coalesced rows, rendered dirty rows, and recomputed rows.

Correctness validation should include the stress demo plus focused cases for cursor block inversion, IME preedit, alternate screen, scrollback scroll, resize, selection spans, non-ASCII prompt glyphs, hard block graphics, and synchronized-output/public-projection paths.

## Files inspected

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_renderer.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\vnm_terminal_surface_render_bridge.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_ring.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_row_traversal.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_row_record_codec.cpp`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_change_governance.md`

## Report file

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_qsg_report.md`
