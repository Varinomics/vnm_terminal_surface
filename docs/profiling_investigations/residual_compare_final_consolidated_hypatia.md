# Residual Compare Final Consolidation - Hypatia

## Executive summary

The span-local ASCII conversion succeeded. The post-change profile reports `printable_ascii_row_copies=0` and `printable_ascii_row_copy_cells=0`, and the ASCII writer source now uses local change flags rather than `advance_row_content_generation_if_changed`.

The remaining residual comparison problem is still real and still mostly inside `print_text`: `nelostie_profile_span_local.txt` attributes 876,318 `advance_row_content_generation_if_changed::compare` calls and 4.584 s directly under `Terminal_screen_model::apply_action::print_text`. That is 98.7% of the global 887,639 row-content comparisons in the profile.

The confirmed root cause is not the converted ASCII span path. It is the non-ASCII scalar path, primarily `install_cell_span`, with a smaller `append_zero_width_scalar` autowrap clear path. These still copy a full row and run a full-row selection-content comparison even though their mutation footprint is local.

The next implementation slice should be narrow: add tests first, then convert `install_cell_span` and the `append_zero_width_scalar` autowrap source-row clear to local selection-content detection plus `advance_row_content_generation_with_change_flag`. Do not convert broad control-sequence mutators in the same slice. Do not change the generation contract to include style or hyperlink unless the product explicitly decides to change that contract.

## Validated measured evidence

The two profile captures are not clean A/B performance runs, but they are valid for mechanism checks.

| Metric | Hardened | Span-local | Interpretation |
| --- | ---: | ---: | --- |
| Geometry | 235 x 873 | 235 x 873 | Comparable row width and surface size. |
| `print_text_calls` | 75,865 | 81,631 | Similar count, but not identical workload. |
| Printable ASCII span characters | 3,078,371 | 7,212,515 | Span-local processed 2.34x more printable characters. |
| `printable_ascii_row_copies` | 55,572 | 0 | ASCII row-copy target was eliminated. |
| `printable_ascii_row_copy_cells` | 48,493,629 | 0 | Full-row ASCII copy counter was eliminated. |
| `printable_ascii_local_cells_inspected` | 0 | 5,183,956 | Local ASCII detector replaced row copy work. |
| `row_content_generation_comparisons` | 937,538 | 887,639 | Full-row comparisons remain. |
| `row_content_generation_comparison_cells` | 818,329,995 | 772,753,829 | Remaining comparisons still scan nearly full rows. |
| `row_content_generation_advances` | 274,064 | 311,440 | More content changes occurred in the span-local run. |

Post-change parent-path evidence is decisive:

| Scope in span-local profile | Calls | Total | Mean | Meaning |
| --- | ---: | ---: | ---: | --- |
| `Terminal_screen_model::apply_action::print_text` | 81,631 | 10.505 s | 128.686 us | Still material. |
| Child `advance_row_content_generation_if_changed::compare` under `print_text` | 876,318 | 4.584 s | 5.231 us | Residual full-row compare under print action. |
| Global `row_content_generation_comparisons` | 887,639 | N/A | N/A | 98.7% of comparisons are under `print_text`. |

The average span-local global comparison scans 870.6 cells, effectively the 873-column row width. That proves the remaining work is still row-width work, not a narrow local detector.

The before/after workload drift matters. Raw renderer and snapshot timing deltas should not be used as direct proof of span-local benefit or regression. The valid conclusion is mechanism-level: ASCII row copies are gone, but full-row generation comparisons remain under `print_text`.

## Report quality assessment

Strong reports:

| Report | Assessment |
| --- | --- |
| `residual_compare_audit_codex_a_callers.md` | Strong. Correctly isolates residual `print_text` callers, separates converted ASCII paths from non-ASCII scalar paths, and gives a practical next slice. |
| `residual_compare_audit_codex_e_counter_integrity.md` | Strong. Correctly warns that the profiles are not equal workloads and gives useful normalized interpretation. This should govern all performance claims. |
| `residual_compare_audit_codex_g_design.md` | Strong. Best implementation design for the immediate residual compare slice: helper, tests, counters, narrow migration. |
| `residual_compare_audit_codex_h_skeptical_review.md` | Strong. Correctly rejects the false claim that ASCII span-local failed, and validates parent-path attribution. |
| `residual_compare_audit_claude_01_print_subpaths.md` | Strong. Good caller-level diagnosis and test list for `install_cell_span` and combining/autowrap. |

