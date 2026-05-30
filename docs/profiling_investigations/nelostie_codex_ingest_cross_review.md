# Nelostie Profile Cross-Review - Agent A

## Report-by-report assessment

### 1. `nelostie_codex_ingest_print_text_report.md`

Quality: high. The report is focused, evidence-backed, and useful as the primary ingest/apply-path reference. It correctly treats the stress workload as valid and separates direct profile totals from source-based inference.

Most useful points:

| Point | Assessment |
| --- | --- |
| `print_text` dominance | Strong. The cited aggregate total, 35.398 s over 472,802 calls, accounts for 98.1% of `apply_parser_actions` and 92.8% of `Terminal_screen_model::ingest`. |
| Full-row copy/compare hypothesis | Strong. Source confirms `write_printable_ascii_span` copies `screen_row.cells` and then compares full rows through `advance_row_content_generation_if_changed`. |
| Action fragmentation as multiplier | Strong but secondary. The high `print_text`, style, control, and publication call counts explain why per-action row-width work explodes. |
| Dirty-row container work | Properly framed as secondary for the captured timing, despite high duplicate mark counts. |

Limitations:

| Limitation | Impact |
| --- | --- |
| It does not inspect render-thread/QSG costs. | Acceptable because it was scoped to ingest/apply. |
| It proposes bulk ASCII writing and dirty-row accumulator changes before subscopes exist. | Reasonable as options, but the first implementation batch should add targeted counters or a narrow row-diff removal before broader rewrites. |

Reliability: high. Use this report as the authoritative starting point for `apply_parser_actions` and `print_text` work.

### 2. `nelostie_codex_render_snapshot_dirty_rows_report.md`

Quality: high. This is the best focused explanation of why dirty-row metadata is not yet reducing snapshot construction cost.

Most useful points:

| Point | Assessment |
| --- | --- |
| `append_rows` dominance in snapshot publication | Strong. `render_snapshot::append_rows` is 7.797 s of 7.831 s in `render_snapshot`, and 91.9% of `publish_render_snapshot`. |
| Dirty-row computation is cheap | Strong. `render_snapshot::dirty_rows` and `dirty_rows()` totals are milliseconds, not seconds. |
| Full visible-row serialization despite dirty ranges | Verified. `render_snapshot` computes `dirty_row_ranges`, then loops every model row and calls `viewport_row_cells`. |
| Coalescing after full snapshot construction | Strong. The report correctly identifies that later coalescing cannot reduce `append_rows` cost. |

Limitations:

| Limitation | Impact |
| --- | --- |
| Row-cache/delta snapshot recommendations are architecturally large. | Correctly flagged as high risk, but not a first implementation step without stronger contract mapping. |
| It does not deeply evaluate session/public-projection observer semantics before recommending pre-publication coalescing. | Needs validation before implementation. |

Reliability: high for diagnosis, medium-high for the proposed redesign sequence.

### 3. `nelostie_codex_renderer_qsg_report.md`

Quality: high. This is the most complete render-thread/QSG report and is the most useful for final-report integration across GUI snapshot, frame construction, and QSG text-node work.

Most useful points:

| Point | Assessment |
| --- | --- |
| Render-thread split | Strong. It identifies `build_terminal_render_frame` at 8.368 s and `sync_text_resource_nodes` at 10.547 s over 293 frames. |
| Two full render-frame passes | Strong. The profile split between `build_terminal_render_frame::cells` at 4.229 s and `packed_data` at 4.100 s supports duplicated full-frame work. |
| QSG text layout is cumulative many-small-layout work | Strong. `prepare_text_layout` and `add_text_run_layout` each have 516,835 calls with no slow-layout outliers. |
| Retained-history ring is not the lever | Strong. The measured retained-history append work is about 14.620 ms, immaterial against GUI/render seconds. |
| Existing ASCII coalescing is useful but late | Strong. The report correctly notes coalescing is effective but does not avoid upstream per-cell/run costs. |

Limitations:

| Limitation | Impact |
| --- | --- |
| It sequences render-thread work before snapshot redesign. | Reasonable from implementation-risk perspective, but final planning should still keep GUI `append_rows` visible because it is 7.8 s and multiplied by publication count. |
| Some proposed retained-row renderer work overlaps with incremental snapshot design. | Needs a single owner design to avoid two state-retention systems drifting apart. |

Reliability: high. Use this report as the authoritative render-thread/QSG reference.

### 4. `nelostie_claude_end_to_end_pipeline_report.md`

Quality: low as a standalone report. The file at the requested path is a short summary, not a full investigation report. It contains useful headline numbers and source pointers, but it does not show enough direct evidence, inspected-file list, caveats, or validation detail to rely on independently.

Useful points:

| Point | Assessment |
| --- | --- |
| `print_text` at 35.4 s / 73% GUI wall | Consistent with profile and other reports. |
| `append_rows` at 7.8 s / 16% | Consistent with profile and other reports. |
| Producer-side snapshot overproduction estimate | Directionally useful: 1,252 backend snapshot publications versus 293 rendered frames is a real ratio. The exact `~6.4 s throw-away work` number is an estimate and should be presented as derived, not direct evidence. |
| Dirty-row write amplification | Directionally correct, but the summary overweights it relative to measured timing. |

Weaknesses:

| Weakness | Impact |
| --- | --- |
| Too shallow to audit. | Do not rely on it as final-report evidence unless the same claim appears in a deeper report or is independently verified. |
| The `147x dirty-row write amplification` phrase is ambiguous. | Prefer explicit counters: 7.875 M mark requests, 7.225 M duplicate mark requests, 53,632 published unique rows, 991,700 publish calls. |
| It does not distinguish direct profile measurements from source inference. | Final report should not inherit its compressed wording. |

Reliability: medium for headline alignment, low as a source of independent reasoning.

### 5. `nelostie_claude_scaling_strategy_report.md`

Quality: mixed. The file at the requested `profiling_investigations` path is only a pointer/summary, which is too shallow to rely on by itself. The linked `docs/reviews/02_claude_scaling_strategy.md` is substantive and useful, but it contains one overstatement about QSG text-cache reuse.

Useful points from the substantive linked report:

| Point | Assessment |
| --- | --- |
| High-severity row copy/diff finding | Strong. It matches the strongest confirmed ingest conclusion. |
| Full snapshot append finding | Strong. Source confirms full-row append regardless of dirty rows. |
| Render-frame double pass | Strong. Matches the Codex QSG report. |
| Retained-history de-prioritization | Strong. Matches measured totals. |
| Test gap around large grids | Useful and likely important. It is not fully verified here, but it is plausible and actionable. |

Weak or overstated points:

| Claim | Cross-review assessment |
| --- | --- |
| `text_frame_cache_key` makes all three text reuse fast paths unreachable when any dirty row changes the frame key. | Overstated. Source confirms `same_text_frame_key` gates `clean_cache_skip` and `text_resource_descriptor_reuse`, but `key_match_reuse` only checks the per-row old slot key after building the current row key. The global key still weakens clean-row reuse, but it does not make every reuse path unreachable. |
| Expected 4-8x `print_text` reduction from removing row diff. | Plausible but speculative. Present as hypothesis requiring subscopes and before/after profile validation. |
| `publish once at the end of ingest()` as a follow-up. | Potentially valuable, but too broad without proving mid-ingest publication boundaries are not observable through selection leases, snapshot sequence, synchronized-output release, transcript/public projection, or recovery logic. |
| ASCII inline `Cell` storage. | Potentially valuable but too large for early work. It touches broad representation and should be late, after eliminating avoidable row copies. |

Reliability: medium-high for diagnosis when read through the linked full report; low for the shallow file at the requested path alone. Final report can use its validated findings, but should correct the QSG reuse wording.

## Confirmed shared conclusions

