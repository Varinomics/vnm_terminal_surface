# Nelostie Profile Final Consolidated Investigation

## Executive summary

Nelostie is an intentional stress workload: a very large terminal grid with high
dirty-row pressure. The solution is not to reduce the grid size or reject the
profile. The useful finding is that several pipeline stages still scale with
full row width, full visible grid size, or full snapshot count before dirty-row
metadata can reduce work.

Measured top costs from `nelostie_profile.txt`:

| Area | Measured cost | Calls | Mean | Main scaling issue |
| --- | ---: | ---: | ---: | --- |
| Ingest text application | `apply_action::print_text` 35.398 s | 472,802 | 74.868 us | Small text spans pay row-width work. |
| Snapshot publication | `render_snapshot::append_rows` 7.797 s | 1,252 | 6.228 ms | Every publication materializes all visible rows. |
| Render-frame construction | `build_terminal_render_frame` 8.368 s | 293 | 28.561 ms | The render thread walks the large snapshot in duplicated passes. |
| QSG text synchronization | `sync_text_resource_nodes` 10.547 s | 293 | 35.995 ms | Many dirty row text resources are laid out/rebuilt. |

The highest-confidence first implementation target is the printable ASCII
`print_text` path. The input reports show that ASCII span writing copies the full
row and then compares the full row to decide whether to advance retained-line
content generation. On an 871-column grid, short text runs pay row-width work.
This is both the largest measured cost and the most localized root cause.

The second major theme is dirty-row timing. Dirty rows are recorded and useful,
but several expensive stages happen before dirty rows can prune work. Snapshot
construction computes dirty ranges, then still appends every visible row.
Session and surface coalescing happen after full snapshots already exist.
Render-frame construction and QSG text synchronization also do substantial
full-frame or many-row work.

Retained-history flat-ring work is not the bottleneck in this profile. The
measured retained-history append work is about 15 ms total, versus seconds to
tens of seconds in text application, snapshot publication, render-frame
construction, and QSG text synchronization.

Recommended sequencing:

1. Harden measurement and add scale validation.
2. Remove low-risk row-wide costs from the `print_text` hot path.
3. Reduce full snapshot construction, duplicate render-frame passes, and QSG
   text rebuild churn.
4. Consider larger incremental snapshot or renderer-delta contracts only after
   the validation model is explicit.

## Measured evidence

### Workload geometry and dirty-row pressure

Measured profile facts:

| Metric | Value |
| --- | ---: |
| Grid rows | 233 |
| Grid columns | 871 |
| Visible cell positions | 202,943 |
| Surface size | 3051.4 x 1630.4 |
| Window size | 3065 x 1664 |
| Device pixel ratio | 1.25 |
| Dirty row mark requests | 7,875,373 |
| Duplicate dirty row mark requests | 7,224,579 |
| Unique pending row marks | 650,794 |
| Published unique rows | 53,632 |
| `mark_all_dirty` calls | 41 |
| Dirty-row snapshot calls | 1,298 |
| Dirty-row snapshot rows | 55,889 |
| Max pending dirty rows | 272 |

Measured interpretation:

| Observation | Value |
| --- | ---: |
| Duplicate dirty mark rate | 91.7% |
| Average dirty rows per dirty-row snapshot | 43.1 |
| Backend render snapshot publications | 1,252 |
| Rendered frames through `updatePaintNode` | 293 |
| Snapshot publications per rendered frame | About 4.27 |

Inference:

The average dirty-row payload is much smaller than the full 233-row visible
grid, but snapshot construction and several render stages still scale close to
full-grid work. The publication/frame ratio suggests that some full snapshots may
be constructed before the render thread can consume them. That is a strong
optimization lead, but the exact coalescible set must be validated against
snapshot-visible semantics.

### GUI ingest and apply path

