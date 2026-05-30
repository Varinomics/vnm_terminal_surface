# Focus F: workload and application behavior audit

## Executive summary

Nelostie should be treated as an intentional stress workload, but the captured behavior is not arbitrary terminal noise. The scalar-span profile looks like a large-grid, cursor-addressed, full-screen TUI workload that repeatedly mutates visible rows in place. It does not look like a scrollback-heavy stream, a line-wrap stream, or a DEC synchronized-output workload.

The strongest workload facts are:

- The visible grid is extreme: 235 rows by 873 columns, or 205,155 cell positions per full viewport.
- Text mutation is now a secondary cost: `apply_action::print_text` is 1.010 s over 83,349 calls, while downstream frame construction, QSG sync, and snapshot work dominate the scalar-span profile.
- Printable ASCII is common at ingest: 54,736 printable-ASCII span calls wrote 3,046,585 cells, with no printable-ASCII row copies left in this profile.
- Rendered content is single-width only in the captured renderer counters. There are no multi-width cells, decorations, or hyperlinks in renderer stats.
- Cumulative rendered content includes a large non-ASCII/graphic component: 24,094,528 non-ASCII cells and 22,885,056 cells rendered as graphics across render frames, versus 31,742,664 printable-ASCII cells.
- The workload does not use DEC synchronized output in this capture: dirty-row synchronized collection and release counters are all zero.
- Text writes did not cause line wrapping or scrollback appends: `line_wraps_from_text_writes=0` and `scrollback_appends_from_text_writes=0`.
- Full-screen repaint behavior exists, but it is episodic: `mark_all_dirty_calls=16`, `max_pending_dirty_rows=235`, and cumulative renderer full-dirty rows are 2,115. Most publications are not full dirty: 14,379 visible dirty rows across 1,498 snapshots, averaging about 9.6 dirty rows per snapshot.

The practical conclusion is that specialized work should target full-screen, row-addressed animation patterns: single-width row spans, graphic/block-cell spans, repeated cursor-addressed updates, and high duplicate dirty marking within a small row set. DEC synchronized-output fast paths are unlikely to help this exact capture unless a future trace proves Nelostie emits `DECSET ?2026` / `DECRST ?2026` in another mode.

## Workload evidence from profiles/source/traces if available

Profile evidence comes from `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`. I did not find Nelostie application source under the two requested roots, and I did not find raw terminal byte/control-sequence traces in `C:\plms\varinomics\vnm_terminal\build` or `C:\plms\varinomics\vnm_terminal\build\profiles`. The available trace-like evidence is therefore aggregate profile counters and the existing profiling reports.

Measured geometry and publication shape:

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
| `mark_all_dirty_calls` | 16 |
| `max_pending_dirty_rows` | 235 |
| Render snapshots constructed | 1,498 |
| Rendered frames | 662 |
| Snapshots superseded before render | 112 |

Dirty-row interpretation:

- The duplicate dirty mark rate is about 97.2%, so many operations repeatedly touch rows that are already dirty before publication.
- Dirty snapshots average about 9.6 visible dirty rows per snapshot, which is far below the 235-row viewport.
- The timeline includes both small-row update buckets and burst buckets. Several high-mark buckets have `max_pending_dirty_rows=2`, indicating intense repeat updates over a narrow row set. Other buckets include `mark_all_dirty_calls` and 235-row pending maxima, indicating episodic full-screen invalidation.
- `render_snapshot_full_repaint_fallbacks=0` and `render_snapshot_viewport_fallbacks=0`, so the model did not report fallback-driven full repaint behavior in this capture. The full-dirty events appear to come from terminal/app-visible state changes, explicit full dirty marking, or semantic invalidation, not from snapshot fallback failure.

Text/apply evidence:

| Metric | Value |
| --- | ---: |
| `print_text_calls` | 83,349 |
| `printable_ascii_span_calls` | 54,736 |
| `printable_ascii_span_characters` | 3,046,585 |
| `printable_ascii_cells_written` | 3,046,585 |
| Max printable ASCII span length | 640 |
| `printable_ascii_row_copies` | 0 |
| `printable_ascii_local_cells_inspected` | 2,387,338 |
| `scalar_span_local_cells_inspected` | 1,388,543 |
| `row_content_generation_comparisons` | 1,241 |
| `row_content_generation_comparison_cells` | 1,083,393 |
| `row_content_generation_advances` | 369,357 |
| `dirty_marks_from_text_writes` | 164,208 |
| `line_wraps_from_text_writes` | 0 |
| `scrollback_appends_from_text_writes` | 0 |

This says the current profile is no longer the old printable-ASCII row-copy problem. It is still a heavy text/update workload, but text writes are not wrapping and are not appending scrollback. Because many text calls happen without wrap or scrollback, and because the viewport is very wide, the likely behavior is explicit cursor addressing followed by bounded row spans.

Parser/action evidence from the existing end-to-end post-scalar report:

| Scope | Calls | Total |
| --- | ---: | ---: |
| `Terminal_screen_model::parser_ingest` | 1,500 | 470.349 ms |
| `Terminal_screen_model::apply_parser_actions` | 1,500 | 1.216 s |
| `Terminal_screen_model::apply_action::print_text` | 83,349 | 1.010 s |
| `Terminal_screen_model::apply_action::control_sequence` | 20,284 | 143.401 ms |
| `Terminal_screen_model::apply_action::style_mutation` | 67,289 | 4.953 ms |

The profile does not include a control-sequence histogram, but the combination of many control sequences, zero text-wraps, zero text-scrollback appends, and high visible-row mutation is consistent with a cursor-addressed full-screen application.

Renderer content evidence:

| Cumulative renderer metric | Value |
| --- | ---: |
| `frame_visible_rows` | 155,335 |
| `frame_dirty_rows` | 11,096 |
| `frame_full_dirty_rows` | 2,115 |
| `frame_cell_pass_input_cells` | 55,837,192 |
| `frame_text_cells_rendered_as_text` | 32,952,136 |
| `frame_text_cells_rendered_as_graphic` | 22,885,056 |
| `frame_text_cells_printable_ascii` | 31,742,664 |
| `frame_text_cells_non_ascii` | 24,094,528 |
| `frame_text_cells_single_width` | 55,837,192 |
| `frame_text_cells_multi_width` | 0 |
| `frame_text_cells_with_decorations` | 0 |
| `frame_text_cells_with_hyperlink` | 0 |
| `frame_text_style_changes` | 854,773 |
| `frame_text_distinct_styles` | 6,465 |
| `frame_text_runs_emitted` | 32,952,136 |
| `frame_graphic_rects_emitted` | 8 |
| `graphic_layer_rebuilds` | 272 |
| `cursor_layer_rebuilds` | 160 |
| `cursor_text_layer_rebuilds` | 69 |

Last-frame renderer evidence is more ASCII-dominant:

| Last-frame renderer metric | Value |
| --- | ---: |
| `frame_visible_rows` | 235 |
| `frame_dirty_rows` | 75 |
| `frame_text_cells_rendered_as_text` | 102,975 |
| `frame_text_cells_rendered_as_graphic` | 0 |
| `frame_text_cells_printable_ascii` | 102,345 |
| `frame_text_cells_non_ascii` | 630 |
| `frame_text_style_changes` | 144 |
| `frame_text_distinct_styles` | 2 |
| `frame_text_runs_emitted` | 102,975 |
| `text_resource_runs_before_coalescing` | 46,151 |
| `text_resource_runs_after_coalescing` | 147 |

The difference between cumulative and last-frame stats matters. Nelostie appears to move through phases: earlier frames contain substantial non-ASCII/graphic-rendered content, while the final frame is mostly printable ASCII with very low style diversity.

Source evidence relevant to terminal semantics:

- `terminal_screen_model.cpp` handles DEC private modes 1047, 1049, and 2026. Mode 1047 and 1049 enter/leave alternate screen; mode 2026 toggles synchronized output.
- `terminal_screen_model.cpp` has counters for text-write line wrapping and text-write scrollback appends; both are zero in the profile.
- `terminal_session.cpp` has explicit synchronized-output entry/release handling and force-release paths, but the profile synchronized dirty-row counters stayed at zero.
- `vnm_terminal_surface.cpp` has user-facing behavior for alternate-screen wheel policy and synchronized-output scroll policy, but the profile does not include direct workload counters proving active alternate-screen state.
- `vnm_terminal/src/main.cpp` writes the renderer/model counters used above into profile output, including style, graphic/non-ASCII, cursor, line-wrap, scrollback, and synchronized-output dirty-row counters.