| Conclusion | Confidence | Basis |
| --- | --- | --- |
| The profile is valid stress evidence and should not be dismissed because the grid is large. | High | All reports align; user instruction confirms. |
| The top GUI ingest/apply bottleneck is `Terminal_screen_model::apply_action::print_text`. | High | 35.398 s over 472,802 calls; 98.1% of `apply_parser_actions`. |
| The likely root cause inside `print_text` is row-width work per text span. | High | Source confirms full `std::vector<Cell>` copy and full-row comparison for ASCII spans. |
| Parser cost is not the bottleneck in this capture. | High | `parser_ingest` is 1.801 s versus 36.100 s in `apply_parser_actions`. |
| SGR/style mutation cost itself is not material. | High | 58.825 ms over 443,371 calls. Style/action fragmentation matters as a multiplier, not as direct SGR apply time. |
| Dirty-row metadata is collected but does not prune model snapshot construction. | High | `append_rows` loops every row after dirty ranges are computed. |
| `render_snapshot::append_rows` is a real GUI-side bottleneck. | High | 7.797 s over 1,252 calls, nearly all of `render_snapshot`. |
| Snapshot production exceeds rendered frame count. | High | About 1,252 backend render snapshot publications versus 293 rendered frames. |
| Render-thread frame construction is also grid-size driven. | High | `build_terminal_render_frame::cells` and `packed_data` total about 8.329 s. |
| QSG text resource synchronization/layout is the largest render-thread child cost. | High | `sync_text_resource_nodes` is 10.547 s; `make_text_resource_node` is 6.470 s. |
| Retained-history flat-ring work is not a material bottleneck for this profile. | High | Visible retained-history append work is about 15 ms total. |
| Existing tests are likely insufficient as performance/scaling guards. | Medium-high | Reports identify small-grid functional coverage and lack of stress-grid performance contracts. This should be verified before relying on exact test counts. |

## Disagreements or weak claims

| Claim or tension | Assessment | Final-report treatment |
| --- | --- | --- |
| Claude end-to-end report is a complete report. | False for the requested file. It is a terse summary. | Treat it as a useful note, not a relied-on report. |
| Claude scaling report at requested path is complete. | False for the requested file. The linked review is substantive. | Cite the linked content only if final process accepts following that pointer; otherwise mark the requested report shallow. |
| `text_frame_cache_key` invalidates all per-row QSG reuse paths. | Overstated. It gates two reuse shortcuts, but `key_match_reuse` remains reachable after per-row key construction. | Reframe as: global frame key prevents cheap clean-row/descriptor skip paths from firing when the frame key changes, forcing more per-row key/coalescing/layout work than necessary. |
| Dirty-row `std::set` publication is a top measured bottleneck. | Weak. The call count is huge, but direct scoped time is only 106.810 ms for `publish_pending_changes`, plus unscoped mark work inside mutations. | Keep as a secondary cleanup or enabling change after the row-copy and full-frame issues. Add subscopes before prioritizing it as a time win. |
| Producer-side coalescing can reclaim `~6.4 s throw-away work`. | Directionally plausible but not directly measured. | Present the publication/frame mismatch as direct evidence, then recommend counters for superseded snapshots before claiming savings. |
| Incremental snapshot/delta publication should be first. | Debatable. It has high payoff but high contract risk. | Put after instrumentation and more local print/render-frame fixes unless a design batch explicitly owns snapshot semantics. |
| Inline ASCII `Cell` storage should be early. | Too broad. It may help but touches core data representation. | Defer until avoidable row copies are removed and remaining allocation/copy profile is known. |
| `publish_pending_changes` can simply move to end of ingest. | Unproven. There are mutation identity, synchronized-output, recovery, selection, and publication-boundary concerns. | Treat as an investigation item with semantic audit, not an immediate fix. |

## Prioritized improvement backlog

### P0 - Add scaling validation and subscopes before risky redesigns

Expected benefit: enables safe prioritization and prevents regressions.

Actions:

| Action | Reason |
| --- | --- |
| Add profiling subscopes/counters inside `print_text` for row copy, row comparison, cell clear/write, dirty marking, and wrap splits. | Confirms exact share of the 35.398 s before changing contracts. |
| Add render publication counters: snapshots requested, snapshots constructed, snapshots superseded before render, rows materialized, cells scanned/emitted. | Separates unavoidable publication from coalescible waste. |
| Add render-frame counters: rows recomputed, rows reused, dirty-row lookup count, packed spans emitted, and packed pass input size. | Validates retained-row/fused-pass work. |
| Add QSG counters for reuse failure reasons, dirty-but-descriptor-identical rows, per-row post-coalescing run count distribution, and global-key-gated skips. | Turns the QSG cache diagnosis into measurable gates. |
| Add a large-grid stress fixture or replay gate based on the Nelostie profile dimensions. | Existing small-grid tests cannot catch grid-scaling regressions. |

### P1 - Remove row-wide copy/compare from ASCII `print_text`

Expected benefit: very high. This attacks the largest measured cost: 35.398 s.

Fix shape:

| Step | Notes |
| --- | --- |
| Replace before/after full-row copies with direct mutation detection for touched cells and affected wide-boundary cells. | Preserve retained-line content-generation semantics. |
| Add or reuse a bulk printable-ASCII span writer after the row-diff removal is proven. | Avoid per-cell general clear path where ASCII ordinary-cell writes can be handled by a span operation. |
| Keep wide glyph, combining mark, no-autowrap, hyperlink, style, and pending-wrap behavior covered by focused tests. | This is the main correctness risk. |

### P2 - Reduce full-frame render-thread duplication

Expected benefit: high. This targets about 8.329 s in `build_terminal_render_frame::cells` plus `packed_data`.

Fix shape:

| Step | Notes |
| --- | --- |
| First implement an O(1) per-row dirty-state vector for frame building if dirty lookup appears in hot subscopes. | Low-risk helper for later row retention. |
| Fuse packed-data construction into the main cell-classification pass where possible. | Removes the clearest duplicated full-cell pass without changing snapshot semantics. |
| Then consider retained row-frame outputs keyed by row identity/content generation. | Higher payoff, higher semantic surface. |

### P3 - Improve QSG text resource reuse and reduce text-node churn

Expected benefit: high. This targets 10.547 s in `sync_text_resource_nodes` and 6.470 s in text node creation.

Fix shape:

| Step | Notes |
| --- | --- |
| Split global text-frame cache context from per-row content keys. | Keep font/geometry/style-palette invalidation separate from row content identity. |
| Make clean-row and descriptor reuse reachable when only unrelated rows are dirty. | Corrects the real part of the Claude cache-key finding. |
| Stabilize viewport-row wrapper slots where possible. | Reduces reparenting and wrapper churn. |
| Delay in-place QSG leaf updates and glyph-run caching until row reuse is fixed. | Otherwise the optimization may be hidden by full-row rebuilds. |

### P4 - Reduce full snapshot construction or construct fewer snapshots

Expected benefit: high but riskier than P1/P2/P3.

Fix shape:

| Step | Notes |
| --- | --- |
| Audit publication boundaries and consumers before changing snapshot timing. | Must cover selection leases, public projection, transcript/capture, synchronized output, cursor/mode updates, and recovery logic. |
| Coalesce model results before snapshot construction only where boundaries are proven unobservable. | Targets the 1,252 snapshot publications versus 293 rendered frames. |
| If full snapshots remain required, add row-cache/delta publication under a governed design. | Avoid two independent cache systems drifting between model/session/render. |

### P5 - Dirty-row container and `Cell` representation cleanup

Expected benefit: low-medium until larger bottlenecks are removed.

Fix shape:

| Step | Notes |
| --- | --- |
| Replace dirty-row `std::set<int>` with a bounded dirty-row value type if profiling shows mark/publish work remains relevant. | Current direct publish scope is only 106.810 ms. |
| Consider inline ASCII cell storage only after row-copy elimination. | Otherwise representation work may be premature and broad. |
| Keep retained-history ring optimizations out of this backlog unless a new profile changes the evidence. | Current cost is immaterial. |