Measured profile facts:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `Terminal_session::process_pending_commands` | 1,529 | 47.323 s | 30.950 ms | 1.613 s |
| `Terminal_session::process_backend_output_command` | 1,285 | 47.127 s | 36.675 ms | 647.557 ms |
| `Terminal_session::ingest_backend_output_segment` | 1,285 | 46.671 s | 36.320 ms | 646.538 ms |
| `Terminal_session::model_ingest` | 1,285 | 38.168 s | 29.702 ms | 630.946 ms |
| `Terminal_screen_model::ingest` | 1,285 | 38.166 s | 29.701 ms | 630.942 ms |
| `Terminal_screen_model::parser_ingest` | 1,285 | 1.801 s | 1.401 ms | 11.119 ms |
| `Terminal_screen_model::apply_parser_actions` | 1,285 | 36.100 s | 28.093 ms | 626.721 ms |
| `Terminal_screen_model::apply_action::print_text` | 472,802 | 35.398 s | 74.868 us | 10.385 ms |
| `Terminal_screen_model::apply_action::control_sequence` | 61,296 | 298.097 ms | 4.863 us | 18.030 ms |
| `Terminal_screen_model::apply_action::style_mutation` | 443,371 | 58.825 ms | 132 ns | 118.200 us |

Measured ratios:

| Ratio | Value |
| --- | ---: |
| `print_text` share of `apply_parser_actions` | 98.1% |
| `print_text` share of `Terminal_screen_model::ingest` | 92.8% |
| `parser_ingest` share of `Terminal_screen_model::ingest` | 4.7% |
| `style_mutation` share of `Terminal_screen_model::ingest` | 0.15% |

Source-based inference from the input reports:

The parser is not the main issue. The hot shape is applying text. The printable
ASCII path copies a full row into `before_cells`, writes a span, then compares
full rows through retained content-generation logic. On an 871-column grid, this
makes short ASCII spans expensive even when only a few cells changed.

### Snapshot publication and dirty rows

Measured profile facts:

| Scope | Calls | Total | Mean | Self | Child | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `Terminal_session::publish_backend_render_snapshot` | 1,252 | 8.494 s | 6.785 ms | 7.19 ms | 8.487 s | 15.576 ms |
| `Terminal_session::publish_render_snapshot` | 1,252 | 8.487 s | 6.779 ms | 656.1 ms | 7.831 s | 15.569 ms |
| `Terminal_screen_model::render_snapshot` | 1,252 | 7.831 s | 6.255 ms | 8.32 ms | 7.823 s | 13.572 ms |
| `Terminal_screen_model::render_snapshot::append_rows` | 1,252 | 7.797 s | 6.228 ms | 7.797 s | 0 | 13.540 ms |
| `Terminal_screen_model::render_snapshot::dirty_rows` | 1,252 | 3.39 ms | 2.71 us | 3.39 ms | 0 | 45.7 us |
| `Terminal_screen_model::dirty_rows` | 1,285 | 1.57 ms | 1.22 us | 1.57 ms | 0 | 19.6 us |
| `Terminal_screen_model::publish_pending_changes` | 991,674 | 106.8 ms | 107 ns | 106.8 ms | 0 | 71.8 us |

Measured interpretation:

`append_rows` is 99.6% of `Terminal_screen_model::render_snapshot` time and
91.9% of `Terminal_session::publish_render_snapshot` time. Dirty-row computation
itself is milliseconds total, not seconds.

Source-based inference from the input reports:

`Terminal_screen_model::render_snapshot` computes dirty row ranges, then still
loops every visible row and materializes row cells. Dirty ranges are metadata on
a full snapshot. They do not currently prune upstream snapshot construction.
Dirty-row coalescing in session/surface code happens after full snapshots have
already been built.

### Render-thread frame construction

Measured profile facts:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `VNM_TerminalSurface::updatePaintNode` | 293 | 19.748 s | 67.398 ms | 1.578 s |
| `build_terminal_render_frame` | 293 | 8.368 s | 28.561 ms | 89.951 ms |
| `build_terminal_render_frame::cells` | 292 | 4.229 s | 14.483 ms | 49.414 ms |
| `build_terminal_render_frame::packed_data` | 292 | 4.100 s | 14.042 ms | 44.889 ms |
| `Qsg_terminal_renderer::update_node` | 293 | 11.133 s | 37.998 ms | 1.503 s |

Measured interpretation:

Render-frame construction is not a minor downstream cost. The two main child
scopes, `cells` and `packed_data`, are nearly equal and together account for
about 8.329 s. This supports the conclusion that frame construction duplicates
full-cell work.

Source-based inference from the input reports:

The frame builder classifies cells once for text/graphics/decorations and then
revisits cells to build packed data. Dirty rows do not eliminate those full
passes in the current profile shape.

### QSG text synchronization and layout

Measured profile facts:

| Scope or counter | Calls / value | Total / value | Mean |
| --- | ---: | ---: | ---: |
| `sync_text_resource_nodes` | 293 | 10.547 s | 35.995 ms |
| `make_text_resource_node` | 21,593 | 6.470 s | 299.611 us |
| `append_batched_text_run_nodes` | 21,593 | 6.463 s | 299.297 us |
| `prepare_text_layout` | 516,835 | 2.201 s | 4.259 us |
| `add_text_run_layout` | 516,835 | 3.898 s | 7.542 us |
| `text_run_groups_by_viewport_row` | 293 | 204.854 ms | 699.160 us |
| `text_resource_row_descriptor` | 32,112 | 698.923 ms | 21.765 us |
| `sync_text_resource_nodes::coalescing` | 21,597 | 553.832 ms | 25.644 us |
| `sync_text_resource_nodes::replace_cache_entry` | 15,336 | 988.250 ms | 64.438 us |
| `sync_text_resource_nodes::remove_stale_entries` | 293 | 433.767 ms | 1.480 ms |
| `sync_text_resource_nodes::reparent_slots` | 284 | 517.688 ms | 1.823 ms |

Measured renderer stats from the input reports:

| Counter | Value |
| --- | ---: |
| `text_runs_considered` | 14,730,881 |
| `text_resource_runs_before_coalescing` | 12,660,003 |
| `text_resource_runs_after_coalescing` | 516,863 |
| `text_content_rebuilds` | 21,593 |
| `text_content_reused` | 10,519 |
| `text_clean_reuse_skips` | 6,401 |
| `text_resource_descriptor_reuses` | 4,114 |
| `text_cache_entries_created` | 6,257 |
| `text_cache_entries_replaced` | 15,336 |
| `text_content_removed` | 6,094 |
| `text_leaf_nodes_created` | 227,035 |

Measured interpretation:

Text coalescing is effective but too late to remove the dominant costs. The
profile still has 516,835 layout calls each for prepare/add. Slow-layout tracking
reported no >10 ms layout outliers, so the problem is cumulative
many-small-layout work.

Correction to one input report:

One Claude-derived report claimed that any dirty row invalidates the entire
text-node graph because the text cache is keyed on a frame-wide content hash.
The cross-reviews found this is overstated. The text frame key covers stable
frame context such as font, logical size, cell metrics, grid size, and active
buffer. It does not hash text content or dirty rows. Row-level reuse exists. The
accurate conclusion is that row-level reuse and coalescing exist but remain
insufficient under the captured dirty-row/text-run pressure.

## What is not the bottleneck

### Parser ingestion

Measured fact:

`Terminal_screen_model::parser_ingest` is 1.801 s total, while
`apply_parser_actions` is 36.100 s and `print_text` is 35.398 s.

Conclusion:

The parser is not the dominant cost in this capture. Text application is.

### SGR/style mutation application

Measured fact:

`Terminal_screen_model::apply_action::style_mutation` is 58.825 ms over 443,371
calls.

Conclusion:

Style mutation application itself is not material. Frequent style changes can
still fragment text and multiply downstream per-span costs, but the direct style
mutation scope is not the lever.

### Dirty-row range computation

Measured fact:

`render_snapshot::dirty_rows` is 3.39 ms total, `dirty_rows()` is 1.57 ms total,
and `publish_pending_changes` is 106.8 ms total.

Conclusion:

Dirty-row handling has striking counts and may contribute inside larger scopes,
but the measured standalone dirty-row scopes are not primary bottlenecks.
Dirty-row representation cleanup should follow the larger row-copy, snapshot,
and render-thread fixes unless new sub-profiling changes the evidence.

### Retained-history flat ring

Measured fact:

The detailed renderer report measured retained-history append paths at about
14.620 ms total across 458 appends. Lower-level ring operations are
microsecond-scale relative to the dominant costs.

Conclusion:

Retained-history ring work should not be prioritized for Nelostie performance.
Keep it monitored, but do not spend the first optimization batches there.

### Reducing grid size

Measured/context fact:

The 233 x 871 grid is intentional stress coverage.

