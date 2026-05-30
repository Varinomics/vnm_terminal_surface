# Residual compare audit: skeptical review of span-local result

## Executive summary

The span-local printable ASCII implementation appears to have removed the original full-row copy target. In the hardened profile, `printable_ascii_row_copies=55572`, `printable_ascii_row_copy_cells=48493629`, and the `Terminal_screen_model::apply_action::print_text::row_copy` scope costs 197.107 ms. In the span-local profile, `printable_ascii_row_copies=0`, `printable_ascii_row_copy_cells=0`, and no aggregate row-copy tree node remains under `apply_action::print_text`.

The residual compare attribution under `Terminal_screen_model::apply_action::print_text` is real in the hierarchical profile tree, not merely a dirty-row timeline artifact. The span-local profile has `Terminal_screen_model::advance_row_content_generation_if_changed::compare` as a child of `apply_action::print_text` with 876,318 calls and 4.5841783 s total. That does not mean the printable ASCII span-local path still performs full-row compares. Source inspection shows `write_printable_ascii_span` now uses `printable_ascii_span_changes_selection_content`, writes the span, and calls `advance_row_content_generation_with_change_flag` rather than `advance_row_content_generation_if_changed`.

The before/after performance comparison is noisy. The span-local profile processed 7,212,515 printable ASCII span characters versus 3,078,371 in hardened, or 2.34x more. It also has more snapshot requests, more dirty rows, more full repaint fallbacks, more `mark_all_dirty_calls`, and more render frames. Raw total timing is therefore not a clean A/B result.

The skeptical conclusion is: do not treat the residual `print_text` compare as proof that the span-local ASCII implementation failed. Treat it as evidence that broad compare producers still execute under the `print_text` action wrapper, especially non-ASCII, combining, wide-cell, and row edit paths.

## Possible attribution pitfalls

- The profile tree groups scopes by parent path and same child name. The same scope label appears in multiple places: span-local has a 4.5841783 s compare child under `apply_action::print_text`, a separate 17.5829 ms aggregate compare node elsewhere, and another 0.1847 ms node elsewhere.

- The timeline output prints `total_ns`, `mean_ns`, `min_ns`, and `max_ns` only. It does not print `self_ns` or `child_ns`. Summing timeline buckets for nested scopes can double-count wall time when a parent and child are both present in the same interval.

- The hierarchical tree does print `self_ns` and `child_ns`. For the residual compare question, the tree is the safer source: `apply_action::print_text` in span-local has 10.5048234 s total, 5.9206451 s self, and 4.5841783 s child time, with the compare scope accounting for that child time.

- The residual compare is real as a child of `apply_action::print_text`, but the parent scope wraps all of `put_text`, not just the printable ASCII span writer. Calls from `put_scalar`, `append_zero_width_scalar`, `install_cell_span`, and row mutation helpers can be charged under the same print-text action.

- `row_content_generation_comparisons` is a global model counter. In span-local it reports 887,639 comparisons and 772,753,829 comparison cells, but it does not identify the producer. It should not be used alone to attribute all comparison cells to printable ASCII spans.

- `printable_ascii_local_cells_inspected=5183956` is not equivalent to printable characters processed. The local span predicate short-circuits on the first changed cell, so it is a partial work counter and does not expose worst-case unchanged-span scan cost.

- The hardened and span-local captures are not equal workloads. Span-local has 81,631 `print_text_calls` versus 75,865, 1,919 render snapshot requests versus 1,586, 35,279 dirty snapshot rows versus 14,657, 87 `mark_all_dirty_calls` versus 18, and 843 frames versus 739.

## Correctness risks

- The span-local ASCII path depends on `printable_ascii_cell_changes_selection_content` exactly matching the selection-content effects of `clear_cell_at` plus the subsequent ASCII write. This is probably valid for the current code because the predicate compares text, display width, wide-continuation, and occupied state, but it is more manual and less conservative than the old before/after row comparison.

- Wide-cell boundary behavior is the main edge to protect. `clear_cell_at` resolves the base position and clears the whole span. A partial overwrite of a wide glyph should still be detected because the inspected base or continuation cell differs from the intended ASCII cell, but this needs explicit regression coverage before relying on further conversions.

- Style and hyperlink changes are intentionally excluded from both the old row comparison and the new local predicate. That is not a new span-local regression, but it remains a contract risk if any consumer treats retained-line `content_generation` as full cell identity rather than selection-content identity.

