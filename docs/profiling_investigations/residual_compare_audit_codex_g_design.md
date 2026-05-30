# Residual Compare Audit: Focus G Implementation Design

## Executive summary

The remaining generation optimization should target the non-ASCII `print_text` residual, not broader control-sequence row operations. The post-span-local profile still shows `row_content_generation_comparisons=887639`, with `876318` compare calls under `Terminal_screen_model::apply_action::print_text` and `4.584178300 s` in the compare child scope. Printable ASCII row copies are already eliminated in `nelostie_profile_span_local.txt` (`printable_ascii_row_copies=0`), so the next material source is `install_cell_span` plus the `append_zero_width_scalar` autowrap source-row clear.

The implementation design should introduce one shared span-local selection-content detector for scalar cell installs, then migrate the non-ASCII print subpaths to call `advance_row_content_generation_with_change_flag` instead of `advance_row_content_generation_if_changed`. The API should not add a checksum or fingerprint contract. If a later debug-only oracle is needed, keep it behind profiling/test instrumentation and outside the production decision path.

The migration should be small and governed: tests first, shared helper second, `install_cell_span` conversion third, `append_zero_width_scalar` autowrap conversion fourth, counters/profile emission fifth, then profiling comparison. Do not migrate `insert_cells`, `delete_cells`, `erase_row_range`, `erase_visible_screen`, or `resize_rows` in this batch; their measured cost is outside the residual `print_text` focus and they have different semantics.

## Proposed API changes

Add a single internal helper family beside the existing printable-ASCII detectors in `Terminal_screen_model`:

```cpp
bool scalar_span_changes_selection_content(
    const Terminal_screen_row&     row,
    terminal_grid_position_t       position,
    QStringView                    text,
    int                            display_width) const;

bool scalar_span_clear_changes_selection_content(
    const Terminal_screen_row&     row,
    terminal_grid_position_t       position) const;
```

Recommended naming:

| Name | Purpose |
| --- | --- |
| `scalar_span_changes_selection_content` | Compares the pre-mutation cells touched by a non-ASCII scalar install with the intended base-plus-continuation layout. |
| `scalar_span_clear_changes_selection_content` | Handles the source-row clear in the `append_zero_width_scalar` autowrap branch before the wrapped install occurs. |
| `scalar_span_local_cells_inspected` | New profiling counter for cells inspected by the scalar span-local detector. |

Helper contract:

- Use `cells_have_same_selection_content` as the only field-level comparison authority.
- Compute `cell_base_position(position)` before mutation when the input position may point into a wide continuation.
- Inspect the union of the existing base span and intended install span, clamped to the row width.
- Build intended cells for comparison locally: one occupied base cell with `text`, `display_width`, and `wide_continuation=false`; continuation cells with empty text, `display_width=0`, `wide_continuation=true`, and `occupied=true`; erased cells outside the intended span where the old wide base or continuation is cleared.
- Exclude `style_id` and `hyperlink_id` from the generation decision, matching the current `cells_have_same_selection_content` contract.
- Return `true` if any inspected cell differs in selection-visible content; return `false` only when the existing full-row comparison would have found no selection-content change.

Call-site shape:

```cpp
const bool selection_content_changed =
    scalar_span_changes_selection_content(
        screen_row,
        position,
        text,
        display_width);

clear_cell_at(position);
install intended base and continuations;
advance_row_content_generation_with_change_flag(
    screen_row,
    selection_content_changed);
```

The helper should be private to `Terminal_screen_model`. Do not expose a public compatibility API, do not add a `_v2` name, and do not keep a parallel production row-snapshot path after the migration lands. A temporary test-only parity oracle is acceptable if it is compiled only for tests or profiling validation and is removed when the batch closes.

Checksum/fingerprint position:

| Decision | Rationale |
| --- | --- |
| Do not use checksums or fingerprints in the production generation decision. | The changed region is already knowable from terminal mutation semantics, and a checksum would introduce another state contract without removing the need to reason about wide bases and continuations. |
| Consider a debug-only fingerprint later only for profile triage. | If future reports show unexplained generation mismatches, a debug aid could summarize row state for diagnostics, but it should not determine whether content generation advances. |

## Migration sequence