Conclusion:

Reducing the grid size is not a solution. The objective is to make the pipeline
scale better for large grids and high dirty-row pressure.

## Primary bottleneck stack

### Bottleneck 1: row-width work per printable ASCII span

Measured fact:

`Terminal_screen_model::apply_action::print_text` is 35.398 s, the largest
single measured cost.

Source-based inference:

The printable ASCII path performs full-row copy and full-row comparison around
span writes. This turns many short text spans into `span_count * columns` work.

Priority rationale:

This is the largest measured cost and the most localized fix candidate. It
should be addressed before larger snapshot or renderer contract changes.

### Bottleneck 2: full visible-row snapshot materialization

Measured fact:

`Terminal_screen_model::render_snapshot::append_rows` is 7.797 s over 1,252
calls.

Source-based inference:

Dirty-row metadata does not prune snapshot materialization. Full snapshots are
built before session/surface coalescing can help.

Priority rationale:

This is the second GUI-side cost center and is multiplied by snapshot publication
count. It is high value, but delta snapshot changes are higher risk than the
`print_text` fix.

### Bottleneck 3: producer/consumer mismatch in snapshot publication

Measured fact:

There are 1,252 backend render snapshot publications and 293 rendered frames.

Inference:

Some snapshots may be superseded before rendering. The exact savings are not
directly measured, because some publications may have externally visible
semantics.

Priority rationale:

Add counters before implementation. If many snapshots are safely coalescible
before full construction, this can reduce the `append_rows` cost without
immediately changing the full-snapshot contract.

### Bottleneck 4: duplicated render-frame cell passes

Measured fact:

`build_terminal_render_frame::cells` is 4.229 s and
`build_terminal_render_frame::packed_data` is 4.100 s.

Source-based inference:

The render-thread frame construction revisits the same large snapshot data for
related classification/packing work.

Priority rationale:

A fused or retained row-frame path can reduce 8.368 s of render-thread work and
support later QSG reductions.

### Bottleneck 5: QSG text-resource rebuild and many-small-layout cost

Measured fact:

`sync_text_resource_nodes` is 10.547 s, with 6.470 s in
`make_text_resource_node` and 516,835 calls each to `prepare_text_layout` and
`add_text_run_layout`.

Source-based inference:

Existing row-level reuse and ASCII coalescing help, but dirty-row pressure still
causes enough row text-resource rebuilds and layout calls to dominate QSG update
time.

Priority rationale:

This is the largest render-thread child, but safe improvements depend on clear
row identity, descriptor reuse, cursor/IME behavior, and QSG node ownership
semantics.

## Improvement roadmap

| Phase | Goal | Primary measured target | Risk |
| --- | --- | --- | --- |
| Phase 0 | Harden measurement and scale validation. | All hot scopes. | Low |
| Phase 1 | Remove localized row-width hot-path work. | 35.398 s `print_text`. | Medium |
| Phase 2 | Reduce snapshot/render/QSG repeated full-frame work. | 7.797 s `append_rows`, 8.368 s frame build, 10.547 s QSG text sync. | Medium to high |
| Phase 3 | Consider larger stateful/delta architecture. | Full pipeline scaling under dirty rows. | High |

Concrete priority order:

1. Add subscopes and counters before behavior changes.
2. Remove full-row copy/compare from printable ASCII writes.
3. Add contract-safe pre-materialization snapshot coalescing.
4. Merge or share render-frame full-cell passes.
5. Improve QSG row text reuse and reduce text-resource rebuilds.
6. Design an incremental snapshot or renderer-delta contract only after the
   validation model is explicit.
7. Optimize dirty-row containers and `Cell` representation only after the hotter
   avoidable work is reduced.

## Phase 0: measurement hardening

Phase 0 should land before implementation-heavy performance changes.

### Add model/apply subscopes

Required counters or scopes:

| Counter/scope | Purpose |
| --- | --- |
| ASCII spans written | Separates action count from span count. |
| Full-row copies in text writes | Confirms source-based inference. |
| Row content-generation comparisons | Measures row-wide compare share. |
| Cells written by ASCII span path | Measures useful work versus overhead. |
| Wide-boundary repairs during ASCII writes | Quantifies correctness-sensitive slow cases. |
| Dirty marks from text writes | Measures dirty marking inside the hot scope. |

