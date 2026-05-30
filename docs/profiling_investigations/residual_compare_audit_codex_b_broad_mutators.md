# Residual compare audit B: broad row mutators

## Executive summary

The residual `Terminal_screen_model::advance_row_content_generation_if_changed::compare` cost is no longer dominated by printable ASCII row-copy detection. In the span-local Nelostie profile, `printable_ascii_row_copies=0`, but `row_content_generation_comparisons=887639` and `row_content_generation_comparison_cells=772753829` remain. In the hardened profile, the older print path still records `printable_ascii_row_copies=55572`, and the model records `row_content_generation_comparisons=937538` over `818329995` compared cells.

The remaining broad producers are concentrated in `Terminal_screen_model` row mutators that still snapshot `screen_row.cells` and call `advance_row_content_generation_if_changed`: retained-row resize repair, `install_cell_span`, erase-range based clearing, full-screen erase fallback, insert cells, and delete cells. These paths differ from printable ASCII because several of them can be exact no-ops for selection-visible row content, or can change only style/hyperlink fields that current generation comparison deliberately ignores.

Only a small subset can be converted directly to `advance_row_content_generation_with_change_flag(true/false)` without changing generation semantics. Most need exact local detection over the affected span or suffix. Row-move paths such as insert/delete lines and scrolling already avoid the compare by moving row identities or replacing retained line IDs, and should remain conservative at the row-identity level rather than being rewritten as per-cell local detection.

## Producer inventory

| Producer | Current detection | Notes |
| --- | --- | --- |
| `resize_rows` | Copies `row.cells` for existing retained rows, resizes, repairs wide spans, then full-row compares. | The compare is only for rows with retained provenance. New rows get a new retained line ID instead. |
| `install_cell_span` | Copies the full row, clears the target span, writes base and continuation cells, then full-row compares. | Used by spacing/wide scalar placement and combining-scalar rewrites. Style and hyperlink changes are ignored by the current comparison. |
| `append_zero_width_scalar` overflow branch | Copies the full row around `clear_cell_at(target)` before wrapping and installing the combined span on the next row. | The target is known occupied before the clear, so that clear is selection-content changing. The subsequent install is routed through `install_cell_span`. |
| `erase_row_range` | Copies the full row, erases every requested column through `erase_cell_at`, then full-row compares. | Used by erase-in-line, erase-characters, clear-before/after-cursor, and much of erase-in-display. |
| `erase_visible_screen` non-recovery branch | For each row, copies the full row, fills with `erased_cell()`, then full-row compares. | The primary repaint recovery branch replaces row IDs instead and does not use the compare. |
| `insert_cells` | Copies the full row, clears wide boundaries, shifts the suffix right, fills inserted cells, repairs wide spans, then full-row compares. | Can be a true no-op on an already erased suffix, but can also affect a wide span outside the literal insertion range. |
| `delete_cells` | Copies the full row, erases deleted cells, clears a wide boundary, shifts the suffix left, fills the tail, repairs wide spans, then full-row compares. | Same local-detection shape as insert, but the affected suffix starts at the cursor and includes tail replacement. |
| `repair_wide_spans_in_row` | Mutates invalid continuations/overlaps in place and returns no change flag. | It is a hidden source of content changes inside resize and insert/delete-cell producers. |
| `insert_lines`, `delete_lines`, `scroll_up_region`, `scroll_down_region` | Move whole `Terminal_screen_row` objects and call `replace_row_with_erased_retained_line` for introduced blank rows. | These paths mark dirty rows but do not do full-row content-generation compares. |
| `visual_row_projection_for_current_geometry` | Copies/projections rows and repairs wide spans for rendering/selection reads. | It is not a retained-line content-generation producer and should not be part of this conversion. |

## Safe boolean conversions

| Path | Safe flag | Reason |
| --- | --- | --- |
| `resize_rows` for existing retained rows when the row cell vector length changes | `true` | `rows_have_same_selection_content` returns false on size mismatch before inspecting cells. A column-count change therefore always advances under the current semantics. |
| `resize_rows` for non-retained/new rows | No generation call | The current code replaces the retained line ID and skips content-generation advancement. Preserve that shape. |
| `append_zero_width_scalar` overflow clear before wrapping | `true` | The branch has already proven the base cell is occupied. Clearing that cell or span changes selection-visible content before the wrap. |
| `append_zero_width_scalar` non-overflow combining rewrite at the call-site level | `true`, if routed through a caller-supplied flag | The combined text is `cell.text + text` after proving the base cell is occupied, so the selection-visible text changes. This does not make generic `install_cell_span` always true. |
| `insert_lines`, `delete_lines`, `scroll_up_region`, `scroll_down_region` introduced blank rows | Keep row replacement, not a generation flag | `replace_row_with_erased_retained_line` intentionally creates a new retained line identity. There is no residual compare to remove. |

These are safe only if “safe” means preserving the current content-generation observable behavior. A blanket `true` for all erase, insert/delete-cell, or span-install paths would be conservative for invalidating leases, but it would not preserve the existing no-advance behavior for no-op erases or style/hyperlink-only rewrites.

## Local-detection candidates