## Validation plan

### Performance validation

Use the same Nelostie stress workload as the main gate. Compare before/after profile captures with the same geometry and demo behavior.

Required metrics:

| Area | Metrics |
| --- | --- |
| Ingest/apply | `apply_action::print_text` total/mean/max, `apply_parser_actions` total, `parser_ingest` total, row-copy/row-compare/cell-write subscopes. |
| Dirty rows | mark requests, duplicate marks, unique pending marks, published unique rows, publish calls, dirty snapshot rows. |
| Snapshot publication | render snapshot requests, constructed snapshots, superseded snapshots, `append_rows` total/mean/max, materialized rows/cells. |
| Render-frame build | `build_terminal_render_frame`, `cells`, `packed_data`, recomputed rows, reused rows, packed spans/cells. |
| QSG text | `sync_text_resource_nodes`, `make_text_resource_node`, `prepare_text_layout`, `add_text_run_layout`, reuse counts, rebuild counts, replacement/removal counts, reparent counts. |
| End-to-end | GUI root total, render-thread `updatePaintNode` total/mean/max, rendered frame count, worst 100 ms bucket behavior. |

### Correctness validation

Required focused cases:

| Case | Purpose |
| --- | --- |
| Long printable ASCII line on 871-column grid. | Main optimized path. |
| Short text runs separated by SGR changes. | Fragmentation and style metadata. |
| Autowrap at final column and no-autowrap clipping. | Cursor/pending-wrap behavior. |
| ASCII overwriting wide glyph starts and continuations. | Wide-span cleanup. |
| Combining marks adjacent to ASCII writes. | Zero-width scalar attachment. |
| Hyperlink start/end around short text runs. | Cell hyperlink metadata. |
| Scroll, insert/delete lines, alternate screen, and resize. | Row identity and dirty-range semantics. |
| Selection leases and selected text across dirty/non-dirty rows. | Retained provenance/content generation. |
| Synchronized output release. | Publication boundary semantics. |
| Cursor block inversion, IME preedit, and visual bell. | Renderer overlay/text descriptor reuse safety. |

### Process validation

| Gate | Requirement |
| --- | --- |
| Baseline capture | Record current metrics before each implementation batch. |
| One lever per batch | Avoid combining row-diff removal, snapshot deltas, and QSG cache changes in one commit. |
| Independent review | Required for any snapshot/delta/cache contract change. |
| No silent fallback | If an incremental path cannot prove correctness, fall back loudly with counters, not silently with degraded semantics. |

## Recommended final-report framing

Recommended final framing:

1. State that the profile exposes a consistent scaling pattern: several hot paths scale with full grid size or full visible frame count even when dirty-row metadata is available.
2. Lead with the direct measured cost stack: `print_text` at 35.398 s, snapshot `append_rows` at 7.797 s, render-frame build at 8.368 s, and QSG text sync at 10.547 s.
3. Emphasize that parsing, SGR application, and retained-history ring work are not primary bottlenecks in this capture.
4. Present `print_text` row-wide copy/compare as the first high-confidence implementation target because it is both the largest measured cost and the most localized causal path.
5. Present snapshot publication and render/QSG work as a shared dirty-row scaling design problem, not three unrelated micro-optimizations.
6. Correct the Claude cache-key claim: the global text frame key blocks cheap clean-row/descriptor reuse when it changes, but not every per-row reuse path is unreachable.
7. Treat the Claude end-to-end report and the requested-path Claude scaling summary as shallow. Use them only where their claims are corroborated by the fuller Codex reports, the linked review, or direct source/profile verification.
8. Recommend a governed multi-batch plan: instrumentation and stress gates first, then local `print_text` fix, then render-frame/QSG reuse, then snapshot publication redesign if still justified.

Changed files for this cross-review:

| File | Change |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_ingest_cross_review.md` | Created cross-review report. |