1. Add focused generation tests before behavior changes.

   Required cases:

   | Case | Expected generation behavior |
   | --- | --- |
   | Idempotent wide scalar rewrite | No generation advance. |
   | Idempotent non-ASCII spacing scalar rewrite | No generation advance. |
   | Non-ASCII scalar over a wide continuation | Advances exactly when selection-visible content changes. |
   | Style-only rewrite on a wide/non-ASCII cell | No generation advance. |
   | Hyperlink-only rewrite on a wide/non-ASCII cell | No generation advance. |
   | Combining mark autowrap source-row clear | Source row advances only if the clear changes selection-visible content; destination row follows the install helper. |

2. Add the shared helper declarations and implementation.

   The helper should be introduced next to `printable_ascii_cell_changes_selection_content` and `printable_ascii_span_changes_selection_content`. Keep the old `advance_row_content_generation_if_changed` available only for unmigrated producers.

3. Convert `install_cell_span`.

   Remove the `before_cells = screen_row.cells` full-row copy from `install_cell_span`. Compute `selection_content_changed` before `clear_cell_at(position)`, perform the existing mutation, then call `advance_row_content_generation_with_change_flag`. Keep `mark_terminal_content_changed` and `mark_dirty(position.row)` unchanged.

4. Convert the `append_zero_width_scalar` autowrap source-row clear.

   Replace its `before_cells` copy and `advance_row_content_generation_if_changed` call with `scalar_span_clear_changes_selection_content` plus `advance_row_content_generation_with_change_flag`. Leave the subsequent destination-row `install_cell_span` call in place so it benefits from the previous step.

5. Add and emit counters.

   Add `scalar_span_local_cells_inspected` to `Terminal_screen_model_profile_stats`, emit it from the profile text writers used by `vnm_terminal`, and include it in any benchmark JSON/profile output that already mirrors model counters.

6. Re-profile with the provided Nelostie workload.

   Expected direction:

   | Metric | Expected result |
   | --- | --- |
   | `row_content_generation_comparisons` | Drops by approximately the print_text residual share, around `876318` calls in the span-local profile. |
   | `row_content_generation_comparison_cells` | Drops by approximately the row-width residual share, around `772753829` cells minus non-print_text residuals. |
   | `row_content_generation_advances` | Stays semantically equivalent, not necessarily byte-identical if tests reveal a current over-advance bug. Any difference must be explained. |
   | `scalar_span_local_cells_inspected` | Increases and explains the replacement local work. |
   | `apply_action::print_text` child compare time | Drops by about the measured `4.584 s` residual if no new bottleneck dominates. |