Useful but lower-priority reports:

| Report | Assessment |
| --- | --- |
| `residual_compare_audit_codex_b_broad_mutators.md` | Useful inventory of broad mutators, but it should not drive the first slice. The post-change parent tree shows the urgent cost is under `print_text`, not broad control-sequence mutators. |
| `residual_compare_audit_codex_d_tests.md` | Useful test framing, but partly stale: it still emphasizes printable/no-autowrap ASCII even though the ASCII paths are already converted. Keep the parity-oracle idea, redirect it to non-ASCII scalar installs. |
| `residual_compare_audit_codex_f_pipeline_roi.md` | Useful broader performance context. It is right that render-frame/snapshot/QSG costs are larger than the residual compare, but that is a different performance track from this residual comparison consolidation. |
| `residual_compare_audit_claude_02_broad_mutators.md` | Useful missing-test list for broad mutators, but overstates immediate severity for `erase_row_range` and `erase_visible_screen` relative to the measured `print_text` parent attribution. |
| `residual_compare_audit_claude_04_roi_strategy.md` | Useful render/snapshot ROI ideas, but proposes a large snapshot contract change that should not be mixed into the residual compare slice. |

Weak or misleading reports/claims:

| Report or claim | Problem |
| --- | --- |
| `residual_compare_audit_codex_c_consumers.md` required invariant that generation must advance for style/hyperlink changes | This conflicts with current source semantics: `cells_have_same_selection_content` compares text, display width, wide-continuation, and occupied state only. Style and hyperlink are intentionally excluded today. Treat this as a possible future contract discussion, not a requirement for this slice. |
| `residual_compare_audit_claude_03_profile_normalization.md` | Too thin as a final report artifact. The summary points are mostly correct, but it does not provide enough source-level or action-level detail to lead implementation. |
| Claims that residual `print_text` compare proves ASCII span-local failed | Incorrect. Source and counters show ASCII row copies are gone and ASCII paths call `advance_row_content_generation_with_change_flag`. |
| Claims that broad mutators explain the 876,318 residual `print_text` compare calls | Overbroad. Broad mutators exist, but the measured parent-path residual is under `print_text`; the primary reachable source is scalar installation, not ED/EL/ICH/DCH. |

## Confirmed root cause

The current retained-line generation comparison contract is implemented by `rows_have_same_selection_content`, which compares only these fields per cell:

| Included | Excluded |
| --- | --- |
| `text` | `style_id` |
| `display_width` | `hyperlink_id` |
| `wide_continuation` | renderer cache state |
| `occupied` | dirty flags |

The converted ASCII paths use local predicates and call `advance_row_content_generation_with_change_flag`.

The residual hot paths still do this shape:

1. Copy `screen_row.cells` into `before_cells`.
2. Mutate a local span.
3. Call `advance_row_content_generation_if_changed(screen_row, before_cells)`.
4. Compare nearly the full 873-column row.

Confirmed hot residual callers under `put_text`:

| Path | Why it remains expensive | Required conversion shape |
| --- | --- | --- |
| `put_text -> put_scalar -> put_spacing_scalar -> place_cell_text -> install_cell_span` | Every non-ASCII spacing scalar writes through `install_cell_span`, which still snapshots and compares the full row. | Local scalar-span detector over the union of the old wide span and new install span. |
| `put_text -> put_scalar -> append_zero_width_scalar -> install_cell_span` | Combining updates rebuild a base cell through the same full-row `install_cell_span` gate. | Same scalar-span detector. |
| `put_text -> put_scalar -> append_zero_width_scalar` autowrap overflow source-row clear | Source row clear still snapshots and compares before wrapping combined content to the destination row. | Local clear detector over the old base wide span, or proven `true` when the occupied base cell is cleared. |

The key correctness edge is wide-span repair. A local detector must compute `cell_base_position(position)` before mutation and inspect the old base span as well as the intended new base/continuation layout. Otherwise overwriting a wide continuation can be misclassified.

## What not to do

Do not roll back the ASCII span-local change. The counters and source confirm it did the intended work.

Do not treat style-only or hyperlink-only rewrites as content-generation changes in this slice. That would change the current comparator contract and collapse cache/lease reuse for cases the current code deliberately treats as selection-content-identical.

Do not convert `install_cell_span` to unconditional `changed=true`. Idempotent non-ASCII and wide-glyph rewrites must continue to avoid generation advances.

