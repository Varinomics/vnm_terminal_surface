# Residual compare audit E: counter integrity and before/after interpretation

## Executive summary

The span-local profile is only partially comparable to the hardened profile. It is comparable for counter-integrity checks because both captures use the same profile format, time unit, surface geometry, row count, column count, font, and device pixel ratio. It is not comparable as a simple before/after performance run because the workload changed materially.

The largest workload drift is printable text volume: hardened wrote 3,078,371 printable ASCII span characters, while span-local wrote 7,212,515, or 2.34x more. Span-local also produced 1.21x model render snapshots, 1.14x renderer frames, 2.41x requested dirty snapshot rows, 3.76x full-dirty renderer rows, 4.83x `mark_all_dirty_calls`, and 11x full-repaint/viewport fallbacks.

The strongest valid before/after signal is still the local-span optimization itself. Hardened reports 55,572 printable ASCII row copies and 48,493,629 copied cells; span-local reports zero row copies and 5,183,956 local cells inspected. Normalized per span, the old path copied about 872.63 cells/span while the span-local path inspected about 86.36 cells/span. Normalized per character, generation comparison work dropped from 265.83 comparison cells/char to 107.14 comparison cells/char, despite the larger span-local workload.

Raw timing totals for render snapshots, GUI synchronization, and render-thread painting are misleading before/after indicators. They increased mostly in areas where span-local had more snapshots, frames, dirty rows, and full repaint fallback work. Use character-, span-, snapshot-, row-, and frame-normalized metrics for interpretation until a tighter workload fingerprint is captured.

## Counter comparison

| Counter | Hardened | Span-local | Span / hardened | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Surface rows x columns | 235 x 873 | 235 x 873 | 1.00x | Geometry is comparable. |
| Printable ASCII span characters | 3,078,371 | 7,212,515 | 2.34x | Workload is not comparable by raw totals. |
| `print_text_calls` | 75,865 | 81,631 | 1.08x | Calls are close, but characters/call are not. |
| Printable ASCII span calls | 55,572 | 60,025 | 1.08x | Span count is close, but span size more than doubled. |
| Avg chars/span | 55.39 | 120.16 | 2.17x | Span-local processed much larger spans. |
| Avg chars/`print_text_call` | 40.58 | 88.36 | 2.18x | `print_text_calls` alone is not a workload proxy. |
| Row copies | 55,572 | 0 | 0.00x | Expected elimination in span-local path. |
| Row-copy cells | 48,493,629 | 0 | 0.00x | Old counter no longer has same semantics in span-local. |
| Local cells inspected | 0 | 5,183,956 | N/A | New span-local replacement counter. |
| Row-copy cells/span | 872.63 | 0.00 | 0.00x | Old path effectively copied nearly a full row per span. |
| Local inspected cells/span | 0.00 | 86.36 | N/A | New path inspects a narrower local range. |
| Row generation comparisons | 937,538 | 887,639 | 0.95x | Total comparisons fell despite higher text volume. |
| Row generation comparison cells | 818,329,995 | 772,753,829 | 0.94x | Total comparison cells fell slightly; per character fell sharply. |
| Row generation advances | 274,064 | 311,440 | 1.14x | More content changes occurred in span-local. |
| Dirty marks from text writes | 166,716 | 180,075 | 1.08x | Tracks span count pattern, not unique dirty rows. |
| Dirty row mark requests | 2,856,477 | 2,931,193 | 1.03x | Dominated by duplicate/non-text marks; weak before/after signal. |
| Unique pending row marks | 113,513 | 180,086 | 1.59x | Sensitive to publication cadence and fallback behavior. |
| Dirty snapshot calls | 1,592 | 1,921 | 1.21x | Matches higher snapshot activity. |
| Dirty snapshot rows | 14,657 | 35,279 | 2.41x | Major workload/fallback drift. |
| Published unique rows | 14,286 | 32,543 | 2.28x | Not a distinct changed-row total; publication cadence matters. |
| Render snapshot requests | 1,586 | 1,919 | 1.21x | Higher model publication workload. |
| Snapshot cells scanned | 319,373,307 | 379,139,110 | 1.19x | Scales with snapshot count and materialized rows. |
| Snapshot cells emitted | 142,378,029 | 172,488,235 | 1.21x | Scales with snapshot count. |
| Full repaint fallbacks | 2 | 22 | 11.00x | Breaks raw dirty-row comparability. |
| Renderer frames | 739 | 843 | 1.14x | Render-thread totals need per-frame normalization. |
| Renderer dirty rows | 13,043 | 26,128 | 2.00x | Inflated by more full-dirty frames. |
| Renderer full dirty rows | 3,933 | 14,790 | 3.76x | Strong evidence of fallback/workload drift. |
| Text content rebuilds | 8,511 | 15,363 | 1.81x | Not attributable to span-local alone because dirty rows doubled. |
| Text resource runs before coalescing | 4,791,082 | 9,090,234 | 1.90x | Mostly workload-driven. |
| Text resource runs after coalescing | 249,229 | 286,043 | 1.15x | Coalescing kept output growth much lower than input growth. |

