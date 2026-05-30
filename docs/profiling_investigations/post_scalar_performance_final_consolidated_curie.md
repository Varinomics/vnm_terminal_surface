# Post-Scalar Performance Final Consolidation - Curie

## Executive summary

The scalar-span profile shifted the bottleneck away from model text mutation and
toward the visual publication/render pipeline. The current top problem is not
parser ingest and not dirty-row bookkeeping. It is repeated full-grid/full-cell
work performed after the model already knows the dirty rows are sparse.

Current measured ranking from `nelostie_profile_scalar_span.txt`:

| Rank | Area | Measured cost | Calls | Mean |
| ---: | --- | ---: | ---: | ---: |
| 1 | Render-frame construction, `build_terminal_render_frame` | 26.128 s | 662 | 39.468 ms |
| 2 | Snapshot append, `render_snapshot::append_rows` | 10.597 s | 1,498 | 7.074 ms |
| 3 | QSG text sync, `sync_text_resource_nodes` | 8.044 s | 662 | 12.152 ms |
| 4 | Surface sync, `VNM_TerminalSurface::sync_from_session` | 2.475 s | 1,771 | 1.397 ms |
| 5 | Model ingest, `Terminal_screen_model::ingest` | 1.829 s | 1,500 | 1.220 ms |
| 6 | Printable text mutation, `apply_action::print_text` | 1.010 s | 83,349 | 12.122 us |

The decisive next implementation path should be a render-frame/QSG-local slice:
remove or gate renderer-unused packed text sidecars, then merge or reduce the
duplicate `cells` and `packed_data` full-cell passes. This targets the largest
measured cost first while avoiding the correctness blast radius of an immediate
snapshot/delta contract change.

The strategic direction remains row-retained visual state. Snapshot append is a
real bottleneck, and the long-term architecture should stop building full
visible cell payloads for sparse dirty-row updates. But the next implementation
slice should prove and harvest the lower-risk render-frame duplication first,
then use the reprofiled result to decide whether to proceed with snapshot row
indexing, retained rows/slabs, or publication throttling.

## Measured bottleneck stack

### 1. Render-frame construction

Measured:

| Scope | Total | Calls | Mean |
| --- | ---: | ---: | ---: |
| `build_terminal_render_frame` | 26.128 s | 662 | 39.468 ms |
| `build_terminal_render_frame::cells` | 13.281 s | 661 | 20.092 ms |
| `build_terminal_render_frame::packed_data` | 12.751 s | 661 | 19.290 ms |

The profile reports `frame_cell_pass_input_cells=55,837,192` and
`frame_packed_pass_input_cells=55,837,192`. The two dominant frame-builder
children process the same cell count. Reports C and Claude 03 independently
identify this as the largest current render-thread cost.

This is the current first target.

### 2. Snapshot construction and publication

Measured:

| Scope / counter | Value |
| --- | ---: |
| `Terminal_screen_model::render_snapshot` | 10.635 s over 1,498 calls |
| `Terminal_screen_model::render_snapshot::append_rows` | 10.597 s over 1,498 calls |
| `render_snapshot_rows_visited` | 352,030 |
| `render_snapshot_rows_materialized` | 352,030 |
| `render_snapshot_cells_scanned` | 307,322,190 |
| `render_snapshot_dirty_rows_visible` | 14,379 |
| `render_snapshot_full_repaint_fallbacks` | 0 |

The rows visited equal `1,498 * 235`, and the cells scanned equal
`1,498 * 235 * 873`. Snapshot construction is full-visible-grid work despite an
average dirty payload of about 9.6 rows per snapshot.

This is the next strategic target after the render-frame slice.

### 3. QSG text synchronization

Measured:

| Scope / counter | Value |
| --- | ---: |
| `sync_text_resource_nodes` | 8.044 s over 662 calls |
| `make_text_resource_node` | 4.320 s over 7,906 calls |
| `prepare_text_layout` | 1.400 s over 483,907 calls |
| `add_text_run_layout` | 2.670 s over 483,907 calls |
| `text_resource_row_descriptor` | 1.060 s over 93,517 calls |
| `text_content_reused` | 85,611 |
| `text_clean_reuse_skips` | 83,853 |
| `text_resource_runs_before_coalescing` | 4,378,068 |
| `text_resource_runs_after_coalescing` | 483,907 |

The QSG cache is not completely broken. Clean-row reuse and ASCII coalescing are
working. Remaining cost is dirty-row rebuilds, many small text-layout additions,
text-resource creation, descriptor work, stale removal, and slot reparenting.

### 4. Graphic row-layer work

Measured:

| Scope / counter | Value |
| --- | ---: |
| `sync_graphic_rect_row_layer` | 2.351 s |
| `packed_hard_graphic_rects` | 884.900 ms |
| `frame_text_cells_rendered_as_graphic` | 22,885,056 |
| `rect_resource_rects_before_coalescing` | 22,883,676 |
| `rect_resource_rects_after_coalescing` | 313,879 |

This is smaller than QSG text but relevant because the scalar-span workload has
a substantial single-width graphic/block-cell phase.

### 5. Dirty publication and scheduling

Measured:

| Counter | Value |
| --- | ---: |
| Dirty mark requests | 4,380,137 |
| Duplicate dirty mark requests | 4,258,550 |
| `publish_pending_calls` | 174,953 |
| `published_unique_rows` | 14,379 |
| Snapshot publications | 1,498 |
| Rendered frames | 662 |
| `snapshots_superseded_before_render` | 112 |
| `dirty_coalescing_applied` | 45 |
| `max_unrendered_snapshot_generations` | 11 |

Dirty-row and pending-publication churn is real, but the direct measured cost of
`publish_pending_changes` is tiny relative to snapshot append and frame building.
Publication cadence is a secondary scheduling problem, not the primary root of
the current runtime.

### 6. Model mutation

Measured:

| Scope / counter | Value |
| --- | ---: |
| `Terminal_screen_model::ingest` | 1.829 s |
| `Terminal_screen_model::parser_ingest` | 470.349 ms |
| `Terminal_screen_model::apply_parser_actions` | 1.216 s |
| `apply_action::print_text` | 1.010 s |
| `printable_ascii_row_copies` | 0 |
| `row_content_generation_comparison_cells` | 1,083,393 |

The old `print_text` row-copy hotspot is not the current dominant problem.
Further mutation work should wait until downstream full-grid costs are reduced
or a new profile reorders the stack.

## Report quality assessment

### Strongest reports

`post_scalar_performance_audit_codex_a_end_to_end.md` is the best broad map. It
correctly reprioritizes the current scalar-span profile and avoids treating
snapshot append as the only issue.

`post_scalar_performance_audit_codex_c_qsg.md` is the strongest implementation
lead. It identifies the largest current cost, the duplicated cell/packed-data
passes, the packed-text sidecar issue, the eager text descriptor cost, and the
row-cache/QSG churn details.

`post_scalar_performance_audit_codex_b_snapshot.md` is the best snapshot
contract audit. It makes the full-grid append problem concrete and separates
full-snapshot semantics from dirty-row metadata.

`post_scalar_performance_audit_codex_d_dirty_publication.md` is valuable because
it prevents overcorrecting toward dirty-row bookkeeping. It shows publication
churn is noisy but not the primary measured cost.

`post_scalar_performance_audit_codex_f_workload.md` is important for scope. It
shows Nelostie is a cursor-addressed full-screen workload with no synchronized
output, no text-write scrollback appends, and no text-write line wrapping in this
capture.

### Useful but more speculative reports

`post_scalar_performance_audit_codex_e_architecture.md` is directionally right:
full snapshots should remain a correctness oracle, while the hot visual path
needs retained row/slab state. Its recommendation is strategic, not the next
smallest implementation slice.

