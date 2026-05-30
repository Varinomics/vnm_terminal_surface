# Residual Compare Final Consolidation - Bacon

## Executive summary

The residual comparison problem is real, well-attributed, and narrower than several reports make it sound.

The printable-ASCII span-local change succeeded. The old row-copy path is gone in `nelostie_profile_span_local.txt`: `printable_ascii_row_copies=0` and `printable_ascii_row_copy_cells=0`, compared with `55572` row copies and `48493629` copied cells in the hardened profile. Do not roll that work back and do not keep investigating printable ASCII as the source of the remaining full-row comparisons.

The remaining model-side full-row comparison hotspot is under `Terminal_screen_model::apply_action::print_text`: the span-local profile records `876318` calls and `4584178300 ns` in `Terminal_screen_model::advance_row_content_generation_if_changed::compare` under `print_text`. Source shape confirms the likely producers are non-ASCII scalar install paths: `install_cell_span` and the `append_zero_width_scalar` autowrap source-row clear. These still snapshot `screen_row.cells` and call `advance_row_content_generation_if_changed`.

The next residual-compare implementation slice should be tests-first, then convert `install_cell_span` and the `append_zero_width_scalar` overflow clear to span-local selection-content detection using the existing `advance_row_content_generation_with_change_flag` path. Do not include `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, `resize_rows`, render snapshot materialization, or QSG frame construction in that first residual-compare slice.

The strongest skeptical caveat is also valid: the two profile captures are not clean A/B runs. Span-local processed `7212515` printable ASCII span characters versus `3078371` in hardened. Use the profiles for mechanism validation and current residual ranking, not raw end-to-end before/after claims.

## Validated measured evidence

| Evidence | Hardened profile | Span-local profile | Consolidated interpretation |
| --- | ---: | ---: | --- |
| Surface geometry | 235 x 873 | 235 x 873 | Geometry is comparable. |
| `printable_ascii_span_characters` | 3,078,371 | 7,212,515 | Workload is not comparable by raw totals. |
| `print_text_calls` | 75,865 | 81,631 | Call counts are close, but text volume differs materially. |
| `printable_ascii_span_calls` | 55,572 | 60,025 | Span count is close; span-local spans are much larger on average. |
| `printable_ascii_row_copies` | 55,572 | 0 | ASCII row-copy path was eliminated. |
| `printable_ascii_row_copy_cells` | 48,493,629 | 0 | Old full-row copy target is gone. |
| `printable_ascii_local_cells_inspected` | absent/0 | 5,183,956 | Replacement local work is visible. |
| `row_content_generation_comparisons` | 937,538 | 887,639 | Full-row comparisons remain, but are no longer ASCII row-copy evidence. |
| `row_content_generation_comparison_cells` | 818,329,995 | 772,753,829 | Remaining comparisons still average near full-row width. |
| `row_content_generation_advances` | 274,064 | 311,440 | Different workload; use only as same-input semantic parity after implementation. |
| `print_text` total | 11.238 s | 10.505 s | Still material. Raw comparison is noisy. |
| `compare` under `print_text` | 935,526 calls, 4.718 s | 876,318 calls, 4.584 s | Residual compare attribution under `print_text` is real. |
| `control_sequence` child compare | 1,468 calls, 7.675 ms | 6,127 calls, 17.583 ms | Real but small relative to print residual. |

Validated source facts:

| Source fact | Consolidated conclusion |
| --- | --- |
| `write_printable_ascii_span`, no-autowrap clipped ASCII, and `write_printable_ascii_cell` already use local selection-content predicates and `advance_row_content_generation_with_change_flag`. | The residual `print_text` compare is not caused by the converted ASCII span writer. |
| `install_cell_span` still copies `screen_row.cells` and calls `advance_row_content_generation_if_changed`. | This is the highest-confidence residual print-text producer. |
| `append_zero_width_scalar` autowrap overflow still copies `screen_row.cells` around `clear_cell_at(target)` before wrapping. | This is a secondary residual print-text producer and should be converted with the same helper family. |
| `cells_have_same_selection_content` compares `text`, `display_width`, `wide_continuation`, and `occupied`. | The next helper must preserve that exact current generation decision unless a separate contract migration changes semantics. |
| `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and `resize_rows` still use full-row comparison. | These are later residual producers, not the first print-focused slice. |