7. Defer control-sequence conversions.

   Leave `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and `resize_rows` on `advance_row_content_generation_if_changed` until after this print residual is profiled. In the span-local profile, control-sequence compare work is measured in milliseconds, not seconds.

## Counter updates

Add one required counter now:

| Counter | Type | Owner | Meaning |
| --- | --- | --- | --- |
| `scalar_span_local_cells_inspected` | `std::uint64_t` | `Terminal_screen_model_profile_stats` | Number of cells inspected by the non-ASCII scalar span-local generation detector. |

Keep existing counters:

| Existing counter | Use after migration |
| --- | --- |
| `row_content_generation_comparisons` | Should represent remaining snapshot-based producers only. |
| `row_content_generation_comparison_cells` | Should shrink with remaining snapshot-based producer scope. |
| `row_content_generation_advances` | Primary semantic parity counter. |
| `printable_ascii_local_cells_inspected` | Keeps attribution for the already migrated ASCII path. |

Optional counters only if implementation evidence needs them:

| Optional counter | When justified |
| --- | --- |
| `scalar_span_clear_local_cells_inspected` | If source-row clear and destination-row install need separate attribution after profiling. |
| `scalar_span_install_calls` | If call count is not easily inferred from existing `print_text` / span counters. |

Do not add fingerprint/checksum counters in the main profile schema for this migration. They would not explain the cost center directly and could make the profile contract drift toward debug internals.

## Rollback strategy

Rollback should be commit-level and measurement-gated, not a runtime dual path.

Recommended rollback plan:

1. Land the helper/counter migration in one implementation commit after the tests are present.
2. If correctness tests fail or generation parity is unexplained, revert that implementation commit rather than adding a fallback to full-row comparison.
3. Keep the tests if they describe the stable contract and fail against the reverted production path only because of the candidate implementation. If a test encodes a wrong assumption, fix or remove the test before retrying.
4. Keep the old `advance_row_content_generation_if_changed` only for producers not migrated in this batch. Do not reintroduce it into `install_cell_span` or `append_zero_width_scalar` as a silent fallback.
5. Re-run the span-local profile after rollback or retry to confirm `row_content_generation_comparisons` and `apply_action::print_text` child compare time return to the expected baseline or improve under the corrected implementation.

Rollback acceptance gates:

| Gate | Required evidence |
| --- | --- |
| Correctness rollback | Specific failing test or reproducible selection/content-generation mismatch. |
| Performance rollback | Compare counts/time fail to drop materially, or local detector work exceeds the removed full-row compare cost. |
| Scope rollback | Implementation starts migrating unrelated control-sequence producers in the same batch. |

## Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Missing old wide-base cells when overwriting a continuation | Under-advances generation and can stale retained selection content. | Always compute `cell_base_position(position)` before mutation and include the old base span in the inspected region. |
| Treating style or hyperlink changes as generation changes | Over-advances retained content generation and weakens row identity reuse. | Reuse `cells_have_same_selection_content`, which intentionally excludes style and hyperlink. |
| Divergent detector logic between ASCII and non-ASCII paths | Future fixes land in one detector but not the other. | Keep both detectors adjacent and factor only the shared cell comparison authority, not unrelated terminal logic. |
| Combining autowrap source and destination rows are conflated | Incorrect generation sequencing across two rows. | Treat source-row clear and destination-row install as separate generation decisions. |
| Counter names overfit the current implementation | Profile schema churn in follow-up work. | Use behavior names (`scalar_span_local_cells_inspected`) rather than line-specific names (`install_cell_span_cells_inspected`). |
| Migration expands into control-sequence cleanup | Harder review and blurred performance evidence. | Defer non-print producers until post-migration profiles show them dominant. |
| Debug fingerprint becomes production contract | Extra state and false confidence without removing semantic comparison logic. | Avoid checksums/fingerprints now; reserve only as later debug aid if profile triage requires it. |

## Implementation checklist

- Add tests for idempotent wide/non-ASCII rewrites, wide-continuation overwrite, combining autowrap, style-only, and hyperlink-only cases.
- Add `scalar_span_changes_selection_content` and `scalar_span_clear_changes_selection_content` private helpers.
- Use `cells_have_same_selection_content` inside the helpers.
- Compute `cell_base_position` before any mutation that may clear a wide span.
- Convert `install_cell_span` from full-row snapshot compare to the local helper plus `advance_row_content_generation_with_change_flag`.
- Convert the `append_zero_width_scalar` autowrap source-row clear the same way.
- Add `scalar_span_local_cells_inspected` to `Terminal_screen_model_profile_stats`.
- Emit the new counter in `vnm_terminal` profile text and benchmark profile output where model counters are already surfaced.
- Re-profile `nelostie_profile_span_local` and compare against the provided profile values.
- Confirm `row_content_generation_comparisons` now mainly represents non-print or control-sequence producers.
- Remove any temporary parity/debug code before closing the implementation batch unless it is deliberately test-only.

## Files inspected

| File | Reason |
| --- | --- |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md` | Coding and API migration constraints. |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md` | Formatting guidance. |
| `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md` | Audit/report scope and future-batch risk framing. |
| `C:\plms\varinomics\varinomics-standards\varinomics_change_governance.md` | Multi-batch sequencing, rollback, and no-dual-path guidance. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h` | Existing profile counters and private generation helper declarations. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp` | Existing ASCII span-local helpers, row compare helper, `install_cell_span`, `append_zero_width_scalar`, and remaining full-row producers. |
| `C:\plms\varinomics\vnm_terminal\src\main.cpp` | Existing profile text emission for model counters. |
| `C:\plms\varinomics\vnm_terminal_surface\benchmarks\embedded_terminal\embedded_terminal_benchmark.cpp` | Existing benchmark/profile counter output surface. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt` | Pre-span-local comparison metrics. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt` | Post-span-local residual comparison metrics. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\residual_compare_audit_claude_01_print_subpaths.md` | Prior residual print-subpath audit and test recommendations. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_profile_final_consolidated_report.md` | Broader Nelostie optimization context and phase ordering. |