Key timing totals:

| Scope | Hardened total | Hardened mean | Span-local total | Span-local mean | Interpretation |
| --- | ---: | ---: | ---: | ---: | --- |
| `Terminal_screen_model::apply_action::print_text` | 11.238 s | 148.13 us/call | 10.505 s | 128.69 us/call | Improved by call and strongly by character. |
| `Terminal_screen_model::apply_action::print_text::row_copy` | 0.197 s | 3.55 us/call | 0.000 s | N/A | Removed from span-local profile. |
| `advance_row_content_generation_if_changed::compare` | 4.726 s | 5.04 us/call | 4.602 s | 5.18 us/call | Total down slightly; per call slightly worse; per character much better. |
| `render_snapshot::append_rows` | 9.190 s | 5.79 ms/snapshot | 11.693 s | 6.09 ms/snapshot | Worse raw total and per snapshot; not a span-local proof. |
| `publish_render_snapshot` | 9.815 s | 6.19 ms/call | 12.641 s | 6.59 ms/call | Higher snapshot count and dirty-row load. |
| `VNM_TerminalSurface::sync_from_session` | 2.049 s | 1.18 ms/call | 2.891 s | 1.11 ms/call | Total up from more calls; per call slightly lower. |
| `VNM_TerminalSurface::updatePaintNode` | 35.788 s | 48.43 ms/frame | 44.541 s | 52.84 ms/frame | Raw total and per-frame worse; affected by more dirty/full rows. |
| `build_terminal_render_frame` | 25.273 s | 34.20 ms/frame | 30.972 s | 36.74 ms/frame | Worse per frame; not comparable without equal frame workload. |
| `sync_text_resource_nodes` | 5.733 s | 7.76 ms/frame | 7.986 s | 9.47 ms/frame | Dirty-row/rebuild workload changed. |
| `text_resource_row_descriptor` | 0.992 s | 9.35 us/call | 1.481 s | 12.53 us/call | Renderer-side text descriptor work worsened. |

## Normalized interpretation

