# Residual full-row comparison audit: print_text callers after span-local conversion

## Executive summary

The span-local conversion removed the printable-ASCII row-copy path from `print_text`: the after profile reports `printable_ascii_row_copies=0` and `printable_ascii_row_copy_cells=0`.

The remaining hot full-row comparison work under `print_text` is now the non-ASCII scalar path, not the ASCII span path. In `nelostie_profile_span_local.txt`, `Terminal_screen_model::advance_row_content_generation_if_changed::compare` still appears as a child of `Terminal_screen_model::apply_action::print_text` with 876,318 calls and 4.584 s total. That accounts for about 43.6% of the measured `print_text` total in that profile.

The residual print subpaths are:

- `put_text -> put_scalar -> put_spacing_scalar -> place_cell_text -> install_cell_span`.
- `put_text -> put_scalar -> append_zero_width_scalar -> install_cell_span`.
- `put_text -> put_scalar -> append_zero_width_scalar`, in the autowrap overflow branch that clears the old base cell before installing the combined cell on the next line.

The safe next slice is to convert `install_cell_span` to a local selection-content change decision and convert the zero-width overflow clear to a boolean generation decision. Do not change the already-converted printable-ASCII paths, and do not sweep unrelated control-sequence callers in the same slice.

## Measured evidence

Both profile inputs use the same surface geometry: 235 rows, 873 columns, cell width 3.5, cell height 6.98438, and device pixel ratio 1.25.

Baseline hardened profile:

| Metric | Value |
| --- | ---: |
| `Terminal_screen_model::apply_action::print_text` | 75,865 calls, 11.238 s total, 148.134 us mean |
| `Terminal_screen_model::apply_action::print_text::row_copy` | 55,572 calls, 197.107 ms total |
| `Terminal_screen_model::advance_row_content_generation_if_changed::compare` under `print_text` | 935,526 calls, 4.718 s total |
| `print_text_calls` | 75,865 |
| `printable_ascii_span_calls` | 55,572 |
| `printable_ascii_span_characters` | 3,078,371 |
| `printable_ascii_row_copies` | 55,572 |
| `printable_ascii_row_copy_cells` | 48,493,629 |
| `row_content_generation_comparisons` | 937,538 |
| `row_content_generation_comparison_cells` | 818,329,995 |
| `row_content_generation_advances` | 274,064 |

After span-local profile:

| Metric | Value |
| --- | ---: |
| `Terminal_screen_model::apply_action::print_text` | 81,631 calls, 10.505 s total, 128.686 us mean |
| `Terminal_screen_model::apply_action::print_text::row_copy` | absent from searched aggregate lines |
| `Terminal_screen_model::advance_row_content_generation_if_changed::compare` under `print_text` | 876,318 calls, 4.584 s total |
| `print_text_calls` | 81,631 |
| `printable_ascii_span_calls` | 60,025 |
| `printable_ascii_span_characters` | 7,212,515 |
| `printable_ascii_row_copies` | 0 |
| `printable_ascii_row_copy_cells` | 0 |
| `printable_ascii_local_cells_inspected` | 5,183,956 |
| `row_content_generation_comparisons` | 887,639 |
| `row_content_generation_comparison_cells` | 772,753,829 |
| `row_content_generation_advances` | 311,440 |

The after-profile comparison-cell average is 870.6 cells per comparison, effectively the full 873-column row. The remaining comparison work is therefore still row-width work.

The after-profile `print_text` child compare total is 4.584 s out of 10.505 s, or about 43.6% of `print_text`. The same compare child is 4.584 s out of 11.380 s in `apply_parser_actions`, or about 40.3% of parser-action application time.

The profile runs are not perfectly count-identical: `print_text_calls`, ASCII span calls, and ASCII characters differ between the two inputs. The direct conclusion does not depend on a pure before/after delta. The post-change profile alone proves that full-row comparisons still dominate a large part of `print_text` after printable ASCII row copies were removed.

## Residual caller map

| Caller path | Snapshot site | Compare call | Under `print_text`? | Hot reason | Conversion status |
| --- | --- | --- | --- | --- | --- |
| `put_text -> put_scalar -> put_spacing_scalar -> place_cell_text -> install_cell_span` | `src/terminal_screen_model.cpp:4872` copies `screen_row.cells` | `src/terminal_screen_model.cpp:4894` | Yes | Every non-ASCII spacing scalar goes through `install_cell_span`; each call compares a full row even though it mutates a bounded cell span. | Safe next target with a local span-change detector. |
| `put_text -> put_scalar -> append_zero_width_scalar -> install_cell_span` | `src/terminal_screen_model.cpp:4872` copies `screen_row.cells` | `src/terminal_screen_model.cpp:4894` | Yes | Combining text updates rebuild one base cell and possible continuations through the same full-row `install_cell_span` gate. | Safe next target through the same local span-change detector. |
| `put_text -> put_scalar -> append_zero_width_scalar`, autowrap overflow branch | `src/terminal_screen_model.cpp:4835` copies `screen_row.cells` before `clear_cell_at(target)` | `src/terminal_screen_model.cpp:4837` | Yes | Rare compared with ordinary spacing scalars, but still inside `print_text`; it clears the old occupied base cell before wrapping the combined text. | Safe boolean target; the guarded old base cell is occupied, so clearing it changes selection content. |

Already converted paths:

| Caller path | Current generation decision | Evidence |
| --- | --- | --- |
| `put_text -> put_printable_ascii_text`, no-autowrap clipped-row path | Local boolean via `printable_ascii_span_changes_selection_content` and `printable_ascii_cell_changes_selection_content`; calls `advance_row_content_generation_with_change_flag`. | `src/terminal_screen_model.cpp:4648-4671`. |
| `put_text -> put_printable_ascii_text -> write_printable_ascii_span` | Local boolean via `printable_ascii_span_changes_selection_content`; calls `advance_row_content_generation_with_change_flag`. | `src/terminal_screen_model.cpp:4718-4721`. |
| `put_text -> write_printable_ascii_cell` | Local boolean via `printable_ascii_cell_changes_selection_content`; calls `advance_row_content_generation_with_change_flag`. | `src/terminal_screen_model.cpp:4747-4753`. |

Residual snapshot callers outside `print_text` were also observed, but are not Focus A targets: `resize_rows`, `erase_row_range`, `erase_visible_screen`, `insert_cells`, and `delete_cells` still call `advance_row_content_generation_if_changed` with full-row snapshots.

## Safe conversions

`install_cell_span` is the high-value safe conversion.

The existing generation comparison only considers selection content: `text`, `display_width`, `wide_continuation`, and `occupied`. It intentionally ignores style and hyperlink fields. A local detector can preserve that contract by comparing only the cells that `install_cell_span` can affect:

- The base cell at `position` after `clear_cell_at(position)` and replacement text installation.
- Any cells cleared because `position` lies inside an existing wide span.
- Continuation cells in `[position.column + 1, position.column + display_width)`.
- Any existing wide-span continuations overwritten by `clear_cell_at` or by the new continuation writes.

The helper shape should be a local boolean decision, not a full-row before snapshot. For example, a helper equivalent to `cell_span_changes_selection_content(position, text, display_width)` can compute the intended local cells and return whether the currently visible local selection content differs. `install_cell_span` can then call `advance_row_content_generation_with_change_flag(screen_row, changed)`.

The zero-width overflow clear in `append_zero_width_scalar` can safely use a boolean decision. The branch has already established that the target base cell is occupied before it reaches the overflow path. Clearing that base cell changes selection content, so the old row can advance with `advance_row_content_generation_with_change_flag(screen_row, true)` after `clear_cell_at(target)`. If the implementation wants a narrower invariant, it can compute the boolean before clearing from the same local base/wide-span region instead of using unconditional `true`.

The ASCII paths should remain on their current local decisions. The after profile confirms they no longer produce printable ASCII row copies, and the code already routes them through `advance_row_content_generation_with_change_flag`.

## Unsafe/deferred conversions

Do not blanket-convert `install_cell_span` to `changed=true`. Idempotent non-ASCII repaint can write the same selection content to the same cells. The current full-row comparison avoids generation bumps in that case, and the local detector must preserve that behavior.

Do not include style or hyperlink changes in the new generation decision. The current `rows_have_same_selection_content` path ignores those fields, so including them would change retained-line content-generation semantics and could over-invalidate text caches.

Do not fold non-`print_text` callers into this slice. `erase_row_range`, `erase_visible_screen`, `insert_cells`, and `delete_cells` may be convertible later, but they are control-sequence paths with separate semantics and validation gates. `resize_rows` is also a structural grid-size path and should remain out of the print-text residual slice.

Do not remove `advance_row_content_generation_if_changed` entirely in this slice. It remains the full-row compatibility gate for broad mutators until each caller is audited and moved deliberately.

## Proposed next slice

Implement a narrow Focus A follow-up:

1. Add a local span selection-content detector for `install_cell_span` that covers the base cell, cleared wide-span region, and new continuation range.
2. Change `install_cell_span` from full-row snapshot plus `advance_row_content_generation_if_changed` to the local boolean plus `advance_row_content_generation_with_change_flag`.
3. Change the `append_zero_width_scalar` overflow clear from full-row snapshot comparison to a boolean generation decision for the old row.
4. Leave `erase_row_range`, `erase_visible_screen`, `insert_cells`, `delete_cells`, and `resize_rows` unchanged.
5. Keep existing ASCII span-local code unchanged.

Expected profile effect for the same workload shape:

- `Terminal_screen_model::advance_row_content_generation_if_changed::compare` should no longer be a material child of `Terminal_screen_model::apply_action::print_text`.
- `row_content_generation_comparisons` should drop by roughly the 876,318 calls currently attributed under `print_text`.
- `row_content_generation_comparison_cells` should drop by roughly row-width times those calls, about 764 million cells for the observed 873-column surface.
- `row_content_generation_advances` should remain semantically equivalent for the same input stream.

## Validation gates

Functional gates:

- Existing retained-line content-generation mutation tests still pass.
- Add or run coverage for idempotent non-ASCII repaint: writing the same non-ASCII scalar to the same cell must not advance generation.
- Add or run coverage for replacing a non-ASCII scalar with different text at the same cell: generation must advance.
- Add or run coverage for wide-cell replacement and continuation repair: generation must advance when base or continuation selection content changes.
- Add or run coverage for zero-width combining scalar updates, including the overflow/autowrap branch.

Profile gates:

- Re-run the same Nelostie profile collection used for `nelostie_profile_span_local.txt`.
- Confirm `printable_ascii_row_copies=0` and `printable_ascii_row_copy_cells=0` remain zero.
- Confirm `Terminal_screen_model::advance_row_content_generation_if_changed::compare` is absent or negligible under `Terminal_screen_model::apply_action::print_text`.
- Confirm `row_content_generation_comparisons` and `row_content_generation_comparison_cells` fall by approximately the former `print_text` residual compare volume.
- Confirm `Terminal_screen_model::apply_action::print_text` total and mean fall materially.
- Confirm `row_content_generation_advances` matches the previous run for equivalent input, or explain any deliberate semantic difference.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