Do not convert broad mutators in the same implementation slice. `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and `resize_rows` need separate idempotency tests and different local-detection rules.

Do not remove `advance_row_content_generation_if_changed` yet. It remains the safe fallback for producers not audited in the current slice.

Do not use checksum or fingerprint state as the production generation decision. The affected cell range is knowable from terminal mutation semantics; a checksum adds state without removing the need to reason about wide bases and continuations.

Do not use the current hardened/span-local profile pair for broad end-to-end timing claims. The workload drift is too large.

Do not mix render-frame/snapshot contract work with this residual compare fix. It may have larger overall ROI, but it is a separate migration with different validation gates.

## Recommended next implementation slice

Implement the non-ASCII `print_text` residual compare slice.

Scope:

1. Add tests first for the scalar install and combining/autowrap cases listed below.
2. Add private scalar-span local detection helpers near the existing ASCII detectors.
3. Convert `install_cell_span` from full-row snapshot comparison to local detection plus `advance_row_content_generation_with_change_flag`.
4. Convert the `append_zero_width_scalar` autowrap source-row clear from full-row snapshot comparison to local clear detection plus `advance_row_content_generation_with_change_flag`.
5. Add a scalar local-inspection counter and emit it with the existing model profile counters.
6. Leave ASCII paths unchanged.
7. Leave broad control-sequence mutators unchanged.

Recommended helper contract:

| Helper | Contract |
| --- | --- |
| `scalar_span_changes_selection_content` | Compare the old affected region against the intended base-plus-continuation layout for a scalar install. |
| `scalar_span_clear_changes_selection_content` | Compare the old base wide span against erased cells for the source-row clear path. |
| `scalar_span_local_cells_inspected` | Count cells inspected by the new scalar local detector. |

The affected region for scalar install is the union of:

- The old base span containing `position`, found before mutation with `cell_base_position(position)`.
- The new install span `[position.column, position.column + display_width)`.
- Any continuation cells that become erased because the old span and new span differ.

The intended comparison must reuse `cells_have_same_selection_content` or an exact equivalent. Do not duplicate the field list in multiple places.

Expected profile result for an equivalent workload:

| Metric | Expected direction |
| --- | --- |
| `Terminal_screen_model::advance_row_content_generation_if_changed::compare` under `print_text` | Drops to negligible or zero. |
| `row_content_generation_comparisons` | Drops by roughly the former 876k `print_text` residual, except for any remaining non-print callers. |
| `row_content_generation_comparison_cells` | Drops by roughly row-width times removed calls. |
| `scalar_span_local_cells_inspected` | Increases and explains replacement local work. |
| `row_content_generation_advances` | Semantically equivalent for identical input. Any difference requires a specific correctness explanation. |
| `apply_action::print_text` total/mean | Falls materially if no new bottleneck dominates. |

## Tests-first gate

Add tests before production changes. The minimum gate should be model-level and generation-specific.

Required cases:

| Case | Required result |
| --- | --- |
| Idempotent non-ASCII spacing scalar rewrite | Same retained id, same generation. |
| Changed non-ASCII spacing scalar rewrite | Same retained id, generation advances once. |
| Idempotent wide/CJK rewrite | Same retained id, same generation. |
| Changed wide/CJK rewrite | Same retained id, generation advances once. |
| Style-only rewrite on same non-ASCII/wide cell | Same generation under the current contract. |
| Hyperlink-only rewrite on same non-ASCII/wide cell | Same generation under the current contract. |
| Scalar overwrite of a wide base | Generation advances when selection content changes. |
| Scalar overwrite of a wide continuation | Generation advances when old base/continuation content is cleared or replaced. |
| Combining mark append that changes base-cell text | Generation advances once. |
| Idempotent combining/text rewrite where selection content is unchanged | Same generation. |
| Combining/autowrap overflow source-row clear | Source row advances when the clear changes selection content; destination row follows scalar install rules. |
| Snapshot provenance smoke after changed and unchanged cases | Visible-line provenance matches model retained provenance. |

Test oracle:

| Captured value | Assertion |
| --- | --- |
| Retained line id before/after | Must remain stable for in-place mutation cases; identity replacement cases are separate. |
| Selection-content signature before/after | Signature changed iff generation advances. |
| `content_generation` before/after | Advances exactly once for one in-place selection-content mutation. |
| Style/hyperlink-only changes | Do not advance generation unless the contract is intentionally changed. |

Do not use timing as a correctness gate. Timing belongs only after semantic tests pass.

## Later slices

Later residual-compare slices, in order:

1. `erase_row_range`: use a widened erased-range detector. Must preserve no-op erases over already erased default-style cells.
2. `erase_visible_screen`: replace per-row before snapshots with a scan against `erased_cell()` and no allocation. Must preserve no-op full-screen erase semantics.
3. `insert_cells` and `delete_cells`: detect selection-content changes over the cursor-to-row-end suffix, including wide-boundary clears and `repair_wide_spans_in_row` effects. Must preserve no-op shifts on erased suffixes.
4. `resize_rows`: keep new/non-retained rows on retained-id replacement; use direct true for column-count changes if matching current size-mismatch semantics; make `repair_wide_spans_in_row` report whether it changed selection content for equal-width repair cases.
5. Retire or narrow `advance_row_content_generation_if_changed` only after all remaining producers are audited and converted.

Separate broader performance track:

| Area | Why separate |
| --- | --- |
| `render_snapshot::append_rows` full materialization | Larger total cost, but changes snapshot/publication contracts and needs different tests. |
| `viewport_row_cells` per-row copies | Good independent optimization candidate, but not a retained-generation comparison fix. |
| `build_terminal_render_frame` duplicate full-cell passes | Likely high ROI, but renderer-side and outside residual compare semantics. |
| QSG text-resource churn | Downstream of snapshot/frame work; should not block the residual compare slice. |

## Re-profile criteria

A valid post-slice comparison needs a tighter workload fingerprint than the current pair.

Minimum profile acceptance criteria:

- Same geometry: rows, columns, font family, font size, device pixel ratio.
- Same deterministic input or transcript hash.
- Printable character count within +/- 2%.
- `print_text_calls` and printable span calls within +/- 5%.
- Parser action counts by kind within +/- 5%.
- Control-sequence counts within +/- 5%.
- `mark_all_dirty_calls`, full-repaint fallbacks, resize counts, and viewport fallbacks matching exactly or explained by cause.
- Render snapshot requests and renderer frames within +/- 5%, unless the slice intentionally changes publication cadence.

Required reported metrics:

| Metric | Required interpretation |
| --- | --- |
| Raw totals | Still report, but do not rely on them alone. |
| Per printable character | Primary model text-write normalization. |
| Per scalar install or local inspected cell | Primary replacement-work normalization. |
| Per `print_text_call` | Secondary, because characters/call can drift. |
| Per snapshot and per frame | Only for broader pipeline context. |
| `row_content_generation_advances` | Semantic parity signal, not a performance metric. |

Acceptance gates after the recommended slice:

- `printable_ascii_row_copies` and `printable_ascii_row_copy_cells` remain zero.
- New scalar local-inspection counter is nonzero and plausible.
- `advance_row_content_generation_if_changed::compare` is no longer a material child of `apply_action::print_text`.
- `row_content_generation_comparison_cells` drops by a row-width-scale amount for removed print callers.
- Retained-line generation tests pass, especially no-op scalar and wide rewrite cases.
- Snapshot provenance tests pass.

## Risks and open questions

| Risk or question | Disposition |
| --- | --- |
| Does any consumer actually require style/hyperlink in `content_generation`? | Current comparator excludes them. Do not change in this slice. If a consumer requires them, that is a separate contract bug and migration. |
| Local scalar detector misses old wide-base effects. | Main correctness risk. Mitigate by computing base position before mutation and testing wide-continuation overwrite. |
| Local detector over-advances idempotent wide/non-ASCII rewrites. | Mitigate with tests-first idempotency cases. |
| Combining/autowrap touches two rows. | Treat source-row clear and destination-row install as separate generation decisions. |
| Workload drift hides profile result. | Use the re-profile criteria above; do not accept raw total timing alone. |
| Counter attribution remains too coarse. | Add scalar local-inspection counter now. Producer-specific comparison counters can wait unless the next profile still has unexplained residual compares. |
| Broad mutator conversions are tempting because they are easy to find. | Defer. They are correctness-sensitive and measured smaller under this workload. |
| Render/snapshot pipeline has larger ROI. | True, but separate. It should be planned as its own governed performance slice after or parallel to this residual compare fix, not mixed into it. |

## Source reports considered

- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_a_callers.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_b_broad_mutators.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_c_consumers.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_d_tests.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_e_counter_integrity.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_f_pipeline_roi.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_g_design.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_codex_h_skeptical_review.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_01_print_subpaths.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_02_broad_mutators.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_03_profile_normalization.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_04_roi_strategy.md`

Profile sources considered:

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
