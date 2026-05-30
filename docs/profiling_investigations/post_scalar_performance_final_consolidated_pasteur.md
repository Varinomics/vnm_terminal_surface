# Post-Scalar Performance Final Consolidation - Pasteur

## Executive summary

The post-scalar profile shows a decisive shift: the old printable-ASCII row-copy problem is no longer the primary bottleneck. The dominant current cost is the visual publication/render path, especially full render-frame construction on the render thread.

Recommended action path: implement a render-frame/QSG-local slice first.

The next implementation slice should remove dead/duplicated render-frame work before changing the snapshot contract. Specifically:

1. Stop building packed text sidecars in the hot QSG render path unless a real production consumer is enabled.
2. Collapse or bypass the second full packed-data pass where possible, starting with packed text removal and dirty-row mask precomputation.
3. Move cheap QSG text-cache skip checks before expensive descriptor/key construction.
4. Add miss/cost counters that prove which remaining text rows rebuild because of dirty content, row identity, cursor/IME, descriptor mismatch, or node-order churn.

This is the best first slice because it targets the largest measured parent scope, `build_terminal_render_frame` at 26.128 s, without immediately rewriting the canonical full-snapshot contract. Snapshot append is real and should be the next strategic slice, but it is more semantically invasive because snapshots carry selection, hyperlink, provenance, transcript/replay, public projection, and fallback meaning.

High-level strategy after the render slice: keep full snapshots as the correctness oracle and diagnostic/fallback contract, but add a retained visual row path only after the frame-builder work proves the remaining cost is still dominated by full-snapshot materialization.

## Measured bottleneck stack

Profile source: `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`.

Current workload shape:

- Surface: 235 rows x 873 columns, 205,155 visible cell positions.
- Render snapshots constructed: 1,498.
- Rendered frames: 662.
- Dirty-row snapshot rows: 14,379, averaging about 9.6 visible dirty rows per snapshot.
- Render-frame dirty rows: 11,096 across 662 rendered frames.
- Full-dirty render rows: 2,115, so full repaint exists but is not the normal case.
- Synchronized-output counters are zero, so DEC 2026 is not the driver in this capture.
- Text writes did not wrap or append scrollback in this capture.

Measured stack, ranked by current actionability:

| Rank | Area | Measured scope/counter | Cost | Why it matters |
| ---: | --- | --- | ---: | --- |
| 1 | Render-frame construction | `build_terminal_render_frame` | 26.128 s / 662 calls, 39.468 ms mean | Largest measured hot path. Two full-cell passes dominate render thread. |
| 1a | Main frame cell pass | `build_terminal_render_frame::cells` | 13.281 s | Full `snapshot.cells` classification and visual primitive emission. |
| 1b | Packed sidecar pass | `build_terminal_render_frame::packed_data` | 12.751 s | Second full pass with repeated classification; packed text appears renderer-unused. |
| 2 | QSG update | `Qsg_terminal_renderer::update_node` | 12.984 s | Downstream of frame construction; mostly text resource sync and graphic grouping. |
| 2a | QSG text resources | `sync_text_resource_nodes` | 8.044 s | Clean-row reuse works, but dirty rows still build many QSG text nodes/layout runs. |
| 2b | Graphic row layer | `sync_graphic_rect_row_layer` | 2.351 s | Significant because this workload has many block/graphic-classified single-width cells. |
| 3 | Snapshot materialization | `render_snapshot::append_rows` | 10.597 s / 1,498 calls, 7.074 ms mean | Full visible rows are materialized despite sparse dirty rows. |
| 4 | Surface sync | `VNM_TerminalSurface::sync_from_session` | 2.475 s / 1,771 calls | Needs subdivision before optimization; not top priority. |
| 5 | Model ingest | `Terminal_screen_model::ingest` | 1.829 s / 1,500 calls | No longer primary. `print_text` is 1.010 s. |

Renderer/frame counters show the duplication clearly:

- `frame_cell_pass_input_cells=55,837,192`.
- `frame_packed_pass_input_cells=55,837,192`.
- `frame_dirty_row_lookup_count=111,674,384`, effectively two per-cell dirty-row lookup sites.
- `frame_text_cells_rendered_as_text=32,952,136`.
- `frame_text_cells_rendered_as_graphic=22,885,056`.
- `text_runs_considered=32,952,136`.

