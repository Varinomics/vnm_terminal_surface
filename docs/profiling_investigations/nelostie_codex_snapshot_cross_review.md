# Nelostie Profile Cross-Review - Agent B

## Report-by-report assessment

### 1. `nelostie_codex_ingest_print_text_report.md`

Quality/usefulness: high.

The report is self-contained, evidence-backed, and correctly distinguishes direct profile evidence from source-based inference. Its central conclusion is confirmed: the ingest/apply path is dominated by `Terminal_screen_model::apply_action::print_text`, with 472,802 calls and 35.398 s total against 36.100 s in `apply_parser_actions`.

Strong points:

- Correctly identifies `print_text` as the primary GUI hot path, not parser ingestion or style mutation.
- Correctly connects the hot path to full-row copy and full-row comparison in `write_printable_ascii_span` and `advance_row_content_generation_if_changed`.
- Correctly treats dirty-row churn as secondary in directly measured scope time, while still identifying it as a structural amplifier inside hot mutation scopes.
- Provides validation that fits the risks: wide glyphs, wrapping, no-autowrap, hyperlink/style transitions, retained history, and synchronized output.

Limitations:

- The lower-bound 411.8 million copied/compared cells is a useful scaling estimate, but it assumes at least one full-row copy/compare per `print_text` call and should be framed as a lower-bound model, not measured work.
- The suggested dirty-row accumulator changes should remain behind the row-copy fix in priority because the profile does not show dirty publication as a direct time sink.

Reliability: strong enough to rely on for prioritizing the first GUI/model optimization batch.

### 2. `nelostie_codex_render_snapshot_dirty_rows_report.md`

Quality/usefulness: high.

The report is self-contained and accurately explains why dirty rows are not reducing snapshot publication cost. The profile evidence is confirmed: `Terminal_session::publish_render_snapshot` accounts for 8.487 s over 1,252 calls, and `Terminal_screen_model::render_snapshot::append_rows` accounts for 7.797 s of that.

Strong points:

- Correctly identifies `append_rows` as 99.6% of `Terminal_screen_model::render_snapshot` time.
- Correctly explains that dirty-row range computation is cheap: `render_snapshot::dirty_rows` is only 3.39 ms total over 1,252 calls.
- Correctly identifies that snapshot construction loops all visible rows and scans all columns before dirty range metadata can help the renderer.
- Correctly identifies that session/surface dirty-row coalescing happens after full snapshots have already been built.

Limitations:

- The row-cache/delta path is correctly high-benefit but high-risk; it should be governed as a design batch, not a quick local fix.
- Pre-publication coalescing is attractive, but the report appropriately notes that notification, transcript, selection lease, and sequence semantics must be audited first.

Reliability: strong enough to rely on for the snapshot-publication part of the final report.

### 3. `nelostie_codex_renderer_qsg_report.md`

Quality/usefulness: high.

The report is broad but well-grounded. It correctly separates GUI snapshot publication, render-frame construction, QSG text-resource synchronization, graphic layers, and retained history. The core render-thread numbers are confirmed: `VNM_TerminalSurface::updatePaintNode` is 19.748 s over 293 calls, `build_terminal_render_frame` is 8.368 s, and `sync_text_resource_nodes` is 10.547 s.

Strong points:

- Correctly identifies two full render-frame passes: `build_terminal_render_frame::cells` at 4.229 s and `build_terminal_render_frame::packed_data` at 4.100 s.
- Correctly identifies QSG text synchronization as the dominant render-thread child: 10.547 s, with 6.470 s in `make_text_resource_node`.
- Correctly notes that text layout has many small costs rather than isolated pathological calls: 516,835 `prepare_text_layout` and 516,835 `add_text_run_layout` calls.
- Correctly deprioritizes retained-history ring work for this profile; visible retained-history append work is roughly 14.6 ms.
- Provides the most useful render-thread backlog and validation counters.

Limitations:

- Some QSG recommendations are architectural and need staged ownership. Retained row-frame construction, row-slot stabilization, and text-resource in-place update should not be attempted in one batch.
- The report is less explicit than the snapshot report about which current cache paths are already reachable and which are gated, but it does not make the incorrect frame-key claim seen in the Claude scaling report.

Reliability: strong enough to rely on for render-thread and QSG prioritization.

### 4. `nelostie_claude_end_to_end_pipeline_report.md`

Quality/usefulness: low as a report artifact.

The requested file is only a short delivery summary, not a full investigation report. Its top-level numbers mostly match the profile and the Codex reports, but it does not provide enough evidence, source context, or validation detail to rely on independently.