| Path | Required local detection |
| --- | --- |
| `install_cell_span` | Compute whether selection-visible content differs over the affected interval before mutation. The interval must include the base span cleared by `clear_cell_at(position)` and the destination width written by the new span. Ignore style and hyperlink changes to match `cells_have_same_selection_content`. |
| `append_zero_width_scalar` generic install calls | Either pass a known `true` flag from the combining caller or use the same local span detector as `install_cell_span`. The generic installer must still support style/hyperlink-only no-advance cases. |
| `erase_row_range` | Expand the requested range to any wide-span base/continuation cells that `erase_cell_at` will clear, then compare only those cells against the `erased_cell()` result. Default-style erase over already erased cells must remain `false`. |
| `erase_visible_screen` non-recovery branch | Detect per row whether filling with `erased_cell()` changes any selection-visible cell. This can be a row scan without allocating a copy; it does not need full-row before storage. |
| `insert_cells` | Detect changes over the cursor-to-row-end suffix, including wide-boundary clears and `repair_wide_spans_in_row`. A no-op insertion into an erased suffix must remain `false`. |
| `delete_cells` | Detect changes over the cursor-to-row-end suffix, including deleted cells, the boundary clear at `cursor + count`, tail replacement, and wide repair. |
| `resize_rows` when the row cell vector length is unchanged | Local detection depends on `repair_wide_spans_in_row`. If the repair helper proves no mutation, the flag is `false`; if it repairs invalid spans, the flag is `true`. |
| `repair_wide_spans_in_row` | Return a boolean or expose a caller-supplied change accumulator. Every assignment to `Cell{}` should set the flag only if the selection-visible fields actually differ. Projection-only callers can ignore the result. |

The useful helper boundary is not another full-row copy. It is a small predicate layer around `cells_have_same_selection_content`, plus mutation helpers that report whether they changed selection-visible fields while they are already touching the affected cells.

## Keep-conservative paths

| Path | Classification |
| --- | --- |
| Primary repaint recovery branch of `erase_visible_screen` | Keep conservative. It intentionally replaces retained line IDs during recovery rather than advancing content generations by local cell comparison. |
| `insert_lines` and `delete_lines` | Keep conservative row-identity behavior. Rows are moved wholesale and blank introduced rows receive new retained IDs; per-cell generation detection would be the wrong abstraction. |
| `scroll_up_region` and `scroll_down_region` | Keep conservative row-identity behavior. Scrollback append and row moves preserve or replace row identities explicitly. |
| `visual_row_projection_for_current_geometry` and `row_cells_for_current_geometry` | Keep out of content-generation conversion. They build read-side projections and do not update retained-line generations. |
| Style/mode-only paths | Must not be collapsed to unconditional `true`. Current generation comparison ignores style and hyperlink, so style-only or hyperlink-only rewrites must not detach selection leases through content generation. |

## Test requirements

The conversion should be test-gated by retained-line content-generation behavior, not only by rendered text equality.

Required test cases:

1. Erase over an already erased default-style row does not advance content generation.
2. Erase over visible text advances content generation.
3. Erase over part of a wide span advances generation for the row and clears the full affected span.
4. Full-row erase with default style over an already erased row does not advance generation.
5. Full-row erase with an erase style that changes `occupied` state advances generation where current comparison would advance.
6. Installing the same text and width with only style or hyperlink changed does not advance generation.
7. Installing different text, different width, or different wide-continuation layout advances generation.
8. Combining scalar append advances generation when it changes the base cell text.
9. Combining overflow/autowrap clear advances the source row and the wrapped install advances the destination row.
10. Insert cells into an erased suffix preserves generation when the row selection content is unchanged.
11. Insert cells shifting visible text advances generation.
12. Insert cells that repair a wide boundary advance generation when the repair changes selection-visible cells.
13. Delete cells from an erased suffix preserves generation when unchanged.
14. Delete cells shifting visible text advances generation.
15. Delete cells that repair a wide boundary advance generation when the repair changes selection-visible cells.
16. Row-count-only resize preserves retained-row content generations when column count and wide-span repair state are unchanged.
17. Column-count resize advances existing retained rows, matching the current size-mismatch comparison behavior.
18. Resize-triggered wide-span repair advances only rows whose selection-visible cells were repaired.
19. Line insert/delete and scroll paths preserve existing retained-line identity behavior.
20. Hardened and span-local Nelostie profiles should show `row_content_generation_comparisons` and `row_content_generation_comparison_cells` falling, while `row_content_generation_advances` stays explainably equivalent for the same workload.

## Phased plan

1. Add local change-reporting primitives without changing callers: a selection-visible cell equality helper is already present, so the first implementation step is a mutation helper or repair-return flag that reports changes in `text`, `display_width`, `wide_continuation`, and `occupied` only.
2. Convert `repair_wide_spans_in_row` to report whether it changed selection-visible content. Keep projection callers ignoring the result.
3. Convert `resize_rows`: use `true` when old and new row cell counts differ, use the repair result when counts match, and keep new/non-retained rows on retained-ID replacement.
4. Convert erase paths: `erase_row_range` and `erase_visible_screen` should compute a local flag while erasing/filling, then call `advance_row_content_generation_with_change_flag`.
5. Convert `install_cell_span` and combining paths: allow callers with proven text changes to pass `true`, but keep generic span install exact for style/hyperlink-only rewrites.
6. Convert `insert_cells` and `delete_cells`: include boundary clears, suffix movement, tail/insertion fill, and repair result in one local change flag.
7. Reprofile hardened and span-local Nelostie profiles. Acceptance should be lower residual compare counts/cells with stable generation-advance semantics and no new wide-boundary repair regressions.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