QSG reuse is real but late:

- `text_groups_considered=93,517`.
- `text_groups_clean=83,853`.
- `text_clean_reuse_skips=83,853`.
- `text_groups_dirty=9,664`.
- `text_content_rebuilds=7,906`.
- `text_resource_descriptor_reuses=1,758`.
- `text_key_match_reuses=0`.
- `text_resource_runs_before_coalescing=4,378,068`.
- `text_resource_runs_after_coalescing=483,907`.
- `text_leaf_nodes_created=73,082`.
- `text_cache_entries_created=3,393`.
- `text_cache_entries_replaced=4,513`.
- `text_content_removed=3,230`.

Snapshot counters show why retained-row architecture remains important later:

- `render_snapshot_rows_visited=352,030`, exactly 1,498 x 235.
- `render_snapshot_rows_materialized=352,030`.
- `render_snapshot_cells_scanned=307,322,190`, exactly rows x columns over all snapshots.
- `render_snapshot_cells_emitted=134,005,265`.
- `render_snapshot_dirty_rows_visible=14,379`.
- `render_snapshot_full_repaint_fallbacks=0`.

## Report quality assessment

Overall quality is good. The reports converge on the same bottleneck stack: downstream full-grid work remains after scalar text mutation was improved.

Strongest reports:

- `codex_a_end_to_end`: Best balanced pipeline ranking. It correctly avoids treating model mutation as the remaining headline and identifies render-frame construction, snapshot append, and QSG text sync as the current stack.
- `codex_b_snapshot`: Best explanation of the snapshot contract and why dirty rows currently remain metadata rather than construction limits.
- `codex_c_qsg`: Best render-side map. It identifies the packed-data pass, QSG text-resource reuse policy, row identity, node churn, and the renderer-unused packed text sidecar issue.
- `codex_d_dirty_publication`: Best correction against over-focusing on dirty publisher churn. It shows pending dirty publication churn is noisy but not the dominant measured cost.
- `codex_e_architecture`: Best long-term architecture framing. It correctly recommends retained render rows before tile rendering or direct model-to-QSG diffs.
- `codex_f_workload`: Best workload calibration. It prevents misdirected work on synchronized output, scrollback append, line wrap, or general Unicode shaping.
- `claude_03_qsg_frame_cache`: Best specific QSG/frame findings. It adds useful points about dead packed text, eager descriptor construction, dead text key reuse, and reparent costs.

Useful but should be treated carefully:

- `claude_01_end_to_end_architecture`: Useful concrete findings, especially on full snapshot append and duplicated frame passes. Some computed waste language around snapshots should not be used as exact proof because `frames_published`, `snapshots_marked_rendered`, and `snapshots_superseded_before_render` are different concepts.
- `claude_02_snapshot_publication`: Good coalescing and publication-cadence analysis. Its strongest recommendation to move coalescing before snapshot construction is strategically aligned, but it still does not remove full snapshot construction unless paired with an incremental snapshot contract.
- `claude_04_unbiased_next_steps`: Useful for challenging assumptions, but some claims are outside this consolidation's accepted evidence, including working-tree comments and exact discarded-snapshot arithmetic. Use its questions, not its exact counts.

Important quality correction:

- Do not assert that `1,498 - 662` snapshots were necessarily wasted full snapshots. Rendered frames and snapshot publications are not one-to-one counters. The reliable measured facts are `render_snapshots_constructed=1,498`, `frames_published=662`, `snapshots_superseded_before_render=112`, `snapshots_marked_rendered=1453`, and `max_unrendered_snapshot_generations=11`. These prove producer/consumer imbalance exists, but not the larger exact waste number claimed in some reports.

## Confirmed root causes

1. The visual pipeline still performs full visible-grid work after dirty rows are known.

Dirty rows are tracked and published, but snapshot append and render-frame construction do not scale with dirty rows. Dirty metadata mostly helps after the frame has already been built.

2. `build_terminal_render_frame` is currently the top measured bottleneck.

The builder has two full-cell passes: the main cell pass and `packed_data`. Both process 55.837 M input cells in the profile. Together they cost 26.128 s, more than snapshot append and more than QSG text sync.