## Terminal pattern hypotheses

### Alternate screen

Hypothesis: Nelostie likely behaves like an alternate-screen or alternate-screen-like full-screen application, but the provided profile does not directly prove alternate-screen mode was active.

Evidence for the hypothesis:

- Text writes produced no scrollback appends.
- Text writes produced no line wraps.
- The workload updates a fixed large visible grid with many dirty-row publications and episodic full dirty marking.
- Source supports DEC 1047/1049 alternate-screen handling, and the surface has alternate-screen wheel behavior, so this is a meaningful semantic distinction for the terminal.

Evidence gap:

- The profile does not expose `active_buffer`, `alternate_screen`, or DECSET/DECRST 1047/1049 event counts.
- A primary-screen application that constantly cursor-addresses within the visible viewport and avoids newline-driven scrolling could produce a similar aggregate profile.

### Full-screen repaints

Hypothesis: Nelostie emits a mix of row-local updates and occasional full-screen invalidations/repaints.

Evidence:

- `mark_all_dirty_calls=16`.
- `max_pending_dirty_rows=235` equals the full viewport height.
- Cumulative render frames include 2,115 full-dirty rows.
- Timeline buckets show initial full dirty behavior and later bursts with full dirty maxima.
- Most snapshots are not full dirty: only 14,379 dirty rows across 1,498 snapshots.

Interpretation:

The workload is not simply repainting the full 235-row viewport every publication. It has frequent sparse or narrow-row updates plus occasional global invalidation. A fast path should preserve sparse-row advantages while avoiding pathological cost when a full dirty event arrives.

### Cursor motion

Hypothesis: Explicit cursor addressing is a core part of the workload.

Evidence:

- The profile has many `print_text` calls and many control-sequence calls, but zero text-write wraps and zero text-write scrollback appends.
- Printable ASCII spans are bounded; the maximum recorded span is 640 characters, below the 873-column width.
- Dirty rows are often sparse despite a huge grid, which is consistent with cursor-addressed row updates rather than streaming output.

Evidence gap:

- The profile does not count CUP/HVP/CUU/CUD/CUF/CUB/CR/LF/EL/ED separately.
- Without raw control-sequence histograms, this remains a profile-based inference.

### Style churn

Hypothesis: There is substantial style fragmentation over the run, but style mutation itself is not the bottleneck.

Evidence:

- `apply_action::style_mutation` has 67,289 calls but only 4.953 ms total in the current profile.
- Cumulative renderer stats record 854,773 frame text style changes and 6,465 distinct style observations.
- The last frame has only 144 style changes and two distinct styles, so style churn is phase-dependent.
- Text coalescing reduced 4,378,068 candidate resource runs to 483,907 cumulative runs, and 46,151 last-frame runs to 147 runs.

Interpretation:

Style-setting escape sequences are cheap in the model, but style fragmentation affects render run grouping and QSG resource churn. The likely fast path is not optimizing SGR application itself; it is preserving row/run products across repeated same-style or low-style-diversity updates.

### Graphics and non-ASCII density

Hypothesis: Nelostie has a significant single-width non-ASCII/block-graphic phase, not a complex Unicode shaping phase.

Evidence:

- Cumulative renderer stats show 24,094,528 non-ASCII cells and 22,885,056 cells rendered as graphics.
- All rendered cells are single-width; `frame_text_cells_multi_width=0`.
- There are no decorations or hyperlinks.
- Last-frame stats show mostly printable ASCII and no graphic-rendered cells, so the graphic/non-ASCII phase is not uniform across the whole run.

Interpretation:

If a specialized path is added, it should target single-width BMP/block/box-drawing style cells and terminal graphic classification, not general grapheme clusters, combining marks, emoji, ligatures, or hyperlinks.