Validation before implementation:

Confirm that row copy/compare is a material child of `print_text`. If not,
adjust Phase 1 to the measured child cost.

### Add snapshot publication counters

Required counters:

| Counter | Purpose |
| --- | --- |
| Snapshot requests | Measures producer frequency. |
| Full snapshots constructed | Measures actual construction count. |
| Snapshots rendered | Measures consumer count. |
| Snapshots superseded before render | Quantifies coalescing opportunity. |
| Rows visited/materialized per snapshot | Separates full-grid work from dirty-row work. |
| Cells scanned/emitted per snapshot | Tracks scaling shape. |
| Full-repaint fallback reasons | Explains dirty-row expansion. |

Validation before implementation:

Identify which snapshot publications are contract-safe to coalesce and which are
externally visible.

### Add render-frame and QSG counters

Required counters:

| Counter | Purpose |
| --- | --- |
| Render-frame rows recomputed/reused | Validates retained row-frame design. |
| Cell-pass input size for main and packed passes | Tracks duplicate work. |
| Dirty-row lookup count/time | Checks whether row dirty lookup is worth optimizing. |
| QSG text reuse failure reasons | Explains rebuilds. |
| Dirty-but-descriptor-identical rows | Finds cheap reuse opportunities. |
| Post-coalescing run count per row | Explains layout distribution. |
| QSG nodes created/destroyed/reparented | Tracks churn. |

Validation before implementation:

Confirm whether text-resource rebuilds are dominated by many dirty rows,
descriptor/key misses, wrapper churn, layout calls, or stale-entry removal.

### Add scale validation fixtures

Required fixture shape:

| Fixture | Purpose |
| --- | --- |
| 233 x 871 ASCII stress fixture | Reproduces row-width scaling. |
| Short styled text-run fixture | Reproduces action fragmentation. |
| Partial dirty-row fixture with mostly clean rows | Validates dirty-row scaling. |
| Full dirty-row fixture | Preserves worst-case behavior. |
| Producer-faster-than-render fixture | Validates snapshot coalescing. |

Use counters for pass/fail where possible. Avoid brittle wall-clock thresholds
except in explicit benchmark tools.

## Phase 1: low-risk hot-path reductions

### Phase 1A: remove full-row copy/compare from printable ASCII writes

Measured target:

`Terminal_screen_model::apply_action::print_text`: 35.398 s.

Fix shape:

Replace full-row `before_cells` copy plus full-row comparison with write-time or
range-based mutation detection. The row content generation should advance only
when selection-visible content actually changes.

Concrete validation before implementation:

- Identify every writer that relies on `advance_row_content_generation_if_changed`
  semantics.
- Define exactly which cell fields affect retained-line selection content.
- Define how wide continuations, combining marks, hyperlink ids, and style ids
  affect the generation decision.
- Add focused tests for no-op writes that must not advance generation and real
  writes that must advance generation.

Expected benefit:

High. This removes row-width overhead from the largest measured hot path. Any
exact speedup factor is an inference until subscopes and before/after profiles
exist.

Risk:

Medium. Incorrect generation handling can break retained row identity, selection
extraction, public projection correctness, or repaint recovery.

### Phase 1B: add a true bulk printable-ASCII span writer

Measured target:

Same `print_text` path, after Phase 1A confirms remaining per-cell work.

Fix shape:

Write contiguous ordinary ASCII spans directly where safe. Avoid repeated
general `clear_cell_at` logic for every one-cell ASCII write. Handle wide-span
boundaries and encountered continuations deliberately.

Concrete validation before implementation:

- ASCII over ordinary empty cells.
- ASCII over ordinary occupied cells with same style/hyperlink.
- ASCII over wide glyph starts and continuations.
- ASCII near combining marks.
- Autowrap and no-autowrap right-margin cases.
- Style and hyperlink transitions around short text runs.

Expected benefit:

Potentially high, but should follow Phase 1A because row copy/compare is the
more obvious avoidable row-width cost.

Risk:

Medium to high. The common path is simple, but terminal cell cleanup semantics
are subtle.

### Phase 1C: low-risk dirty-row helper changes only if subscopes justify them

Measured target:

Potential hidden child work inside `print_text`, not standalone
`publish_pending_changes`.

Fix shape:

If dirty marking is a meaningful child of `print_text`, introduce a bounded
dirty-row representation or optimized mark path. If it is not, defer this to
later cleanup.

Concrete validation before implementation:

Measure dirty marking inside the hot mutation scope. Do not prioritize from
aggregate dirty-row call counts alone.

## Phase 2: render publication and QSG reductions

### Phase 2A: coalesce snapshot publication before full construction where safe

Measured target:

1,252 backend render snapshot publications versus 293 rendered frames, and
7.797 s in `append_rows`.

Fix shape:

Coalesce model results and dirty metadata before calling `render_snapshot` only
when intermediate publications are proven not externally observable.

Concrete validation before implementation:

| Consumer/semantic area | Validation question |
| --- | --- |
| Snapshot-ready notifications | Can an intermediate notification be omitted or merged? |
| Transcript/capture/replay | Does capture require each snapshot sequence? |
| Selection visual leases | Does a skipped snapshot alter lease advancement? |
| Synchronized output release | Are release boundaries observable? |
| Public projection scroll snapshots | Does coalescing break projection diagnostics? |
| Cursor, bell, mode changes | Are visual transitions allowed to coalesce? |
| Resize/geometry snapshots | Are geometry-derived snapshots special? |

Expected benefit:

High if many snapshots are superseded before render. The exact benefit must be
measured with new counters.

Risk:

Medium to high. This touches externally visible sequencing semantics.

### Phase 2B: reduce `append_rows` materialization cost without changing public snapshot semantics

Measured target:

`Terminal_screen_model::render_snapshot::append_rows`: 7.797 s.

Fix shape:

Short of a full delta contract, reduce avoidable work inside full snapshot
construction:

- Avoid copying row vectors by value where a const row reference or row view is
  safe.
- Combine repeated viewport-to-backing row lookups per row.
- Avoid repeated hyperlink metadata scans where rows have no hyperlinks.
- Add fast paths for active-grid rows already at current geometry.

Concrete validation before implementation:

Profile subscopes first. These are lower-risk improvements, but they should be
driven by measured child costs inside `append_rows`.

Expected benefit:

Medium. It does not change the full-grid scaling shape, but it may reduce
constant factors while larger contracts are designed.

Risk:

Low to medium if snapshot output remains structurally equivalent.

### Phase 2C: merge render-frame packed-data work with the main cell pass

Measured target:

`build_terminal_render_frame::cells`: 4.229 s.
`build_terminal_render_frame::packed_data`: 4.100 s.

Fix shape:

Emit packed text/graphic spans as a byproduct of the same classification pass
used to build render-frame text, graphics, decorations, cursor interactions, and
row descriptors. If full fusion is too risky, cache classification results per
row for reuse by the packed pass.

Concrete validation before implementation:

- Compare frame stats before/after for text runs, graphic rects, packed rows,
  packed spans, packed text cells, and packed graphic cells.
- Include cursor, IME, selection, block graphics, and non-ASCII cases.
- Ensure `packed_data` decreases without equivalent growth in `cells`.

Expected benefit:

High to medium. The two scopes together are 8.329 s.

Risk:

Medium. Render classification has many feature interactions.

### Phase 2D: improve QSG text-row reuse and reduce rebuild churn

Measured target:

`sync_text_resource_nodes`: 10.547 s.
`make_text_resource_node`: 6.470 s.
`prepare_text_layout` plus `add_text_run_layout`: 6.099 s combined.

Fix shape:

Improve reuse before attempting low-level node pooling:

- Keep stable frame context separate from row content keys.
- Make cheap clean-row and descriptor reuse fire for partial-dirty frames when
  row content is unchanged.
- Track dirty-but-text-identical rows and avoid rebuilding their text nodes.
- Stabilize viewport-row wrapper slots where possible to reduce reparenting.
- Consider direct row-span resources so the QSG path consumes coalesced spans
  without first building large per-cell text-run lists.

Concrete validation before implementation:

- Add reuse failure reason counters.
- Add tests for partial dirty updates where most rows are clean.
- Include scrollback movement, alternate screen, cursor block inversion, IME
  preedit, resize, and selection overlays.