Useful points:

- Correctly calls out `apply_action::print_text` at 35.4 s as the largest lever.
- Correctly calls out `render_snapshot::append_rows` at about 7.8 s.
- Correctly identifies snapshot production outpacing painted frames as a likely source of throw-away work.
- Correctly identifies high dirty-row write amplification and many `publish_pending_changes` calls.

Weaknesses:

- Too shallow to use as an authoritative report.
- Provides line references and root causes without enough quoted source evidence to audit from the artifact itself.
- Does not separate direct profile evidence from inference.
- Does not provide an actionable validation plan beyond a general test-gap statement.

Reliability: use only as a corroborating summary where it agrees with the detailed Codex reports. Do not rely on it independently.

### 5. `nelostie_claude_scaling_strategy_report.md`

Quality/usefulness: low for the requested file, mixed for the referenced full report.

The requested file is also a short delivery summary. It points to `docs/reviews/02_claude_scaling_strategy.md`, which I inspected to avoid judging the summary unfairly. The referenced full report contains useful evidence and a reasonable phased roadmap, but it also contains at least one material source-analysis error.

Useful points from the requested summary and referenced full report:

- Correctly identifies the same top-line measured costs: 35.4 s `print_text`, 7.8 s `append_rows`, 10.5 s `sync_text_resource_nodes`, and about 8.3 s in render-frame construction.
- Correctly deprioritizes retained-history ring work.
- Correctly calls out lack of scale/performance coverage as a test gap.
- Correctly recommends staged validation gates before fixes.

Material issue:

- The referenced full report claims `text_frame_cache_key` hashes the entire frame and that any dirty row mutates the frame key, making reuse fast paths unreachable. Source inspection does not support this. `text_frame_cache_key` includes font key, logical size, cell metrics, grid size, and active buffer. It does not hash text content or dirty rows. Dirty rows do not by themselves mutate the frame key.

Additional caveat:

- The claim that all three QSG text reuse paths are unreachable on partial-dirty frames is overstated. `clean_cache_skip` and descriptor reuse are gated by `same_text_frame_key`, but `key_match_reuse` can still reuse an old slot when the per-row resource key matches. The real issue is still row/text-resource churn under many dirty rows, but the mechanism should not be described as content dirtiness invalidating a frame-wide content key.

Reliability: the requested file is too shallow to rely on independently. The referenced full report is useful as a brainstorming and roadmap source after correcting the QSG frame-key diagnosis.

## Confirmed shared conclusions

### `print_text` is the largest measured GUI/model bottleneck

Confirmed evidence:

- `Terminal_screen_model::apply_action::print_text`: 472,802 calls, 35.398 s total, 74.868 us mean, 10.385 ms max.
- `Terminal_screen_model::apply_parser_actions`: 1,285 calls, 36.100 s total.
- `Terminal_screen_model::parser_ingest`: 1.801 s total.
- `Terminal_screen_model::apply_action::style_mutation`: 58.825 ms total.

Confirmed source mechanism:

- `write_printable_ascii_span` copies `screen_row.cells` into `before_cells`.
- `advance_row_content_generation_if_changed` calls `rows_have_same_selection_content`, which scans the row and compares cell selection content.
- On a 871-column grid, this makes small ASCII spans pay row-width work.

Consolidated conclusion:

The first model-side optimization should target row-copy/row-compare in the printable ASCII write path, with correctness guardrails around retained-line content generation, wide cells, combining marks, wrapping, style, hyperlink, and selection behavior.

### Snapshot publication builds full visible snapshots despite dirty-row metadata

Confirmed evidence:

- `Terminal_session::publish_render_snapshot`: 1,252 calls, 8.487 s total.
- `Terminal_screen_model::render_snapshot`: 1,252 calls, 7.831 s total.
- `Terminal_screen_model::render_snapshot::append_rows`: 1,252 calls, 7.797 s total.
- `Terminal_screen_model::render_snapshot::dirty_rows`: 1,252 calls, 3.39 ms total.

Confirmed source mechanism:

- `render_snapshot` computes dirty ranges, then `append_rows` loops all visible rows.
- `viewport_row_cells` returns row cells by value through `std::optional<std::vector<Cell>>` for both active and backing rows.
- Session and surface dirty-row coalescing happen after full snapshots are constructed.

Consolidated conclusion:

Dirty rows currently optimize downstream invalidation more than upstream publication. Snapshot construction still scales with `published_snapshots * rows * columns`.

### Render-thread frame construction duplicates full-cell work

Confirmed evidence:

- `build_terminal_render_frame`: 293 calls, 8.368 s total.
- `build_terminal_render_frame::cells`: 292 calls, 4.229 s total.
- `build_terminal_render_frame::packed_data`: 292 calls, 4.100 s total.

Consolidated conclusion:

The render frame path does two expensive full-cell passes. Merging packed-data generation into the main classification pass, or moving to retained per-row frame outputs, is a credible render-side target.

### QSG text resource synchronization is the largest render-thread subtree

Confirmed evidence:

- `sync_text_resource_nodes`: 293 calls, 10.547 s total.
- `make_text_resource_node`: 21,593 calls, 6.470 s total.
- `prepare_text_layout`: 516,835 calls, 2.201 s total.
- `add_text_run_layout`: 516,835 calls, 3.898 s total.
- Slow layout tracking recorded no >10 ms layout calls, so the problem is cumulative many-small-run work.

Consolidated conclusion:

The renderer should reduce rebuilt text resources and per-run text layout work. Row-level retention, descriptor/key reuse, row-slot stability, and direct consumption of packed/simple row spans are better targets than graphic-layer tuning.

### Retained-history ring is not the Nelostie bottleneck

Confirmed evidence:

- Visible retained-history append paths in the profile are roughly 14.6 ms across 458 appends.
- Lower-level ring operations are microsecond-scale totals relative to tens of seconds in GUI and render hot paths.

Consolidated conclusion:

Do not prioritize retained-history ring micro-optimizations for this workload.

### Dirty-row tracking is informative but not the primary direct time sink

Confirmed evidence:

- `mark_requests`: 7,875,373.
- `duplicate_mark_requests`: 7,224,579.
- `publish_pending_calls`: 991,700 dirty-row-stat calls.
- `Terminal_screen_model::publish_pending_changes`: 991,674 profiled calls, 106.8 ms total.
- `dirty_rows_snapshot_rows`: 55,889 over 1,298 calls, about 43.1 rows per snapshot.

Consolidated conclusion:

Dirty-row data structures and publication cadence are structural cleanup targets, but direct timing says they should follow the dominant row-copy, snapshot, and renderer work unless new sub-profiling changes that conclusion.

## Disagreements or weak claims

### Claude end-to-end summary is too shallow as evidence

The file gives plausible conclusions but no durable evidence trail. It should not be used as the final report source except where its numbers are independently corroborated by the profile or by the detailed Codex reports.

### Claude scaling summary is too shallow as the requested artifact

The requested file is a summary that points elsewhere. It is not a self-contained investigation report. The referenced full report is useful, but the final consolidated report should cite the verified profile/source evidence instead of relying on the summary.

### QSG frame-key diagnosis in the referenced Claude scaling report is materially wrong

The referenced full report states that `text_frame_cache_key` hashes the entire frame and that any dirty row mutates it. Source inspection shows the key contains only stable frame context: font key, logical size, cell metrics, grid size, and active buffer. It does not hash row text or dirty rows.

Correct framing:

- `same_text_frame_key` gates `clean_cache_skip` and descriptor reuse.
- Dirty rows do not by themselves make `same_text_frame_key` false.
- `key_match_reuse` can still reuse an old slot if the per-row resource key matches.
- QSG text churn is still real, but the final report should frame it as many dirty groups and expensive row text-resource rebuild/layout work, not as a content-dirty frame hash invalidating the whole graph.

### Snapshot throw-away work needs semantic validation before being called waste

The ratio of snapshot publications to painted frames is real: about 1,252 backend snapshot publications against 293 painted frames, with 13 additional snapshot publications in another path. It is reasonable to suspect superseded snapshots. However, the final report should not call all extra snapshots throw-away without auditing notification, transcript, selection visual lease, cursor, synchronized-output, and sequence semantics.

Correct framing:

- There is strong evidence of producer/consumer rate mismatch.
- Pre-publication coalescing is a promising optimization.
- Whether a given snapshot is safely coalescible is a contract question, not only a performance question.

### Dirty-row write amplification should not displace hotter work

The dirty-row counters are striking, but direct measured time for `publish_pending_changes`, `dirty_rows`, and `render_snapshot::dirty_rows` is small. Dirty marking inside `print_text` may still contribute to the hot scope, but the first fix should target row copy/compare unless new sub-scopes show dirty marking is unexpectedly large.

### Incremental snapshots are high risk

Multiple reports recommend incremental or delta snapshots. This is directionally sound, but the final report should avoid presenting it as a small change. Existing snapshot semantics treat absent cells as blank cells in consumers such as selection extraction. A delta contract requires explicit consumer-side state and fallback rules.

## Prioritized improvement backlog

