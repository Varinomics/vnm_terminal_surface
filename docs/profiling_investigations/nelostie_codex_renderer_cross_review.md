# Nelostie Profile Cross-Review - Agent C

## Report-by-report assessment

| Report | Quality/usefulness assessment | Reliability for consolidation |
| --- | --- | --- |
| `nelostie_codex_ingest_print_text_report.md` | Strong report. It gives concrete profile totals, separates parser cost from apply cost, identifies `print_text` as the dominant GUI-side hot path, and ties the profile shape to source-level row-wide copy/compare work. The proposed first batch is practical and evidence-driven. | High. Use as the primary source for ingest/apply recommendations. |
| `nelostie_codex_render_snapshot_dirty_rows_report.md` | Strong report. It clearly distinguishes cheap dirty-row computation from expensive full visible-row snapshot construction, and it explains why existing dirty-row coalescing happens too late to reduce snapshot materialization cost. The action plan is appropriately cautious because delta snapshots affect contracts. | High. Use as the primary source for snapshot publication and dirty-row conclusions. |
| `nelostie_codex_renderer_qsg_report.md` | Strong report. It gives render-thread totals, identifies the split between `build_terminal_render_frame` and `sync_text_resource_nodes`, and connects text layout/node churn to source behavior. It also correctly down-ranks retained-history ring work based on measured totals. | High. Use as the primary source for QSG/render-thread conclusions. |
| `nelostie_claude_end_to_end_pipeline_report.md` | Too shallow to rely on as a standalone report. The file contains a useful high-level summary, but lacks detailed evidence tables, source excerpts, validation detail, and a complete backlog. Several numbers match the detailed Codex reports, so it is useful as corroboration, not as primary evidence. | Low to medium. Use only where corroborated by detailed reports or direct profile evidence. |
| `nelostie_claude_scaling_strategy_report.md` | Too shallow to rely on as a standalone report. The file says the report was written elsewhere and only provides a short top-line summary. It contains one important overstatement about the QSG text cache invalidating the entire text-node graph on any dirty row; the detailed renderer report and renderer stats show row-level reuse exists, although it is insufficient under this workload. | Low. Use for framing only, not for exact technical claims unless independently verified. |

Assessment notes:

- The three Codex reports are mutually consistent and collectively useful enough to drive planning.
- The two Claude artifacts are summary notes at the requested paths. They should not be cited as final evidence unless a final report explicitly labels them as corroborating summaries.
- The Claude scaling summary's test-gap claim may be directionally useful, but it was not substantiated in the provided artifact and should be validated before being presented as fact.

## Confirmed shared conclusions

Directly supported conclusions across the detailed reports:

- The stress workload is valid for this investigation. The recommendations should improve scaling under a large grid and high dirty-row pressure, not dismiss the workload.
- GUI-side ingest is dominated by text application, not parsing. `Terminal_screen_model::apply_action::print_text` is 35.398 s over 472,802 calls, about 98.1% of `apply_parser_actions` and about 92.8% of `Terminal_screen_model::ingest`.
- The dominant ingest source shape is row-wide work per ASCII span. The detailed ingest report ties the hot path to copying and comparing a full 871-cell row around printable ASCII writes.
- Snapshot publication is a separate major GUI cost. `Terminal_session::publish_render_snapshot` is 8.487 s over 1,252 calls, with `Terminal_screen_model::render_snapshot::append_rows` at 7.797 s.
- Dirty-row computation itself is not the expensive part in the captured profile. Dirty-row snapshot/compaction scopes are milliseconds total, while full snapshot row appending is seconds total.
- Snapshot production outpaces rendering. The reports consistently cite 1,252 backend snapshot publications versus 293 rendered frames, which means many full snapshots are built before the render thread consumes a frame.
- Render-thread work is also material. `VNM_TerminalSurface::updatePaintNode` is 19.748 s over 293 calls, with `build_terminal_render_frame` at 8.368 s and `Qsg_terminal_renderer::update_node` at 11.133 s.
- QSG text synchronization dominates renderer update time. `sync_text_resource_nodes` is 10.547 s, with `make_text_resource_node`, `prepare_text_layout`, and `add_text_run_layout` accounting for most text-node rebuild cost.
- Existing text coalescing is effective but not sufficient. It reduces 12.660 million text resource runs before coalescing to 516,863 after coalescing, yet the remaining layout/node work is still one of the top render-thread costs.
- Retained-history flat-ring work is not a material bottleneck in this profile. The detailed renderer report measured retained-history append paths at about 14.620 ms across 458 appends, far below the dominant GUI and render costs.
- The recurring architectural pattern is work scaling with full grid size, full row width, or full snapshot count before dirty-row information can reduce the work.

## Disagreements or weak claims