### Synchronized output

Hypothesis: Nelostie does not use DEC synchronized output in this capture.

Evidence:

- `collect_synchronized_calls=0`.
- `collect_synchronized_rows=0`.
- `release_synchronized_calls=0`.
- `released_synchronized_rows=0`.
- `max_synchronized_dirty_rows=0`.
- The source supports DEC 2026, so the zero counters are meaningful rather than absence of implementation.

Interpretation:

A DEC synchronized-output fast path should not be justified from this specific profile. If the desired optimization is to coalesce Nelostie-like bursts, it should be framed as backend-output burst coalescing or render-publication cadence control, not as a DEC 2026 optimization, unless raw trace proves DEC 2026 is emitted in another capture.

### Scroll behavior

Hypothesis: This workload is not scrollback-stream dominated.

Evidence:

- `scrollback_appends_from_text_writes=0`.
- `public_projection_scroll_requests=0` and `public_projection_scroll_publications=0`.
- `line_wraps_from_text_writes=0`.
- Dirty rows stay visible and row-local in most publications.

Interpretation:

Optimizing primary scrollback append, public projection scroll recovery, or newline-driven viewport scroll is unlikely to move this profile. Work should focus on visible-row mutation and rendering.

### Line wrapping

Hypothesis: Line wrapping is intentionally avoided by the application.

Evidence:

- `line_wraps_from_text_writes=0`.
- Max printable ASCII span is 640 characters on an 873-column grid.
- No text-write scrollback appends were recorded.

Interpretation:

A wrap-specific fast path is not indicated by this capture. The useful path is bounded span writing inside a row.

## Specialized fast paths

### High-value candidates

1. Single-width row-span render products.

Nelostie content is entirely single-width in renderer counters. A row-product cache keyed by row identity, content generation, style table identity, and renderer font metrics could reuse text-run grouping, graphic classification, and packed/rect products for clean rows. This targets workload semantics directly: cursor-addressed row updates over a huge grid.

2. Single-width non-ASCII and block-graphic span path.

Cumulative non-ASCII and graphic-rendered cells are large, while multi-width/decorated/hyperlink cells are zero. A specialized classification path for contiguous single-width non-ASCII or block/box cells would fit the workload better than broad Unicode machinery. The path should be guarded by explicit width/style/content invariants, not by hard-coding Nelostie.

3. Dirty-row-local frame construction.

The renderer already skips many clean text rows, but frame construction still sees full-cell input. The workload's average dirty set is small relative to the 235-row viewport, so moving row reuse earlier would match the app pattern better than making QSG skip late.

4. Burst publication coalescing without DEC 2026 dependence.

The profile has 1,500 backend ingests, 1,498 snapshots, 662 rendered frames, and 112 snapshots superseded before render. Since synchronized output is not used, a safe backend-output coalescing or publication-cadence path could reduce redundant intermediate publications for bursty cursor-addressed animations. It must not cross terminal reply, input, resize, synchronized-output, or mode-change ordering barriers.

5. Duplicate dirty-mark compression near the mutator.

The dirty-row layer already coalesces, but 4.258 M duplicate dirty mark requests still flow through the system. The app pattern repeatedly mutates already-dirty rows before publication. A lightweight per-ingest or per-row pending-dirty guard closer to text/control mutators may reduce hot-path overhead, provided it preserves row generation and publication semantics.

6. Low-style-diversity row fast path.

The last frame has only two distinct styles, and cumulative coalescing is effective. Rows with few style segments could be represented as compact spans before renderer classification. This should be keyed by actual style runs, because cumulative style churn is phase-dependent and not always low.

### Lower-value or unsupported candidates

1. DEC synchronized-output fast path for Nelostie.

Not supported by this profile. All synchronized dirty-row counters are zero.

2. Scrollback append optimization for this workload.

Not supported by this profile. Text writes did not append scrollback, and public projection scroll counters are zero.

3. Line-wrap optimization for this workload.

Not supported by this profile. Text-write line wraps are zero.

4. General Unicode shaping optimization.

Not supported as the first target. The profile shows non-ASCII/graphic density, but all cells are single-width, with no multi-width cells, decorations, or hyperlinks.