`post_scalar_performance_audit_claude_03_qsg_frame_cache.md` has strong QSG
observations and independently supports the packed-text/duplicate-pass direction.
Some claims should be converted into validation checks rather than accepted as
final design constraints.

`post_scalar_performance_audit_claude_02_snapshot_publication.md` gives useful
snapshot tests and coalescing concerns, but several recommendations jump toward
snapshot semantics before cheaper render-frame duplication has been harvested.

`post_scalar_performance_audit_claude_04_unbiased_next_steps.md` is useful as a
challenge document. It overstates some publication-waste math and includes
claims that need verification before use, but its bias toward measuring
consumer/producer mismatch is useful.

`post_scalar_performance_audit_claude_01_end_to_end_architecture.md` is a good
corroborating summary, but its final recommendation is less precise than the
Codex QSG/snapshot reports.

## Confirmed root causes

### Full snapshot construction ignores sparse dirty rows

Dirty rows are correctly measured and carried as metadata, but
`render_snapshot::append_rows` still visits and materializes every visible row.
This is measured by exact row/cell counters and confirmed by the snapshot audit.

### Render-frame construction duplicates full-cell work

The renderer builds frame text/graphics/decorations in `cells`, then builds
packed rows/spans in `packed_data`. Both passes process 55.837 M input cells.
This is the largest measured cost and the clearest next implementation target.

### Dirty-row reuse happens too late

QSG clean-row reuse avoids many node rebuilds, but only after the model has
built a full snapshot and `build_terminal_render_frame` has walked a full cell
payload. The architecture has retained state at the model and renderer ends, but
a full-frame value-object boundary in the middle.

### QSG text sync is improved but still rebuild-heavy on dirty rows

Clean-row reuse and ASCII coalescing are effective. Remaining text cost comes
from dirty text resource creation/replacement, 483,907 text-layout additions,
row descriptor work, stale entry deletion, and wrapper reparenting.

### The workload is not DEC synchronized-output or scrollback dominated

All synchronized dirty-row collect/release counters are zero. Text-write line
wraps and scrollback appends are zero. Optimizations focused on DEC 2026,
scrollback append, or line wrapping are not justified by this capture.

### Publication cadence is a secondary amplifier

There are more snapshots than painted frames and some supersession, but the
measured superseded count does not explain most runtime. Scheduling/coalescing
work matters after the per-snapshot/per-frame cost is reduced or instrumented
more precisely.

## Non-obvious findings

### Packed text sidecars appear to be a high-value local target

Multiple reports found that packed graphic spans are consumed by the graphic
path, while packed text spans/bytes are not consumed by the QSG text path in the
inspected renderer. Existing tests reportedly clear packed text sidecars and
expect visual parity. This is the best low-risk entry point into the 12.751 s
`packed_data` cost, subject to a final grep/test confirmation when implementing.

### The QSG text cache is not wholesale-invalidating every frame

The current counters show 85,611 text content reuses and 83,853 clean cache
skips. The problem is not that any dirty row invalidates the whole text graph.
The problem is that clean-row reuse happens after full frame construction, while
dirty rows still cause expensive resource rebuilds.

### `text_resource_key` may be dead weight

Claude 03 reports `text_key_match_reuses=0` while descriptor reuse catches 1,758
identical dirty rows. This suggests the byte-serialized text resource key may add
cost without value in this profile. Treat this as a follow-up validation, not the
first slice.

### Eager text descriptors burn time before clean-row skip

`text_resource_row_descriptor` costs 1.060 s over 93,517 calls. Reports agree
that clean rows often only need a cheap eligibility check, not full descriptor
construction. This is a contained QSG optimization after the render-frame pass
work.

### Graphic/block content matters in this scalar profile