### P0: Add targeted profiling counters before implementation

Benefit: medium. Risk: low.

Add counters/subscopes for:

- ASCII span row copy time.
- Row content-generation comparison time.
- ASCII cell write time.
- Dirty marking time inside text writes.
- Snapshot rows visited, rows materialized, cells scanned, cells emitted.
- Snapshot publications constructed, superseded before render, and rendered.
- Render-frame cells scanned in main pass and packed pass.
- Recomputed rows versus dirty rows.
- QSG text groups dirty/clean/reused/rebuilt with reason codes.
- Text-resource runs per row after coalescing.

Rationale:

The current profile identifies large scopes. The first implementation batch needs subscopes to prove the causal mechanism and prevent optimizing a plausible but non-dominant subpath.

### P1: Remove full-row copy/compare from printable ASCII span writes

Benefit: very high. Risk: medium.

Replace whole-row `before_cells` copy plus full-row comparison with range-based or write-time mutation detection. The implementation must preserve retained-line content generation semantics and invalidate retained lookup caches when selection-visible content changes.

Validation gates:

- `apply_action::print_text` total and mean time fall substantially.
- Existing text, wrapping, wide-cell, combining-mark, hyperlink, style, retained-history, selection, and synchronized-output tests remain correct.
- New targeted tests prove content generation changes only when selection-visible content changes.

### P2: Reduce render snapshot construction frequency where coalescing is contract-safe

Benefit: high under bursty backend output. Risk: medium to high.

Coalesce model results before full snapshot construction when no externally observable intermediate snapshot is required. This attacks the 1,252 snapshot publications versus 293 painted frames mismatch without changing the snapshot data contract first.

Validation gates:

- Snapshot construction count decreases under the stress profile.
- Rendered output, notification order, transcript capture, cursor behavior, selection leases, synchronized-output/public-projection behavior, and sequence semantics remain valid.
- New counters identify which snapshots were coalesced and why.

### P3: Merge render-frame packed-data construction with the main cell pass

Benefit: high to medium. Risk: medium.

`build_terminal_render_frame::cells` and `build_terminal_render_frame::packed_data` are near-equal costs over the same snapshot. Fuse packed span emission into the main classification pass or share row classification results.

Validation gates:

- `build_terminal_render_frame::packed_data` falls without equivalent growth in `build_terminal_render_frame::cells`.
- Packed text/graphic stats and visible output remain equivalent.
- Dirty-row and cursor/IME/selection affected rows remain correct.

### P4: Improve QSG text-resource reuse and reduce row text rebuilds

Benefit: high. Risk: medium.

Prioritize row-level reuse and rebuild avoidance over wholesale node-pool work. Specific candidates:

- Preserve/reuse clean row text resources when row content descriptors are unchanged.
- Stabilize viewport-row wrapper slots where possible to reduce reparenting churn.
- Avoid rebuilding dirty rows whose text descriptor is unchanged because only cursor/IME/overlay state changed.
- Consume compact row text spans directly when possible instead of creating and then coalescing large per-cell text-run lists.

Validation gates:

- `sync_text_resource_nodes`, `make_text_resource_node`, `prepare_text_layout`, and `add_text_run_layout` totals fall.
- `text_content_reused`, `text_clean_reuse_skips`, and descriptor/key reuse counts rise in partial-dirty scenarios.
- `text_cache_entries_replaced`, child-node clear counts, and QSG node churn fall.

### P5: Design an incremental snapshot or renderer-delta contract

Benefit: very high long term. Risk: high.

After P2-P4 clarify the render-side state model, design a governed internal delta path that materializes only changed rows under stable viewport row identity. Keep full snapshots for debug/transcript/public consumers unless and until those contracts are explicitly migrated.

Validation gates:

- A/B full snapshot versus delta reconstruction across transcript replay fixtures.
- Forced full-resync on viewport identity mismatch, resize, active-buffer transition, public projection mismatch, selection provenance mismatch, and memory pressure.
- Counters for delta rows, reused rows, full fallbacks, and fallback reasons.

### P6: Replace dirty-row `std::set` plumbing with a bounded dirty-row set type

Benefit: medium to low for this profile. Risk: low to medium.

Use a grid-bounded representation such as bitset plus sorted row list or generation-marked vector. This should be done after hotter work unless sub-profiling shows dirty-row marking is a larger hidden child of `print_text` than current aggregate scopes suggest.

Validation gates:

- Dirty-row vectors/ranges are identical before and after on replay fixtures.
- Stats such as `mark_requests`, `duplicate_mark_requests`, `published_unique_rows`, and `dirty_rows_snapshot_rows` remain semantically coherent.
- Microbenchmarks cover same-row, alternating-row, full-grid, and random-row dirty marking.