5. Snapshot append as the only explanation.

Snapshot append remains expensive, but this focus should not over-weight it. Workload behavior also points to render-frame product reuse, graphic/non-ASCII classification, row-level publication cadence, and cursor-addressed burst tracing.

## What additional trace is needed

The current profile is good enough to identify workload shape but not good enough to assign terminal-pattern causality. Add a compact terminal-pattern trace or profile histogram for one Nelostie run with the same geometry.

Recommended trace fields:

| Trace area | Needed fields |
| --- | --- |
| Mode changes | Counts and timestamps for DECSET/DECRST 1047, 1048, 1049, 2026, bracketed paste, mouse tracking, origin mode, autowrap mode. |
| Cursor motion | Counts for CUP/HVP absolute movement, CUU/CUD/CUF/CUB relative movement, CR, LF, NEL, RI, save/restore cursor, tab, and cursor visibility toggles. |
| Erase/repaint | Counts for ED/EL variants, clear-screen, clear-line, scroll-region changes, full-row clears, full-screen clears, and resulting dirty-row ranges. |
| Print spans | Span length histogram, row/column start/end, ASCII vs non-ASCII vs graphic-classified cells, single-width vs multi-width, zero-width counts, and style id at write time. |
| Style churn | SGR sequence count, changed attributes per SGR, foreground/background changes, reset count, distinct style ids per ingest, style run count per dirty row. |
| Dirty publication | Dirty rows per ingest, rows dirtied repeatedly before publication, full-dirty cause, mark-all caller/reason, publication generation, render generation supersession. |
| Scroll behavior | Newline-induced scrolls, autowrap-induced scrolls, explicit scroll-region scrolls, scrollback append count, viewport offset changes, public projection scroll events. |
| Synchronized output | Entry/release timestamps, bytes and parser actions while held, rows collected while held, forced release events, stale-timeout release events. |
| Alternate screen | Active buffer at publication, enter/leave timestamps, clear-on-enter/leave flag, 1049 cursor-save/restore events. |
| Renderer row misses | For each dirty row rebuild, reason: content generation change, style-table change, graphic classification change, cursor overlap, IME overlap, selection overlap, missing old slot, descriptor mismatch, key mismatch. |

Preferred form:

- A small per-run summary histogram in the profile for stable metrics.
- An optional sampled JSONL trace for the first N seconds or for high-burst windows only, to avoid generating an unbounded artifact.
- A bucketed timeline aligned with existing 100 ms dirty-row buckets, so control-sequence bursts can be matched to dirty-row and render-frame bursts.

The most important missing distinction is whether full dirty events come from app-emitted clear/repaint sequences, alternate-screen transitions, style-table/global metadata changes, resize/geometry changes, or terminal-internal fallback. The current profile says full dirty happened; it does not say why.

## Risks

- The report infers workload behavior from aggregate profile counters because Nelostie source and raw terminal traces were not available under the requested roots.
- Alternate-screen use is plausible but not proven by the current profile. A direct active-buffer/mode counter is needed before building an alternate-screen-specific optimization.
- Renderer `frame_text_cells_rendered_as_graphic` means terminal renderer graphic classification, not necessarily application-level image protocols. Do not infer sixel/iTerm2 graphics from this counter.
- Renderer style-change counters measure rendered cell/run fragmentation, not necessarily one-to-one SGR emissions. The raw SGR histogram is still needed.
- A benchmark-specific fast path would be risky. Any optimization should target stable terminal invariants: single-width rows, dirty row identity, content generation, style run count, and explicit mode/scroll semantics.
- Coalescing backend output without DEC synchronized output is semantically sensitive. It must preserve ordering around replies, input-dependent modes, resize, alternate-screen transitions, synchronized-output boundaries, backend exit/error, and transcript capture.
- The workload appears phase-dependent. Last-frame statistics are much more ASCII-heavy and lower-style than cumulative statistics, so optimizing only the final visual state may miss earlier non-ASCII/graphic phases.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_a_end_to_end.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_b_snapshot.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\post_scalar_performance_audit_codex_c_qsg.md`
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\vnm_terminal_surface.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_session.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