3. Packed text sidecars are high-probability dead work on the QSG hot path.

The inspected QSG renderer consumes packed graphic data through `packed_hard_graphic_rects`, but text rendering uses `frame.text_runs` and text resource runs. Packed text spans/bytes are built and counted but do not appear to drive production QSG text nodes. This makes packed text the most attractive first removal/gating target.

4. QSG clean-row reuse is working, but applied too late.

`text_clean_reuse_skips=83,853` proves the QSG layer avoids many clean-row text rebuilds. However, frame construction already paid full-cell classification, text-run construction, and packed-data work before those clean rows were skipped.

5. Dirty text resource rebuilds still create meaningful node/layout churn.

`make_text_resource_node` costs 4.320 s and `append_batched_text_run_nodes` costs 4.317 s. The renderer creates 73,082 text leaf nodes, replaces 4,513 text cache entries, creates 3,393 entries, and removes 3,230. This is not a single slow layout issue: slow text layouts over 10 ms are zero.

6. Snapshot append remains a strategic architecture problem.

`render_snapshot::append_rows` visits and materializes every visible row for each snapshot. It scans 307.322 M cells to publish 14,379 dirty rows of damage metadata. This cannot be solved fully by local render-frame cleanup, but it should be tackled after the lower-risk render slice.

7. Publication/dirty churn is not the primary measured cost.

`publish_pending_changes` is called very often, and dirty marks are 97.2 percent duplicates. But direct pending-publication cost is tiny compared with snapshot append and render-frame construction. Publication cadence matters mainly because each published snapshot triggers expensive full-state work.

8. The workload is a row-addressed large-grid TUI stress case.

It is not scrollback-stream dominated, not line-wrap dominated, not synchronized-output dominated, and not hyperlink/decorated/multi-width dominated. It is wide, mostly single-width, with phases of ASCII and block/graphic/non-ASCII content.

## Non-obvious findings

- Render-side cost now dominates enough that snapshot-only work would leave the biggest measured bottleneck intact.
- The packed-data pass is not a small metadata step. It is nearly as expensive as the main cell pass.
- Packed graphics appear useful, while packed text appears not to be consumed by QSG text rendering. Treat packed data as two different things, not one subsystem.
- `text_key_match_reuses=0` suggests the serialized text-resource key path is not paying for itself in this profile. Descriptor reuse is the actual identical-dirty-row reuse path.
- `text_resource_row_descriptor` is built for many clean rows that later take the clean skip. Its contents are mostly not needed for the clean skip decision.
- QSG reparent/order work is measurable (`sync_text_resource_nodes::reparent_slots=0.554 s`), but it is not the first bottleneck. Fix after the bigger full-pass and text-resource creation costs.
- Graphic geometry matters in this capture: 22.885 M cells render as graphics cumulatively, and graphic row sync costs 2.351 s. Do not design a text-only optimization that regresses block/box drawing phases.
- DEC synchronized output is not active in this profile. Optimizing DEC 2026 may be valuable generally, but it is not justified as the Nelostie scalar-span fix.
- Full dirty rows exist episodically, so any retained-row path must have cheap full fallback. The common case is sparse dirty rows, but the system must not become pathological on intentional full-screen invalidation.

## What not to do

- Do not start with tile rendering. The workload and renderer are row-oriented, and tile boundaries complicate text shaping, wide/combining cases, selections, hyperlinks, and cursor overlays.
- Do not start with a direct model-to-QSG diff protocol. It risks coupling renderer correctness to parser mutation order and weakens the current immutable oracle.
- Do not treat dirty-mark duplicate suppression as the main performance fix. It is a cleanup item, not the measured bottleneck.
- Do not focus on DEC synchronized-output batching for this profile. The synchronized counters are zero.
- Do not optimize scrollback append or line wrapping for this capture. Both relevant text-write counters are zero.
- Do not optimize general Unicode shaping first. The profile shows single-width cells and zero slow text layouts over 10 ms; the graphic/block-cell path is more relevant.
- Do not rewrite the snapshot contract before harvesting local render-frame wins. The snapshot contract carries broad correctness obligations and is higher risk.
- Do not rely on exact `snapshots - frames` arithmetic as a waste metric. Add better counters first if publication gating becomes the target.
- Do not remove full-snapshot diagnostics or replay capability. Keep full snapshots as oracle/fallback even if a retained visual path is introduced.