The workload has 22.885 M cells rendered as graphics and 24.095 M non-ASCII
cells, all single-width. Do not optimize only ASCII text. Any retained row/frame
strategy must preserve fast single-width graphic classification and graphic row
reuse.

### Max frame outliers are not individual slow text layouts

`slow_text_layouts` reports zero calls over 10 ms, while `updatePaintNode` maxes
near one second. Outliers likely come from aggregate node churn, removal,
reparenting, or layer work rather than one pathological `QTextLayout` call.

## What not to do

Do not start by optimizing `Terminal_screen_model::apply_action::print_text`.
The scalar-span profile shows this is no longer the main cost.

Do not treat dirty-row set insertion or `publish_pending_changes` as the primary
problem. The counts are ugly, but direct measured cost is small.

Do not build a DEC synchronized-output-specific solution for this capture. The
profile did not exercise synchronized-output collection or release.

Do not optimize scrollback append or line-wrap paths for Nelostie until a new
profile shows them active.

Do not jump straight to tile rendering. Terminal semantics and Qt text shaping
are row-oriented; tiles are higher risk and not indicated before retained rows or
row slabs are tried.

Do not remove or weaken full `Terminal_render_snapshot` as a correctness object.
Keep it for tests, replay, diagnostics, selection, and fallback while a retained
visual path is developed.

Do not rely on throttling alone. It may improve burst behavior but leaves the
final rendered frame expensive.

Do not accept report claims about WIP diffs, row-copy regressions, lock behavior,
or exact discarded-snapshot counts without validating them in the implementation
branch. They are useful leads, not final evidence.

## Recommended next implementation slice

### Chosen slice: render-frame packed-data reduction

Implement the first action slice in the render-frame/QSG layer, not in snapshot
semantics.

Goal:

Reduce `build_terminal_render_frame::packed_data` and total
`build_terminal_render_frame` time by removing renderer-unused packed text work
and preparing for a single-pass frame builder.

Why this first:

- It targets the largest measured bottleneck: 26.128 s in frame construction.
- It has a narrower correctness surface than dirty-row-limited snapshots.
- It can be validated with render-frame/QSG tests and pixel parity.
- It preserves the full snapshot contract while reducing current runtime.
- It gives a cleaner profile before making larger retained-row architecture
  decisions.

Concrete implementation shape:

1. Confirm production readers for `Terminal_render_frame::packed_text_spans` and
   `packed_text_bytes`.

2. If no production renderer reader exists, gate or remove packed text span/byte
   emission from normal QSG frame construction. Keep packed rows and packed
   graphic spans if `packed_hard_graphic_rects` still needs them.

3. Add or update tests so the intended contract is explicit:

| Test area | Required assertion |
| --- | --- |
| QSG pixel output | Rendering is unchanged with packed text sidecars absent. |
| Render-frame stats | Packed graphic stats remain correct. |
| Text rendering | QSG text still comes from `frame.text_runs` or the chosen replacement. |
| Debug/sidecar behavior | Any non-render consumer of packed text is gated explicitly. |

4. Reprofile before attempting the second half of the slice.

5. If packed text removal does not materially reduce `packed_data`, merge packed
graphic emission into the main `cells` pass or add a single shared classification
record so `classify_terminal_simple_content_cell`, style lookup, cursor/IME
filtering, and dirty lookup are not repeated.

Expected result:

`build_terminal_render_frame::packed_data` should fall substantially. The exact
reduction depends on how much of `packed_data` is text sidecar work versus row
bucketing, graphic packing, and duplicated classification. If `packed_data` does
not move, the profile will prove that the next step must be full pass fusion
rather than sidecar removal.

Out of scope for this first slice:

- Dirty-row-limited snapshots.
- Retained row/slab visual contract.
- Backend publication throttling.
- General QSG row identity redesign.

## Alternative/later slices

### Slice 2: render-frame pass fusion and dirty-row mask