### P7: Consider cell representation changes only after copy-heavy paths shrink

Benefit: workload-dependent. Risk: high.

Inline ASCII storage or run-based row storage may become attractive, but it touches many contracts. Do not start here. Removing full-row copies and full-frame duplicate passes should reduce the pressure that makes `Cell` size painful.

## Validation plan

### Baseline capture

Use the existing Nelostie stress demo profile as the baseline and retain these headline metrics:

- `Terminal_screen_model::apply_action::print_text` total, mean, max.
- `Terminal_screen_model::apply_parser_actions` total.
- `Terminal_session::publish_render_snapshot` total and call count.
- `Terminal_screen_model::render_snapshot::append_rows` total, mean, max.
- `VNM_TerminalSurface::updatePaintNode` total, mean, max, frame count.
- `build_terminal_render_frame`, `build_terminal_render_frame::cells`, and `build_terminal_render_frame::packed_data` totals.
- `sync_text_resource_nodes`, `make_text_resource_node`, `prepare_text_layout`, and `add_text_run_layout` totals and call counts.
- Dirty-row stats: mark requests, duplicate marks, published unique rows, dirty snapshot rows, rendered dirty rows if available.
- QSG churn stats: text content rebuilt/reused, cache entries created/replaced/removed, child nodes cleared, wrapper order rebuilds.

### Correctness tests required before performance changes

Add focused cases for:

- Printable ASCII spans in wide grids.
- ASCII wrapping at the final column.
- No-autowrap clipping at the right edge.
- ASCII overwriting wide glyph starts and continuations.
- Combining marks adjacent to ASCII writes.
- Hyperlink and style transitions around short text runs.
- Cursor block inversion and cursor movement without text changes.
- IME preedit on clean and dirty rows.
- Scrollback, retained history, retained row identity, and selection extraction.
- Alternate screen transitions.
- Resize and grid reflow.
- Synchronized output hold/release and public projection paths.

### Performance regression tests

Add at least one scale fixture that reflects the stress shape rather than existing small-grid tests:

- 233 x 871 grid.
- Short ASCII text runs with frequent style changes.
- Partial dirty-row updates where most rows are clean.
- Full-dirty frames for worst-case validation.
- Producer faster than render consumer to test snapshot coalescing.

The tests should assert counters rather than brittle wall-clock thresholds where possible:

- Rows copied or materialized per snapshot.
- Cells scanned per snapshot/frame.
- Snapshot publications constructed versus rendered.
- Clean text rows reused on partial-dirty frames.
- Recomputed rows versus dirty rows.

### Per-batch validation gates

Each optimization batch should include:

- A before/after profile using the same Nelostie workload.
- A small deterministic fixture that proves the changed invariant.
- Counter comparison showing the intended scaling shape changed, not only elapsed time.
- No broad compatibility shims or parallel legacy paths unless explicitly governed.

## Recommended final-report framing

The final report should frame the issue as a pipeline-wide scaling mismatch:

- The workload is valid and intentionally large.
- The current architecture records dirty rows, but several upstream stages still do full-grid or full-frame work before dirty rows can help.
- The largest measured GUI problem is `print_text`, where small text writes pay row-width copy/compare costs.
- The second GUI problem is snapshot publication, where every publication materializes all visible rows even when average dirty snapshots are much smaller.
- The render-thread problem has two parts: full-cell render-frame construction, then QSG text-resource layout/rebuild churn.
- Retained history is not the lever for this profile.

Recommended phrasing for priority:

1. First prove and remove row-wide text-write costs in `print_text`.
2. Then reduce unnecessary full snapshot construction and/or coalesce before construction where the contract allows.
3. Then reduce render-thread duplicate cell passes and QSG text-row rebuild churn.
4. Treat incremental snapshots/render deltas as a governed architectural follow-up, not a quick local optimization.
5. Use dirty-row data-structure cleanup as supporting work after the dominant costs are addressed.

The final report should not say that dirty rows are ineffective. They are effective as renderer invalidation metadata. The accurate statement is that dirty rows are applied too late in the pipeline to prevent model-side full-row writes, full snapshot construction, and much render-frame scanning under this workload.

Files inspected for this cross-review:

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_ingest_print_text_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_render_snapshot_dirty_rows_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_qsg_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_claude_end_to_end_pipeline_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_claude_scaling_strategy_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\reviews\02_claude_scaling_strategy.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`

Report written:

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_snapshot_cross_review.md`