| Normalized metric | Hardened | Span-local | Interpretation |
| --- | ---: | ---: | --- |
| `print_text` ns/printable char | 3,650 ns | 1,456 ns | Strong favorable signal for span-local. |
| Generation compare ns/printable char | 1,535 ns | 638 ns | Strong favorable signal, despite slightly worse ns/compare call. |
| Generation comparison cells/printable char | 265.83 | 107.14 | Strong favorable signal for local range comparison. |
| Dirty marks from text/printable char | 0.0542 | 0.0250 | Lower per character, but this is implementation-shape dependent. |
| Unique pending row marks/printable char | 0.0369 | 0.0250 | Favorable, but cadence/fallback sensitive. |
| Dirty snapshot rows/snapshot | 9.24 | 18.38 | Worse span-local profile workload. |
| Dirty snapshot rows/printable char | 0.00476 | 0.00489 | Nearly flat after character normalization. |
| Snapshot cells scanned/snapshot | 201,370 | 197,571 | Comparable; slightly lower span-local. |
| Snapshot cells emitted/snapshot | 89,772 | 89,884 | Essentially unchanged. |
| Renderer dirty rows/frame | 17.65 | 30.99 | Worse span-local render workload. |
| Renderer cells considered/frame | 86,867 | 85,941 | Comparable per frame. |
| Renderer dirty rows/printable char | 0.00424 | 0.00362 | Lower per character, but this hides fallback concentration. |
| Snapshots/frame | 2.15 | 2.28 | Slightly more model publications per rendered frame. |
| Printable chars/frame | 4,166 | 8,556 | Render thread saw much denser text workload per frame. |

The before/after interpretation should be split by subsystem:

- Model text-write path: comparable enough after normalizing by printable characters and span calls. Span-local removed row-copy work and reduced character-normalized comparison/timing cost.
- Model snapshot path: not comparable by raw totals. Snapshot count and dirty rows increased; per-snapshot emitted/scanned cells are roughly stable, while append/publish timing is slightly worse.
- Renderer path: not comparable by raw totals. Span-local has more frames, more dirty rows/frame, many more full-dirty rows, more text rebuilds, and more text-resource churn.
- Dirty-row publication path: partially comparable only after normalizing by characters or snapshots. Raw unique/published dirty-row counters are distorted by publication cadence, fallback count, and coalescing.

## Counter caveats

`print_text_calls` and `printable_ascii_span_calls` are not sufficient workload denominators. Span-local has only about 8 percent more calls but 134 percent more printable characters, so call-normalized conclusions can still be wrong.

`printable_ascii_row_copies` and `printable_ascii_row_copy_cells` are valid for detecting the old full-row copy path, but they are not directly comparable to `printable_ascii_local_cells_inspected`. The latter measures the new local inspection work, not avoided copies. The useful comparison is normalized magnitude: old copied cells/span versus new inspected cells/span.

`dirty_marks_from_text_writes` is misleading as a dirty-row count. In both captures it equals three times the printable span count, which indicates it reflects mark calls made while processing printable text, not unique dirty rows or rows that ultimately reach a renderer frame.

`mark_requests` is dominated by duplicate mark requests. Hardened has 2,742,964 duplicate mark requests out of 2,856,477 total; span-local has 2,751,107 out of 2,931,193. This counter is useful for duplicate pressure, but poor for comparing actual repaint work.

`unique_pending_row_marks`, `dirty_rows_snapshot_rows`, and `published_unique_rows` are cadence-sensitive. They depend on when pending rows are published or snapshotted, how coalescing batches updates, and whether full repaint fallbacks occur. They should not be interpreted as distinct changed content rows.

`render_snapshot_dirty_rows_requested` and `render_snapshot_dirty_rows_visible` match the dirty-row snapshot row totals in these profiles, which is good for counter integrity. They still become misleading when full-repaint/viewport fallback counts differ by 11x.

`render_snapshot_rows_visited`, `render_snapshot_rows_materialized`, `render_snapshot_cells_scanned`, and `render_snapshot_cells_emitted` are internally coherent with geometry and snapshot count. They mainly explain snapshot construction work, not text-write optimization impact.

Renderer `frame_dirty_rows`, `frame_full_dirty_rows`, `text_dirty_rows`, and `text_dirty_rows_rebuilt` are frame-level totals. The same logical row can be counted in multiple frames, and full-dirty frames inflate them. They are useful for render workload accounting, not direct changed-row accounting.