## Recommended next implementation slice

Slice name: render-frame hot-path de-duplication and QSG text skip cleanup.

Goal: Reduce render-thread cost without changing the public/full snapshot contract.

Scope:

1. Packed text hot-path gate/removal.

Stop producing `packed_text_spans` and `packed_text_bytes` in normal QSG render frames unless a real consumer is explicitly enabled. Keep packed graphic data for `packed_hard_graphic_rects`.

Expected effect: reduce a substantial part of `build_terminal_render_frame::packed_data`. This is the lowest-risk high-signal change because reports and tests indicate packed text sidecars do not affect QSG pixels today.

2. Dirty-row mask in frame construction.

Build a row dirty mask once per frame and pass it to both classification paths. This removes repeated linear dirty-range scans and prepares for pass fusion.

Expected effect: small-to-medium standalone win, useful instrumentation foundation.

3. Narrow packed-data work to packed graphics.

After packed text removal, remeasure `build_terminal_render_frame::packed_data`. If packed graphic work is still large, merge packed graphic classification/emission with the main `cells` pass or replace row-table bucketing with row-major linear iteration.

Expected effect: reduce the second full-cell pass without touching snapshot semantics.

4. Reorder QSG text clean-skip checks.

Move clean-row skip eligibility before full `Text_resource_row_descriptor` construction. Use a cheap eligibility predicate for cursor/preedit/clipping/provenance constraints, then build the descriptor only for dirty/stale rows that may compare or rebuild.

Expected effect: reduce the 1.060 s descriptor cost and lower heap churn.

5. Remove or instrument the dead text key path.

Because `text_key_match_reuses=0`, either remove `text_resource_key` from the dirty-row rebuild path or first add one profile counter that proves why it remains necessary. The default recommendation is removal after targeted tests confirm descriptor reuse covers the intended cases.

Expected effect: modest direct win, reduced complexity.

6. Add miss-reason counters while touching the path.

Record why a row rebuilds: no old slot, identity mismatch, dirty content, descriptor ineligible, descriptor mismatch, cursor/preedit, clipped text, key mismatch if retained, and row order changed.

Expected effect: no direct performance win, but necessary to avoid guessing on the later retained-row slice.

Exit criteria for the slice:

- `build_terminal_render_frame::packed_data` is materially reduced or converted to graphic-only work.
- `build_terminal_render_frame` total drops substantially below the current 26.128 s.
- QSG pixel tests remain equivalent for text, block graphics, cursor, IME, selection, and visual bell.
- `text_content_failures` stays zero.
- `paint_completed_frames` remains equal to `frames_published`.
- No increase in row-cache fallbacks.

## Alternative/later slices

1. Snapshot append constant-factor cleanup.

Before changing the contract, split `render_snapshot::append_rows` into subscopes: row lookup, row copy/materialization, cell append, hyperlink metadata, provenance. Then remove obvious duplicate materialization and row-copy costs.

Use this if the render-frame slice leaves GUI-thread snapshot append as the next dominant wall-clock limiter.

2. Publication cadence and producer/consumer gating.

Add exact counters first: snapshots constructed, snapshots delivered to surface, snapshots consumed by paint, snapshots superseded before paint, publish calls per drain, dirty rows merged before construction, and snapshot build skipped because a newer pending update replaced it.

Then consider end-of-drain publication batching or pre-snapshot dirty coalescing. Do not start here without better counters because current reports disagree on exact waste magnitude.

3. Retained visual rows.

Long-term strategic path: keep full snapshots as oracle/fallback, but add a visual update envelope with retained row payloads keyed by active buffer, logical row, retained line id, content generation, and style/color/geometry generations.

This should become the main architecture slice if, after render-frame cleanup, large-grid performance is still dominated by full snapshot append and full frame rows.

4. Row slabs.

If retained per-row overhead becomes too high, group rows into slabs. This is a later refinement, not the initial retained visual prototype.

5. Graphic-specific retained products.

If graphic phases remain expensive after packed-data cleanup, cache row-local graphic rect products or consume packed graphic spans directly in row sync to avoid repeated grouping/localization.

6. Surface sync bundling.