- The no-autowrap clipped branch uses the same change-flag approach and writes only the visible clipped cells. It should be covered separately because it does not share the normal `write_printable_ascii_span` path.

- Broad mutators are riskier to convert mechanically. `erase_row_range`, `erase_visible_screen`, `insert_cells`, and `delete_cells` can interact with wide spans, erased-cell style, retained-line identity, repaint recovery, and dirty marking. A simple `mark_terminal_content_changed` or command-presence flag is not a safe substitute for selection-content comparison.

- The residual compare cost may include non-ASCII and combining-scalar paths. `install_cell_span` still snapshots the row before clearing/writing, and `append_zero_width_scalar` can call the same broad compare path when a combined scalar overflows. These paths need behavior-specific tests before any local conversion.

## Incomplete conversion evidence

- `write_printable_ascii_span` has been converted to local inspection and `advance_row_content_generation_with_change_flag`.

- The no-autowrap clipped printable ASCII branch also calls `advance_row_content_generation_with_change_flag` after local inspection.

- `write_printable_ascii_cell` uses local inspection and `advance_row_content_generation_with_change_flag`.

- `resize_rows` still copies `before_cells` and calls `advance_row_content_generation_if_changed`.

- `append_zero_width_scalar` still copies `before_cells` and calls `advance_row_content_generation_if_changed` for the overflow/autowrap repair path.

- `install_cell_span` still copies `before_cells` and calls `advance_row_content_generation_if_changed`. This is the obvious remaining path for non-ASCII, wide, and combined scalar writes under `apply_action::print_text`.

- `erase_row_range` still copies `before_cells` and calls `advance_row_content_generation_if_changed`.

- `erase_visible_screen` still copies each row before filling it with erased cells unless primary repaint recovery replaces row identity.

- `insert_cells` and `delete_cells` still copy the row before shifting and repairing wide spans.

- The span-local profile still has 887,639 row-content comparisons and 772,753,829 comparison cells. The converted ASCII span path is not the only remaining producer.

## What to verify before further changes

- Add producer-specific attribution before converting more code: at minimum split comparison counters and timers by `install_cell_span`, `append_zero_width_scalar`, `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and resize.

- Re-run hardened and span-local profiles with tighter workload fingerprints: printable characters within 2 percent, `print_text_calls` within 5 percent, span calls within 5 percent, parser action-kind counts within 5 percent, and full repaint fallback counts within a small fixed bound.

- Verify the residual `print_text` compare by parent-path tree output, not by timeline totals. The tree should answer whether compare is under `apply_action::print_text`; timeline buckets should only be used for temporal correlation.

- Add or confirm regression cases for ASCII overwriting a wide base cell, ASCII overwriting a wide continuation cell, ASCII overwriting the cell adjacent to a wide span, no-autowrap clipping at the right margin, style-only rewrites, hyperlink-only rewrites, combining scalar append, and combining scalar overflow.

- Verify that retained-line `content_generation` consumers only require selection-content identity. If any renderer, selection lease, public projection, retained lookup, or snapshot cache consumer needs style or hyperlink identity, that is a larger pre-existing contract mismatch.

- Measure the local ASCII inspection directly. The current counter reports inspected cells but there is no dedicated timer around `printable_ascii_span_changes_selection_content`, so the replacement cost is inferred rather than directly timed.

## Recommendation

Do not roll back the span-local printable ASCII implementation based on the residual `print_text` compare. The row-copy target is gone, and the remaining compare under `print_text` is best explained by nested broad mutation paths still executed inside the print-text action wrapper.

Do not perform another broad conversion without better attribution. The next model-side step should be producer-specific measurement, followed by narrow conversions where the selection-content change flag can be proven locally. `install_cell_span` is the likely first model-side candidate because it sits on the non-ASCII/wide print-text path, but it needs wide-boundary and combining-scalar tests first.

For overall ROI, prioritize render-frame and snapshot pipeline work before chasing the last model-side comparisons. In the span-local profile, `build_terminal_render_frame` costs 30.9718748 s, `publish_render_snapshot` costs 12.6172887 s, `render_snapshot::append_rows` costs 11.6702693 s, and `sync_text_resource_nodes` costs 7.985688 s. The residual print-text compare is material at 4.5841783 s, but it is no longer the dominant pipeline cost.

## Files inspected

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`

- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`

- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`

- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\hierarchical_profiler.h`

- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