## Report quality assessment

| Report | Quality | Use in this consolidation |
| --- | --- | --- |
| `residual_compare_audit_codex_a_callers.md` | Strong | Correctly identifies the residual `print_text` caller chain and distinguishes converted ASCII paths from non-ASCII scalar paths. |
| `residual_compare_audit_codex_b_broad_mutators.md` | Strong but broader than the first slice | Good producer inventory and later-slice guidance. Its broad-mutator plan should not displace the focused print residual slice. |
| `residual_compare_audit_codex_c_consumers.md` | Mixed | Useful consumer map, but overstates the current producer contract by saying generation must advance for style and hyperlink changes. Current source comparison excludes those fields, so that claim is not valid for this slice. |
| `residual_compare_audit_codex_d_tests.md` | Strong | Best tests-first framing. Correctly argues for a selection-content parity oracle rather than timing-only validation. |
| `residual_compare_audit_codex_e_counter_integrity.md` | Strong | Best normalization and counter-integrity report. Its warning about non-identical workloads should govern profile interpretation. |
| `residual_compare_audit_codex_f_pipeline_roi.md` | Good global ROI report, weak as residual-compare sequencing | Correct that render-frame/snapshot/QSG costs are larger post-span-local. Not a reason to mix those architectural changes into the residual compare slice. |
| `residual_compare_audit_codex_g_design.md` | Strong | Best concrete API/migration shape for the immediate implementation slice. |
| `residual_compare_audit_codex_h_skeptical_review.md` | Strong | Correctly rejects rollback and explains attribution pitfalls. |
| `residual_compare_audit_claude_01_print_subpaths.md` | Strong | Good source-level print-subpath findings and missing-test list. |
| `residual_compare_audit_claude_02_broad_mutators.md` | Useful but too eager to broaden | Good broad-mutator details and idempotency gaps; first slice should still stay on `install_cell_span` and zero-width overflow. |
| `residual_compare_audit_claude_03_profile_normalization.md` | Thin artifact, useful points | The artifact is not a full report, but its key conclusions about attribution, missing counters, and dead zero counters are relevant. |
| `residual_compare_audit_claude_04_roi_strategy.md` | Mixed | Correctly highlights bigger snapshot/render costs, but recommends snapshot clean-row omission with contract implications that are outside this residual-compare consolidation. |

## Confirmed root cause

The root cause of the remaining residual comparison cost under `print_text` is that non-ASCII scalar writes still use the old full-row before/after comparison mechanism even though their mutation footprint is local.

Confirmed path family:

| Path | Current problem | Correct replacement shape |
| --- | --- | --- |
| `put_text -> put_scalar -> put_spacing_scalar -> place_cell_text -> install_cell_span` | Full `screen_row.cells` copy and full-row selection-content comparison. | Precompute local selection-content change over the old wide-span union and new scalar span, then call `advance_row_content_generation_with_change_flag`. |
| `put_text -> put_scalar -> append_zero_width_scalar -> install_cell_span` | Same full-row comparison through `install_cell_span`. | Same scalar-span local detector. |
| `put_text -> put_scalar -> append_zero_width_scalar` autowrap overflow source-row clear | Full-row copy to detect clearing one old occupied base/wide span before wrapping. | Local clear detector over the old base span, then call `advance_row_content_generation_with_change_flag`. |

The detector must compare the same selection-content fields as the current full-row comparator. It must not include style or hyperlink fields unless the project deliberately changes the retained-content generation contract in a separate migration.

## What not to do

- Do not roll back the printable-ASCII span-local implementation.
- Do not interpret the residual `print_text` compare as proof that the ASCII span-local path failed.
- Do not use raw hardened-versus-span-local wall time as a clean A/B result; the workloads differ materially.
- Do not blanket-convert `install_cell_span`, erase, insert, or delete paths to `changed=true`.
- Do not advance content generation for style-only or hyperlink-only rewrites in this slice; current comparison semantics exclude those fields.
- Do not add checksums or fingerprints to the production generation decision. They do not remove the need to reason about the touched wide/span region.
- Do not remove `advance_row_content_generation_if_changed` in the first slice; later broad mutators still use it.
- Do not combine render snapshot materialization, QSG frame reuse, broad mutator conversion, and non-ASCII print conversion into one batch.
- Do not make primary repaint recovery depend on this residual-compare optimization.
- Do not accept a profile improvement without the generation parity tests passing first.