Subdivide `sync_from_session`, then bundle session state reads and avoid no-op property work. This is worthwhile, but lower priority than frame and snapshot work.

7. Dirty-mark/pending-publication cleanup.

Add a no-op fast path or per-ingest aggregation if easy, but do not expect it to move the top-line profile by itself.

## Validation and re-profile plan

Use the same scalar-span Nelostie profile scenario first.

Required pre-change or in-slice counters:

- Packed text cells/spans/bytes built.
- Packed graphic cells/spans/codepoints built.
- Dirty-row mask build cost.
- Dirty-row lookup count after mask conversion.
- Text descriptor builds by reason: clean skip candidate, dirty compare candidate, rebuild candidate.
- Text key builds and key-match reuse if key path remains.
- Text rebuild miss reasons.
- Text wrapper reparent count and moved-node count.
- Render-frame pass input cells after packed text removal.

Primary profile gates:

| Metric | Current | Expected after slice |
| --- | ---: | --- |
| `build_terminal_render_frame` | 26.128 s | Materially lower. |
| `build_terminal_render_frame::packed_data` | 12.751 s | Large drop if packed text is gated/removed. |
| `build_terminal_render_frame::cells` | 13.281 s | Same or small increase only if work is fused. |
| `Qsg_terminal_renderer::update_node` | 12.984 s | Same or lower. |
| `sync_text_resource_nodes` | 8.044 s | Lower if descriptor/key changes land. |
| `text_resource_row_descriptor` | 1.060 s | Lower. |
| `text_key_match_reuses` | 0 | Removed, or nonzero with proof. |
| `text_content_failures` | 0 | Must remain 0. |
| Slow text layouts over 10 ms | 0 | Must remain 0. |

Correctness gates:

- Existing QSG render tests for text, cursor, IME, selection, graphic rects/arcs, row cache, and lifecycle.
- Render-frame tests for packed graphic behavior, cursor/IME exclusion, simple content classification, and stats parity.
- Pixel parity for representative ASCII, block/box drawing, mixed style, and cursor-over-text frames.
- Dirty-row coverage tests remain unchanged: no skipped dirty pixels and no stale clean-row reuse.

Second re-profile after render slice:

- If `render_snapshot::append_rows` becomes top-ranked, start the retained visual row design.
- If `sync_text_resource_nodes` remains top-ranked, use the new miss-reason counters to decide between row identity split, descriptor reuse expansion, or text layout/run product caching.
- If graphic row sync becomes top-ranked, optimize packed graphic consumption and row-local graphic caches.

## Risks and open questions

- Packed text may have a non-obvious diagnostic or future consumer. The slice should verify production readers and tests before deleting fields. If removal is too broad, gate construction by option/purpose instead.
- Removing packed text can change stats. Tests or profile consumers may expect counters. Decide whether stats should report zero packed-text work or preserve a diagnostic mode.
- Merging packed graphic emission with the main cell pass risks parity drift around cursor/IME exclusions, block graphics, fallback text, and software scene graph behavior. Add parity tests before broad fusion.
- Moving descriptor construction later must preserve safety for cursor text, preedit caret, clipped runs, and invalid retained provenance. Use cheap eligibility checks, not blind clean reuse.
- Splitting row slot identity later is risky. Current text identity includes content generation, which is safe but churny. A stable-slot plus mutable-content identity can reduce churn but must not allow stale row content.
- Snapshot contract changes will affect selection, hyperlink metadata, transcript/replay, public projection, and validation. Treat retained visual rows as a sibling hot-path contract, not a replacement for full snapshots at first.
- Publication batching can break ordering around terminal replies, input-dependent modes, resize, synchronized-output release, backend error/exit, alternate-screen transitions, and transcript capture. Instrument before implementing.
- The workload is phase-dependent. Last-frame ASCII-heavy stats are not representative of cumulative graphic/non-ASCII phases.
- The current profile is from a profiling build. Some small costs, especially dirty-mark profiling counters, may be measurement artifacts.

## Source reports considered

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_a_end_to_end.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_b_snapshot.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_c_qsg.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_d_dirty_publication.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_e_architecture.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_f_workload.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_01_end_to_end_architecture.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_02_snapshot_publication.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_03_qsg_frame_cache.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_04_unbiased_next_steps.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