- Confirm `text_content_reused` rises and `text_cache_entries_replaced`, stale
  removals, child-node clears, and layout calls fall.

Expected benefit:

High. QSG text synchronization is the largest render-thread child.

Risk:

Medium to high. QSG node lifetime and row identity are sensitive.

## Phase 3: larger architectural options

Phase 3 options should be governed as architectural work, not local hotfixes.

### Option 3A: incremental snapshot or renderer-delta contract

Measured target:

Full snapshot materialization and downstream full-frame work under partial dirty
updates.

Design shape:

Keep full snapshots for debug/transcript/public consumers unless explicitly
migrated. Add an internal renderer-facing delta or row-cache path that carries
changed row payloads, row identity, content generation, cursor/mode/style
changes, and invalidation flags. The render side retains the last full row state
and applies deltas under stable viewport row identity.

Inference:

This could make publication and render-frame construction scale with dirty rows
rather than full visible rows. The average dirty-row snapshot had about 43 rows
versus 233 visible rows, so the theoretical opportunity is large. Actual benefit
depends on viewport changes, full repaints, row identity mismatches, cursor/IME,
selections, and public projection behavior.

Required validation before implementation:

- A/B comparison of existing full snapshots versus reconstructed delta state
  across transcript replay.
- Forced full-resync on viewport identity mismatch, resize, active-buffer
  transition, public projection mismatch, selection provenance mismatch,
  retained history mismatch, and memory pressure.
- Counters for delta rows, reused rows, full fallbacks, and fallback reasons.
- No silent fallback. Unexpected fallback growth must be visible in profiles.

Risk:

High. Missing cells currently mean blank cells in full-snapshot consumers, not
unchanged cells. A delta contract must be explicit.

### Option 3B: retained per-row render-frame cache

Measured target:

`build_terminal_render_frame` and QSG row resource rebuilds.

Design shape:

Retain render-frame outputs per viewport row keyed by row identity and content
generation. Recompute only dirty rows, cursor/IME affected rows, selection
affected rows, and rows whose retained identity changed after scroll or resize.

Required validation before implementation:

- Prove row identity and content generation are sufficient for reuse.
- Include scrollback, alternate screen, resize, cursor, selection, IME, and
  hyperlinks.
- Compare retained outputs against full frame construction in replay tests.

Risk:

Medium to high. This overlaps with incremental snapshot design and should not
become a second inconsistent state-retention system.

### Option 3C: row/run-oriented model storage or inline ASCII cell storage

Measured target:

Cell copy and allocation pressure after avoidable row-wide copies are removed.

Design shape:

Represent common ASCII cells more compactly or store rows as runs where terminal
semantics allow. Keep heap/string representation for complex grapheme clusters.

Required validation before implementation:

- First re-profile after Phases 1 and 2.
- Confirm `Cell` size, `QString` refcounting, or per-cell allocation remains a
  dominant cost.
- Build broad compatibility tests for Unicode, combining marks, wide glyphs,
  hyperlinks, styles, selections, and history encoding.

Risk:

High. This touches core representation and should not be first.

## Validation gates

### Gate 1: baseline and instrumentation

Before code changes:

- Save the current Nelostie profile metrics listed in this report as the
  baseline.
- Add Phase 0 counters under existing profiling controls.
- Add at least one large-grid stress fixture or profiling replay gate.

Pass condition:

The new counters explain the dominant existing scopes without materially
changing profile behavior.

### Gate 2: `print_text` row-width fix

Before implementation:

- Prove row copy and row comparison are material children of `print_text`.
- Define content-generation semantics for all affected cell fields.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `apply_action::print_text` total | Down substantially. |
| `apply_action::print_text` mean | Down substantially. |
| `apply_parser_actions` total | Down roughly with `print_text`. |
| Parser and style mutation totals | No material regression. |
| Dirty-row output counters | Semantically equivalent. |
| Selection/retained identity tests | Pass. |

### Gate 3: snapshot publication changes

Before implementation:

- Classify snapshot publications as coalescible or non-coalescible.
- Add counters for constructed, rendered, and superseded snapshots.

After implementation:

| Metric | Expected direction |
| --- | --- |
| Full snapshots constructed | Down where coalescing is allowed. |
| `append_rows` total | Down with construction count or per-snapshot cost. |
| Snapshot-ready behavior | Contract-preserving. |
| Transcript/replay behavior | Contract-preserving. |
| Selection and synchronized-output behavior | Contract-preserving. |

### Gate 4: render-frame and QSG changes

Before implementation:

- Add counters for recomputed rows, reused rows, and QSG reuse failure reasons.

After implementation:

| Metric | Expected direction |
| --- | --- |
| `build_terminal_render_frame::packed_data` | Down without equivalent `cells` growth. |
| `sync_text_resource_nodes` | Down. |
| `make_text_resource_node` | Down. |
| `prepare_text_layout` / `add_text_run_layout` calls | Down in partial-dirty cases. |
| Text content reuse counters | Up. |
| Node replacement/removal/reparent counters | Down. |

### Gate 5: architectural delta/retention changes

Before implementation:

- Write the row identity, content generation, dirty-range, and fallback contract.
- Add an A/B full-snapshot oracle.

After implementation:

| Metric | Expected direction |
| --- | --- |
| Rows materialized per partial-dirty update | Tracks dirty rows, not full rows. |
| Render rows recomputed per partial-dirty update | Tracks dirty/affected rows, not full rows. |
| Full fallback count | Low and explainable. |
| Full snapshot equivalence checks | Pass. |

## Risks and open questions

### Retained-line content generation semantics

Risk:

Removing full-row compare from `print_text` can produce stale or over-eager
content generations.

Open question:

Which exact fields define selection-visible row content? The implementation must
encode that invariant directly instead of relying on a full-row before/after
comparison.

### Snapshot publication semantics

Risk:

Coalescing snapshots before construction may skip an externally observable
state.

Open question:

Which snapshot publications are observable through notifications, transcript
capture, selection leases, synchronized-output release, public projection
diagnostics, cursor/bell/mode changes, and resize handling?

### Full snapshot versus delta semantics

Risk:

Existing consumers treat missing cells as blanks. A delta snapshot cannot reuse
that meaning.

Open question:

Should the renderer get a separate internal delta type while
public/debug/transcript paths keep full snapshots, or should the main snapshot
contract evolve?

### QSG text reuse diagnosis

Risk:

Overcorrecting the incorrect Claude frame-key claim could hide a real reuse
issue.

Open question:

Which reuse misses dominate: dirty group classification, descriptor mismatch,
row identity changes, per-row key mismatch, wrapper reparenting, stale removals,
or text layout itself? Phase 0 counters should answer this before QSG changes.

### Test coverage at stress scale

Risk:

Small-grid correctness tests can pass while large-grid scaling regresses.

Open question:

Which existing tests already cover large grids or performance counters? The
reports suggest little stress-grid coverage, but the final implementation plan
should audit the test suite before adding duplicate fixtures.

### Multi-batch governance

Risk:

Combining model text writes, snapshot semantics, render-frame retention, and QSG
row reuse in one batch would be hard to validate and review.

Open question:

Where should the state-retention boundary live: model/session snapshot
publication, render-frame construction, QSG text resources, or a deliberately
shared row-state layer?

## Source reports considered

Initial investigation reports:

| Report | Final use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_ingest_print_text_report.md` | Primary evidence for ingest/apply and `print_text`. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_render_snapshot_dirty_rows_report.md` | Primary evidence for snapshot publication and dirty-row behavior. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_qsg_report.md` | Primary evidence for render-thread/QSG behavior. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_claude_end_to_end_pipeline_report.md` | Corroborating summary only. Too shallow to rely on independently. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_claude_scaling_strategy_report.md` | Corroborating summary only. Too shallow at requested path; linked detailed review has useful points but includes an overstated QSG cache-key claim. |

Cross-reviews:

| Report | Final use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_ingest_cross_review.md` | Confirms report quality, priority order, and Claude limitations. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_snapshot_cross_review.md` | Confirms snapshot/dirty-row conclusions and corrects the Claude QSG frame-key claim. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_cross_review.md` | Confirms render/QSG conclusions and phases. |

Profile source:

| Source | Final use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt` | Direct measured evidence for all profile totals and call counts. |

Final report artifact:

| Report | Path |
| --- | --- |
| Final consolidated report | `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_profile_final_consolidated_report.md` |