| Claim | Assessment | Correct framing |
| --- | --- | --- |
| Claude scaling: any dirty row invalidates the entire text-node graph because QSG text cache is keyed on a frame-wide hash. | Overstated. The renderer has row-level slots and measured reuse: cumulative stats include `text_content_reused=10519`, `text_clean_reuse_skips=6401`, and `text_resource_descriptor_reuses=4114`. | Row-level reuse exists, but high dirty-row counts, descriptor/key misses, wrapper reparenting, and text-node replacement still leave `sync_text_resource_nodes` as the dominant QSG cost. |
| Claude end-to-end: about 6.4 s of throw-away snapshot work. | Plausible derived estimate, not direct profile evidence. It depends on treating snapshots not consumed as wasted and multiplying by average publication cost. | Present as derived from 1,252 publications versus 293 rendered frames, not as a directly measured scope. |
| Claude end-to-end: 147x dirty-row write amplification. | Numerically plausible as `mark_requests / published_unique_rows`, but this mixes mark attempts with eventual published unique rows and does not directly measure time. | Use as a descriptive amplification ratio, not as proof that dirty-row storage is a primary time sink. |
| Codex ingest: lower-bound row copy/compare estimate. | Reasonable and explicitly labeled as source/profile inference. It depends on one or more ASCII spans per `print_text` call; if spans split on wraps/styles, actual row-wide work can be higher. | Keep the estimate, but validate with subscopes before implementing the optimization. |
| Codex snapshot: up to 254 million row/column visits in `append_rows`. | Reasonable as a scaling estimate from geometry and snapshot count. The exact emitted cell count can vary with occupancy, but the source still visits every column for materialized rows. | Use as a worst-case or upper-bound visit estimate, while relying on the direct 7.797 s profile total for priority. |
| Claude test-gap claim: many render tests but no stress-grid fixture. | Potentially important, but unsupported by the requested Claude artifact and not independently validated in this cross-review pass. | Include as a validation task: audit existing tests and add stress/perf fixtures if absent. |

## Prioritized improvement backlog

| Priority | Work item | Expected benefit | Risk | Rationale |
| ---: | --- | --- | --- | --- |
| 1 | Add targeted profiling counters before behavior changes. Counters should cover ASCII row snapshot copy, row comparison, cell writes, dirty marking, snapshot rows/cells materialized, superseded snapshots, recomputed render rows, text descriptor reuse misses, and post-coalescing row run counts. | Medium immediate, high enabling value. | Low if guarded by existing profiling controls. | The top costs are known, but subscopes are needed to validate causal fixes and avoid broad rewrites. |
| 2 | Remove row-wide copy/compare from the printable ASCII span path. Replace with range-based generation detection and then consider a true bulk ASCII span writer. | High. Targets the 35.398 s dominant GUI hot path. | Medium to high. Wide glyphs, combining marks, wrapping, hyperlinks, style metadata, and retained-line generation semantics are correctness-sensitive. | This is the largest measured cost and can be improved without first redesigning the render contract. |
| 3 | Coalesce snapshot publication before full snapshot construction where semantics allow. | High in bursty output. Reduces 1,252 full snapshot builds toward the 293 rendered frames when publications are superseded. | Medium to high. Snapshot-ready notifications, transcript capture, selection leases, synchronized output, cursor/bell behavior, and sequence semantics must be classified. | Existing coalescing happens after full snapshots are already built, so it cannot reduce `append_rows`. |
| 4 | Introduce row-cached or delta-aware render publication for stable viewport identity. | High. Makes snapshot publication scale closer to dirty rows instead of full visible grid size. | High. Touches snapshot semantics, retained provenance, hyperlinks, selection, public projection, resize, and renderer state. | This addresses the 7.797 s `append_rows` cost at its source, but needs governance as multi-batch architectural work. |
| 5 | Reduce render-thread full-frame duplication. Merge packed-data construction into the main frame cell pass or retain row-level frame outputs. | High. Targets 8.368 s in `build_terminal_render_frame`, especially the near-equal split between `cells` and `packed_data`. | Medium. Must preserve text/graphic routing, cursor/IME exclusion, styles, dirty classification, and packed payload correctness. | Current render-frame construction walks the large snapshot at least twice. |
| 6 | Stabilize QSG text row resources and reduce text-node rebuild churn. Use fixed viewport-row wrapper slots, improve dirty-but-descriptor-identical reuse, and consider span-based row text resources. | High. Targets 10.547 s in `sync_text_resource_nodes` and 6.470 s in `make_text_resource_node`. | Medium to high. QSG node ownership, row identity, scroll movement, cursor text, IME, and alternate-screen transitions are sensitive. | Existing coalescing helps but still leaves 516,835 layout operations each for prepare/add. |
| 7 | Optimize dirty-row storage and publication mechanics after larger costs are reduced. Consider bounded bitset/vector representations and coarser safe publication boundaries. | Low to medium for this profile. | Medium. Must preserve sorted unique rows and stable mutation identity semantics. | Dirty-row call counts are high, but measured dirty-row publication scopes are small compared with print, snapshot, and render costs. |
| 8 | Add stress-grid performance tests and regression gates. | High process value. | Low to medium depending on CI cost. | The profile exposes scaling behavior that ordinary small-grid correctness fixtures may miss. |

Recommended first implementation batch:

- Instrument the printable ASCII span path and snapshot/render row materialization counters.
- Use the new counters to confirm the row-wide copy/compare subcost inside `print_text`.
- Implement the range-based ASCII generation fix as the first causal performance change.

Recommended second implementation batch:

- Add pre-materialization snapshot coalescing for clearly coalescible backend output updates.
- Keep externally visible snapshot boundaries explicit and non-coalesced until their semantics are proven safe.

Recommended third implementation batch:

- Prototype retained row-level render state, covering both snapshot publication and render-frame construction.
- Use full-snapshot comparison as the oracle during development.

## Validation plan

Performance validation should recapture the same Nelostie stress profile after each batch and compare these metrics:

| Area | Metrics |
| --- | --- |
| Ingest/apply | `apply_action::print_text` total/mean/max, `apply_parser_actions` total, row snapshot copy count/time, row comparison count/time, ASCII cell write count/time. |
| Dirty rows | mark requests, duplicate marks, unique pending marks, published unique rows, dirty snapshot calls/rows, dirty range count, dirty-row storage timing. |
| Snapshot publication | `publish_render_snapshot` total/mean/max, `render_snapshot` total, `append_rows` total, rows visited, rows materialized, cells scanned, cells emitted, superseded snapshots. |
| Render-frame construction | `build_terminal_render_frame` total/mean/max, `cells` total, `packed_data` total, recomputed row count, reused row count, packed span counts. |
| QSG text | `sync_text_resource_nodes` total/mean/max, `make_text_resource_node`, `append_batched_text_run_nodes`, `prepare_text_layout`, `add_text_run_layout`, text rebuild/reuse/replacement counts, wrapper order rebuilds. |
| QSG churn | `qsg_nodes_created`, `qsg_nodes_destroyed`, `text_leaf_nodes_created`, child nodes cleared for replacement/removal, stale-entry removals, reparent slot time. |
| Retained history | Retained-history append total remains immaterial, and no optimization batch regresses it significantly. |

Correctness validation should include these cases:

| Case | Purpose |
| --- | --- |
| Printable ASCII in ordinary cells | Main optimized ingest path. |
| ASCII wrapping at the final column | Cursor movement, pending wrap, and dirty rows. |
| No-autowrap right-margin writes | Clipping and margin behavior. |
| ASCII overwriting wide glyph starts and continuations | Wide-cell cleanup and continuation integrity. |
| Combining marks around ASCII writes | Zero-width scalar attachment behavior. |
| Hyperlinks and style changes around short text runs | Cell metadata preservation. |
| Scroll, insert/delete lines, and retained history | Retained identity and content generation. |
| Selection spans and selected text extraction | Full-snapshot and delta equivalence. |
| Cursor block inversion and cursor blink | Text overlay and dirty row classification. |
| IME preedit | Text/resource reuse with preedit overlay rows. |
| Alternate screen | Active-buffer transitions and row identity. |
| Resize and geometry-derived snapshots | Full invalidation and resync behavior. |
| Synchronized output/public projection | Publication ordering and release semantics. |
| Non-ASCII prompt glyphs and hard block graphics | QSG text/graphic routing. |

Validation infrastructure recommendations:

- Add an A/B oracle that compares delta or cached snapshots against the existing full snapshot on transcript replay.
- Add forced full-resync paths for viewport identity mismatch, grid resize, active-buffer transition, public projection mismatch, and memory pressure.
- Add counters for fallback-to-full repaint frequency and treat unexpected fallback growth as a regression.
- Add at least one stress-grid fixture with dimensions close enough to the captured 233 x 871 geometry to expose row-width scaling.
- Keep final claims tied to before/after profile captures, not only microbenchmarks.

## Recommended final-report framing

The final report should frame the problem as an end-to-end scaling issue with three layers of grid-size work:

- Ingest layer: small text mutations pay row-width costs through full-row copy/compare in printable ASCII application.
- Snapshot layer: dirty-row metadata is produced, but full visible snapshots are still materialized too often and too early.
- Render layer: the render thread derives full-frame structures and rebuilds many QSG text resources even though row-level reuse and text coalescing already exist.

The final report should use the detailed Codex reports as primary evidence and the Claude reports only as corroborating summaries. It should explicitly avoid the unsupported framing that any dirty row invalidates the entire QSG text graph. A precise statement is stronger: row-level reuse exists, but the captured workload still causes enough dirty groups, descriptor/key misses, layout calls, replacements, removals, and reparenting to make `sync_text_resource_nodes` the dominant QSG cost.

The final report should not over-focus on retained-history flat-ring profiling. It should state that retained-history append is measured and currently immaterial for this profile, while keeping a small watch item for future regressions.

The final report should present a phased backlog rather than one large rewrite. The safest high-yield first move is instrumentation plus the printable ASCII row-generation fix. Snapshot coalescing and row-delta publication should follow as governed multi-batch work because they affect externally visible semantics. Render-frame row retention and QSG text-resource stabilization can proceed after the row identity and validation model is explicit.

Changed file:

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_cross_review.md`