After packed text work, merge `cells` and remaining `packed_data` work or share
classification outputs. Add a per-frame dirty-row mask so dirty checks are O(1)
and not repeated through range scans.

Validation target:

`build_terminal_render_frame::cells + build_terminal_render_frame::packed_data`
should move toward one full-cell pass rather than two.

### Slice 3: snapshot append subprofiling and constant-factor cleanup

Subdivide `append_rows` into row resolution, row cell materialization/copy,
hyperlink metadata, provenance, and retained-history lookup. Then fix measured
constant-factor costs such as duplicated retained-row materialization or row
vector copies if they are present in the implementation branch.

Validation target:

`render_snapshot::append_rows` decreases without changing full-snapshot output.

### Slice 4: row-indexed snapshot layout

Add row spans or row records to `Terminal_render_snapshot` so downstream code can
consume row-local payloads without repeatedly re-bucketing flat cells. This is a
bridge between current full snapshots and retained visual rows.

Validation target:

Remove `build_explicit_snapshot_row_table` and reduce row grouping work without
changing snapshot semantics.

### Slice 5: retained visual rows or row slabs

Promote row identity/content generation into a hot-path visual contract. Keep
full snapshots for diagnostics and fallback, but publish or retain row payloads
so clean rows are referenced rather than rebuilt.

Validation target:

For a stable viewport with one dirty row, model publication and frame
construction scale with dirty rows plus overlay rows, not with visible rows times
columns.

### Slice 6: QSG text cache cleanup

After frame duplication is reduced, tackle eager descriptor construction,
`text_resource_key` dead weight, in-place dirty row replacement, stale entry
cleanup, and wrapper reparenting.

Validation target:

`sync_text_resource_nodes`, `make_text_resource_node`, text layout call count,
text cache replacements/removals, and reparent time decrease in partial-dirty
frames.

### Slice 7: publication cadence and burst coalescing

Instrument and then reduce render snapshot publication cadence only where
ordering barriers allow it. This should follow cheaper per-frame/per-snapshot
work unless a new profile shows publication mismatch has become dominant.

Validation target:

Snapshot publications per rendered frame and max unrendered generations decrease
without lost transcript, selection, synchronized-output, geometry, or public
projection semantics.

## Validation and re-profile plan

### Baseline preservation

Use `nelostie_profile_scalar_span.txt` as the immediate baseline. Preserve these
headline values in the implementation report:

| Metric | Baseline |
| --- | ---: |
| `build_terminal_render_frame` | 26.128 s |
| `build_terminal_render_frame::cells` | 13.281 s |
| `build_terminal_render_frame::packed_data` | 12.751 s |
| `render_snapshot::append_rows` | 10.597 s |
| `sync_text_resource_nodes` | 8.044 s |
| `Qsg_terminal_renderer::update_node` | 12.984 s |
| `updatePaintNode` | 39.623 s |
| `frame_cell_pass_input_cells` | 55,837,192 |
| `frame_packed_pass_input_cells` | 55,837,192 |
| `text_resource_runs_after_coalescing` | 483,907 |
| `text_content_rebuilds` | 7,906 |
| `slow_text_layouts` over 10 ms | 0 |

### Pre-implementation checks for the chosen slice

1. Search production readers of packed text sidecars.
2. Identify tests that intentionally clear packed text sidecars and confirm their
   contract.
3. Add temporary counters if needed:

| Counter | Purpose |
| --- | --- |
| Packed text cells considered | Quantifies sidecar input. |
| Packed text UTF-8 bytes emitted | Quantifies avoided byte work. |
| Packed graphic cells considered | Separates remaining necessary packed work. |
| Packed pass row-table build time | Separates bucketing/sorting from emission. |

### After implementation

Run the focused render/QSG test set and then reprofile the same Nelostie scalar
workload.

Expected profile movements:

| Metric | Expected direction |
| --- | --- |
| `build_terminal_render_frame::packed_data` | Down substantially. |
| `build_terminal_render_frame` | Down. |
| `build_terminal_render_frame::cells` | No significant increase. |
| `Qsg_terminal_renderer::update_node` | No regression. |
| `sync_text_resource_nodes` | No regression from sidecar removal. |
| `frame_packed_graphic_cells` | Semantically equivalent. |
| QSG pixel/render tests | Equivalent. |
| `text_content_failures` | Remains zero. |
| Slow text layouts | Remains zero over threshold. |

### If the chosen slice underperforms

If `packed_data` stays high after packed text removal, do not pivot to snapshot
deltas immediately. The next action should be explicit pass fusion or a shared
classification record, because the evidence would show row-table construction,
duplicated dirty lookup, style lookup, or graphic packing are the real
`packed_data` costs.

### Reprofile after each slice

Do not stack render-frame pass fusion, snapshot append changes, and QSG cache
policy changes in one profile. Reprofile after each slice so the next bottleneck
ranking is real.

## Risks and open questions

### Packed text sidecar ownership

Open question:

Is packed text used by non-QSG tooling, debug output, tests, or planned external
interfaces? Reports found no production QSG consumer, but implementation must
confirm before deletion.

Risk:

Removing sidecars without an explicit contract could break hidden consumers or
future tests. Prefer gating if ownership is unclear.

### Snapshot contract migration risk

Open question:

What is the smallest row-indexed or retained-row contract that preserves
selection, transcript, public projection, hyperlink metadata, cursor/IME, and
geometry-derived snapshots?

Risk:

Dirty-row-limited snapshots turn underreported dirty rows into stale rendering.
Keep full snapshots as fallback and oracle.

### Publication semantics

Open question:

Which snapshot publications are externally observable through transcript,
selection leases, synchronized-output boundaries, geometry changes, public
projection, cursor/mode transitions, or notifications?

Risk:

Naive throttling can skip observable state even if the rendered pixels would
look acceptable.

### QSG identity policy

Open question:

Should text row slot ownership be keyed separately from content generation so
dirty rows replace resources in place instead of create/remove/reparent cycles?

Risk:

A too-stable key can reuse stale text resources. A too-fresh key creates churn.
This needs miss-reason counters.

### Workload representativeness

Open question:

Is Nelostie always row-addressed with no DEC synchronized output, or only in this
capture?

Risk:

A later mode may exercise synchronized output, alternate screen, scrollback, or
multi-width text differently. Add terminal-pattern histograms before designing
workload-specific fast paths.

### Measurement ambiguity

Open question:

How much of `append_rows` is row copy/materialization, hyperlink metadata,
provenance, retained-history lookup, allocator churn, or cell scanning?

Risk:

Jumping to retained rows before subprofiling could solve the right asymptotic
problem but miss a cheaper constant-factor win.

## Source reports considered

| Report | Final use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_a_end_to_end.md` | Primary end-to-end ranking and pipeline map. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_b_snapshot.md` | Snapshot contract, append cost, and publication/coalescing analysis. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_c_qsg.md` | Primary basis for chosen next slice: render-frame/QSG packed-data reduction. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_d_dirty_publication.md` | Dirty-row and publication cadence calibration; prevents over-prioritizing dirty bookkeeping. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_e_architecture.md` | Strategic retained-row/slab direction and migration framing. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_f_workload.md` | Workload shape and unsupported optimization exclusions. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_01_end_to_end_architecture.md` | Corroborating broad summary; lower precision than Codex reports. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_02_snapshot_publication.md` | Useful snapshot/coalescing test ideas and row materialization concerns. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_03_qsg_frame_cache.md` | Useful independent QSG/frame-cache findings, especially packed text and descriptor/key concerns. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_claude_04_unbiased_next_steps.md` | Challenge report; useful alternative ideas, but several exact claims need implementation-branch validation. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt` | Metric source for the final bottleneck ranking. |