## Recommended next implementation slice

Implement one focused residual-compare slice:

1. Add tests for non-ASCII scalar generation parity before source changes.
2. Add a private scalar-span local detector next to the existing printable-ASCII detectors.
3. Convert `install_cell_span` to use that detector and `advance_row_content_generation_with_change_flag`.
4. Convert the `append_zero_width_scalar` autowrap source-row clear to local clear detection plus `advance_row_content_generation_with_change_flag`.
5. Add `scalar_span_local_cells_inspected` or equivalent profile counter and emit it wherever model profile counters are already emitted.
6. Re-profile the same Nelostie workload shape and verify that `advance_row_content_generation_if_changed::compare` is absent or negligible under `apply_action::print_text`.

Recommended helper/API shape:

```cpp
bool scalar_span_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position,
    QStringView                text,
    int                        display_width) const;

bool scalar_span_clear_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position) const;
```

Helper rules:

| Rule | Requirement |
| --- | --- |
| Comparison authority | Reuse `cells_have_same_selection_content`. Do not duplicate the field list elsewhere. |
| Wide-span safety | Compute `cell_base_position(position)` before mutation and include the old base span in the inspected region. |
| New span safety | Include `[position.column, position.column + display_width)` in the inspected region. |
| Style/hyperlink | Preserve current no-generation behavior for style-only and hyperlink-only rewrites. |
| Counters | Count inspected scalar-span cells separately from printable ASCII local inspections. |
| Cleanup | Do not keep a production fallback to the full-row compare for these converted paths. |

## Tests-first gate

Minimum hard gate before implementation:

| Test | Required result |
| --- | --- |
| Identical CJK/wide scalar rewrite | Retained id stays stable; `content_generation` does not advance. |
| Different CJK/wide scalar rewrite | Retained id stays stable; `content_generation` advances exactly once. |
| Same non-ASCII text with style-only change | `content_generation` does not advance. |
| Same non-ASCII text with hyperlink-only change | `content_generation` does not advance. |
| Non-ASCII overwrite of a wide base | Advances exactly when selection content changes. |
| Non-ASCII overwrite of a wide continuation | Advances exactly when selection content changes and old wide span cleanup is represented. |
| Combining mark append without overflow | Advances when combined text changes selection content. |
| Combining mark autowrap overflow | Source-row clear and destination-row install make separate correct generation decisions. |
| Snapshot provenance smoke | Render snapshot visible-line provenance carries the model generation after changed and unchanged cases. |

The oracle should be the current selection-content signature: `occupied`, `text`, `display_width`, and `wide_continuation`. Style, hyperlink, dirty state, cursor state, and renderer cache state are excluded for this slice.

## Later slices

Order later work by measured residual after the focused print slice lands:

1. Broad mutator attribution counters.

   Add producer-specific comparison counters or scopes for `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and `resize_rows` before changing them. The global `row_content_generation_comparisons` counter is not enough once the `print_text` residual is removed.

2. Erase paths.

   Convert `erase_row_range` and `erase_visible_screen` only after idempotency tests exist for already-erased ranges/screens. Use local scan-without-copy against `erased_cell()`, not unconditional `true`.

3. Insert/delete cell paths.

   Convert only after no-op shift tests exist. Detection must include suffix movement, tail/insertion fill, wide-boundary clears, and wide repair effects.

4. Resize and wide-repair cleanup.

   Low priority. Useful only if residual counters show it matters or if helper consolidation makes it cheap.

5. Render-frame/snapshot/QSG pipeline work.

   This may be higher global ROI than residual comparison. Treat it as a separate governed optimization stream with its own contracts. In particular, omitting clean rows from full snapshots or changing `viewport_row_cells` return shape is not a residual compare cleanup; it changes snapshot/frame data contracts and needs independent tests.

## Re-profile criteria

Use the profiles for mechanism validation, with stable workload requirements.

Required same-input or tightly matched conditions:

| Dimension | Required stability |
| --- | --- |
| Geometry/font/device pixel ratio | Exact match. |
| Input/transcript identity | Exact match preferred; otherwise documented command and workload fingerprint. |
| Printable characters | Within 2 percent. |
| `print_text_calls` and ASCII span calls | Within 5 percent. |
| Parser action counts by kind | Within 5 percent. |
| Render snapshot requests and frames | Within 5 percent unless the slice intentionally changes publication cadence. |
| Full repaint fallback and `mark_all_dirty` causes | Exact match or cause-level explanation. |

Immediate post-slice success criteria:

| Metric | Expected result |
| --- | --- |
| `printable_ascii_row_copies` | Remains 0. |
| `printable_ascii_row_copy_cells` | Remains 0. |
| `advance_row_content_generation_if_changed::compare` under `apply_action::print_text` | Drops from `876318` calls / `4.584 s` to negligible or absent. |
| `row_content_generation_comparisons` | Drops by approximately the removed print-text residual share for the same workload. |
| `row_content_generation_comparison_cells` | Drops by approximately full-row width times the removed print-text residual calls. |
| `row_content_generation_advances` | Same-input semantic parity; any difference must be tied to a discovered existing bug or an intentional contract change. |
| New scalar local inspection counter | Increases and explains replacement local work. |
| `apply_action::print_text` total/mean | Drops materially without equivalent new local-detector time growth. |

Do not require broad-mutator comparison counts to reach zero in this slice. Remaining comparisons after the slice should be attributable to non-print producers.

## Risks and open questions

| Risk/open question | Consolidated answer |
| --- | --- |
| Does content generation mean full render payload identity or selection-content identity? | Current producer comparator says selection-content identity: `text`, `display_width`, `wide_continuation`, `occupied`. Reports that require style/hyperlink bumps conflict with current source and should trigger a separate contract discussion, not this slice. |
| Can the helper miss a wide base when writing into a continuation? | Yes unless it computes `cell_base_position(position)` before mutation. This is the main correctness risk. |
| Can the helper over-advance idempotent wide/non-ASCII rewrites? | Yes unless tests explicitly cover identical scalar rewrites and style/hyperlink-only rewrites. |
| Is `append_zero_width_scalar` overflow worth converting? | Yes as part of the same helper family. Its standalone ROI is smaller, but leaving it behind keeps an avoidable compare under `print_text`. |
| Should broad mutators be converted in the same batch? | No. They need idempotency tests and producer-specific attribution first. |
| Are checksums/fingerprints useful? | Not in production. A temporary debug-only oracle may help triage future mismatches, but should not become the generation decision. |
| Is render/snapshot pipeline work higher ROI? | Possibly yes globally, but it is a separate optimization stream with different contracts. This consolidation is the residual-comparison action plan. |
| Are current profiles enough for final performance claims? | No. They prove mechanism and current residual attribution, not clean end-to-end before/after speedup. |

## Source reports considered

| Report | Considered result |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_a_callers.md` | Primary caller attribution for residual `print_text` comparisons. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_b_broad_mutators.md` | Broad-mutator inventory and later-slice guidance. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_c_consumers.md` | Consumer map; style/hyperlink advancement claim rejected for this slice. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_d_tests.md` | Tests-first gate and parity-oracle strategy. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_e_counter_integrity.md` | Counter normalization and profile caveats. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_f_pipeline_roi.md` | Global pipeline ROI context, separated from residual-compare action. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_g_design.md` | Immediate helper/API and migration design. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_h_skeptical_review.md` | Attribution pitfalls and rollback rejection. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_01_print_subpaths.md` | Print subpath source audit and missing-test list. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_02_broad_mutators.md` | Broad-mutator findings and idempotency gaps. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_03_profile_normalization.md` | Useful summarized normalization and missing-counter claims, but not a full report artifact. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_04_roi_strategy.md` | Broader ROI claims; snapshot/render recommendations treated as separate-stream work. |

Profile sources considered:

| Source | Use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt` | Hardened baseline mechanism counters. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt` | Post-span-local residual attribution and current target sizing. |

Source files used for validation from prior inspection:

| Source | Use |
| --- | --- |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h` | Profile counters and private helper declarations. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp` | Generation comparator, ASCII local detectors, `install_cell_span`, `append_zero_width_scalar`, and broad mutator call sites. |
| `C:\plms\varinomics\vnm_terminal\src\main.cpp` | Profile text emission for model counters. |
| `C:\plms\varinomics\vnm_terminal_surface\benchmarks\embedded_terminal\embedded_terminal_benchmark.cpp` | Benchmark/profile output surface for counter propagation. |