Renderer text cache counters such as `text_content_rebuilds`, `text_cache_entries_created`, `text_resource_runs_before_coalescing`, and `text_resource_runs_after_coalescing` are affected by dirty-row volume, retained row identity, cache-key stability, and coalescing. They should be normalized by frames, dirty rows, text groups, and runs before being used for before/after claims.

Timing tree totals can include same-named scopes from multiple call sites. Exact-name aggregation is useful for totals, but any repeated scope name should be treated as aggregate instrumentation, not a single source location, unless the tree parent is also considered.

## Additional counters needed

Add a compact workload fingerprint to each profile before the next optimization comparison:

- Backend bytes ingested, parser action count by action kind, printable character count, non-printable/control-sequence count, scrollback append count, resize count, and cursor/style mutation count.
- A deterministic profile input identifier: command/script name, captured transcript hash, environment hash, terminal size, font key, and run duration or completion marker.
- Counts for full repaint fallback causes, viewport fallback causes, and `mark_all_dirty` causes.

Add span-local-specific counters:

- Local inspected cells split by changed cells, unchanged cells, skipped leading/trailing cells, and bounds-clipped cells.
- Spans with no content change, spans with partial change, spans changing whole touched range, and rows advanced by printable spans.
- Dirty rows produced by printable spans, distinct rows touched by printable spans, and dirty marks per source.
- A timer around the local span inspection/selection-content check so the replacement work is timed directly rather than inferred from aggregate compare scopes.

Add dirty-row publication counters:

- Dirty rows before coalescing, after coalescing, after full-repaint expansion, and after viewport clipping.
- Dirty range count per snapshot and cumulative dirty row-ranges published.
- Distinct logical rows dirtied by source, separately from repeated frame/snapshot publication counts.
- Snapshot supersession rows: rows built but never rendered, and rows rendered after coalescing.

Add renderer counters:

- Text row cache hit/miss reason counts: clean skip, descriptor reuse, key match, dirty rebuild, missing old slot, frame-key mismatch, provenance invalid, and fallback.
- Text-resource key bytes and descriptor bytes per frame, normalized by text groups and dirty groups.
- Build-frame input breakdown by dirty rows versus full rows, and by text/graphic/cursor-only changes.
- Per-frame percentiles or histograms for dirty rows/frame, text groups/frame, text runs before/after coalescing, and `updatePaintNode` time.

## Profile acceptance criteria

For the next optimization profile pair, accept a before/after comparison only if these conditions hold:

- Same profile format, time unit, rows, columns, font family, font size, device pixel ratio, and profiling build flags.
- Same deterministic input or matching transcript/input hash.
- Printable characters within +/- 2 percent, `print_text_calls` within +/- 5 percent, span calls within +/- 5 percent, parser action counts by kind within +/- 5 percent, and control sequence counts within +/- 5 percent.
- Render snapshot requests within +/- 5 percent, renderer frames within +/- 5 percent, and snapshots/frame within +/- 5 percent unless the optimization intentionally changes publication cadence.
- `mark_all_dirty_calls`, full repaint fallbacks, viewport fallbacks, resize counts, and scrollback append counts must match exactly or have documented cause-level differences.
- Dirty snapshot rows/snapshot and renderer dirty rows/frame should be within +/- 10 percent unless dirty-row behavior is the optimization target.
- Report both raw totals and normalized values: per printable character, per span, per `print_text_call`, per snapshot, per dirty row, and per renderer frame.
- Treat timing changes as accepted only when the relevant workload denominator is stable and at least one direct replacement counter/timer confirms the intended mechanism.
- Run at least three captures per side or add percentile/histogram counters before claiming small timing differences. Single-run timing should only support large, mechanism-backed changes.

Under these criteria, the current pair should be accepted for validating that row-copy work was removed and local inspection work is substantially smaller per span/character. It should not be accepted for end-to-end render-thread or wall-time before/after conclusions.

## Files inspected

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_session.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_renderer.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
