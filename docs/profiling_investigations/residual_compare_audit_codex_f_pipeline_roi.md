# Residual compare audit: pipeline ROI after span-local

## Executive summary

The span-local printable ASCII change removed the original row-copy target: in `nelostie_profile_span_local.txt`, `printable_ascii_row_copies=0` and `printable_ascii_row_copy_cells=0`, while the hardened profile still had `printable_ascii_row_copies=55572` and `printable_ascii_row_copy_cells=48493629`.

The next best ROI is no longer more printable-ASCII model-side comparison removal. The largest measured residual costs now sit downstream:

| Area | Span-local measured cost | Calls | Mean | Source shape |
| --- | ---: | ---: | ---: | --- |
| Render-frame construction | `build_terminal_render_frame` 30.972 s | 843 | 36.740 ms | Two full snapshot-cell passes: `cells` 16.134 s and `packed_data` 14.736 s. |
| QSG update | `Qsg_terminal_renderer::update_node` 12.818 s | 843 | 15.205 ms | Text-resource sync remains 7.986 s with 15,363 text rebuilds. |
| Snapshot materialization | `render_snapshot::append_rows` 11.670 s | 1,900 | 6.142 ms | Full visible rows are materialized before dirty-row coalescing can reduce render work. |
| Model print text | `apply_action::print_text` 10.505 s | 81,631 | 128.686 us | Residual compare child is 4.584 s; row copies are gone. |

Measured conclusion: target snapshot/frame/QSG before broadening model-side comparison removal. The strongest first slice is render-frame construction, specifically removing duplicated full-cell work in `build_terminal_render_frame::packed_data` or retaining row-frame products for clean rows. The second slice should reduce full snapshot materialization for superseded publications or defer full materialization until render consumption. Residual model comparisons remain worth a later focused slice, but the measured cost is now smaller than frame construction, QSG update, and snapshot append.

Caveat: hardened and span-local profiles are not identical work units. The span-local profile processed more printable ASCII characters (`7212515` vs `3078371`) and more render publications (`1919` vs `1586`). The ranking above therefore uses post-change absolute residual cost and source scaling shape, not only hardened-to-span-local raw deltas.

## Model-side residual cost

The span-local change is present in source and reflected in counters. `Terminal_screen_model::write_printable_ascii_span` now computes `selection_content_changed` with `printable_ascii_span_changes_selection_content`, writes span content, then calls `advance_row_content_generation_with_change_flag` instead of copying the full row and re-comparing it (`src/terminal_screen_model.cpp:4274`, `src/terminal_screen_model.cpp:4314`, `src/terminal_screen_model.cpp:4700`). The no-autowrap clipped path follows the same boolean-change pattern (`src/terminal_screen_model.cpp:4609`).

Measured model evidence after span-local:

| Metric | Hardened | Span-local | Interpretation |
| --- | ---: | ---: | --- |
| `printable_ascii_row_copies` | 55,572 | 0 | Original row-copy target eliminated. |
| `printable_ascii_row_copy_cells` | 48,493,629 | 0 | Original copied-cell target eliminated. |
| `row_content_generation_comparisons` | 937,538 | 887,639 | Broad snapshot-based comparisons remain. |
| `row_content_generation_comparison_cells` | 818,329,995 | 772,753,829 | Residual comparisons still scale by row width. |
| `apply_action::print_text` | 11.238 s | 10.505 s | Still material, but no longer the largest residual pipeline cost. |
| `advance_row_content_generation_if_changed::compare` under `print_text` | 4.718 s | 4.584 s | Broad mutation comparisons still occur inside the print-text flow. |

The remaining compare child under `print_text` is not from the span-local ASCII writer. Source shows snapshot-based comparison remains in broader mutators and non-ASCII/wide paths: `install_cell_span`, `erase_row_range`, `erase_visible_screen`, insert/delete cell paths, and other row-wide operations still call `advance_row_content_generation_if_changed(row, before_cells)` (`src/terminal_screen_model.cpp:4837`, `src/terminal_screen_model.cpp:4894`, `src/terminal_screen_model.cpp:4996`, `src/terminal_screen_model.cpp:5163`, `src/terminal_screen_model.cpp:5203`).

That leaves a valid model-side follow-up, but it is narrower than the original span-local problem. The likely high-value model-only slice is not another printable ASCII rewrite; it is attribution and selective conversion of broad mutators where the change decision is already known or can be computed in one existing sweep. Good candidates are `erase_visible_screen` and insert/delete cells. Poor candidates for immediate conversion are combining/wide scalar paths, where conservative full-row comparison is safer until separately designed.

Model-side dependency on render cost is one-way: removing more compare cost can make ingestion faster, but it does not reduce `render_snapshot::append_rows`, `build_terminal_render_frame`, or QSG text work per publication/frame. If anything, faster ingest can expose render-side costs more clearly by producing snapshots faster. This is why residual model comparison removal is not the best next ROI after span-local.

## Snapshot/frame/QSG residual cost

Snapshot publication remains full visible-grid materialization with dirty-row metadata attached after the fact. `Terminal_screen_model::render_snapshot` reserves `rows * columns`, computes compact dirty ranges, then the `append_rows` scope visits every grid row and appends visible cells (`src/terminal_screen_model.cpp:2788`, `src/terminal_screen_model.cpp:2825`, `src/terminal_screen_model.cpp:2856`, `src/terminal_screen_model.cpp:2876`). In the span-local profile, `render_snapshot::append_rows` costs 11.670 s over 1,900 calls, visits/materializes 439,410 rows, scans 379,139,110 cells, and emits 172,488,235 cells. Dirty rows requested/visible are only 35,279 rows, so dirty metadata is much smaller than the full snapshot payload.

The surface coalesces render snapshots only after a full snapshot object exists. `VNM_TerminalSurface::updatePaintNode` consumes `m_private->render_snapshot`, builds a render frame, and only then calls the renderer (`src/vnm_terminal_surface.cpp:5254`, `src/vnm_terminal_surface.cpp:5303`, `src/vnm_terminal_surface.cpp:5311`). `VNM_TerminalSurface_render_bridge::set_render_snapshot` can coalesce dirty rows when a render update is already pending, but that occurs after model-side snapshot construction (`src/vnm_terminal_surface.cpp:5357`).

Render-frame construction is the largest measured residual. In the span-local profile, `build_terminal_render_frame` costs 30.972 s. Its two main children are nearly the whole cost: `build_terminal_render_frame::cells` costs 16.134 s and `build_terminal_render_frame::packed_data` costs 14.736 s. Source confirms the duplication: the main cell pass loops all `snapshot->cells`, classifies content, emits text/graphics/decorations, and checks dirty-row state (`src/qsg_terminal_renderer.cpp:6040`, `src/qsg_terminal_renderer.cpp:6071`, `src/qsg_terminal_renderer.cpp:6128`). The packed-data pass then builds an explicit row table and loops rows/cells again, reclassifying cells for packed text and graphic spans (`src/qsg_terminal_renderer.cpp:5957`, `src/qsg_terminal_renderer.cpp:5967`). The cumulative renderer counters show `frame_cell_pass_input_cells=72448062` and `frame_packed_pass_input_cells=72448062`, which means the packed pass duplicates the full cell volume.

QSG text synchronization is also material, but it is downstream of the full frame. In the span-local profile, `Qsg_terminal_renderer::update_node` costs 12.818 s and `sync_text_resource_nodes` costs 7.986 s. The renderer considers 47,266,483 text runs, coalesces 9,090,234 resource input runs down to 286,043 output runs, rebuilds 15,363 text resources, reuses 102,835, and creates 74,745 text leaf nodes. Source shows clean-row skip, descriptor reuse, key reuse, and text coalescing already exist (`src/qsg_terminal_renderer.cpp:3474`, `src/qsg_terminal_renderer.cpp:3495`, `src/qsg_terminal_renderer.cpp:3525`, `src/qsg_terminal_renderer.cpp:3550`, `src/qsg_terminal_renderer.cpp:3567`, `src/qsg_terminal_renderer.cpp:3602`). The current issue is not absence of any cache; it is that dirty/full-frame pressure still forces many rows through descriptor/coalescing/layout/replacement.

## ROI ranking

1. Render-frame full-pass reduction. Target `build_terminal_render_frame::packed_data` duplication or retained row-frame rebuilds. Measured span-local target is 30.972 s total, with 14.736 s in the duplicated packed pass alone. This is the largest residual and has a direct source-local explanation.

2. Snapshot materialization and superseded-publication reduction. Target `render_snapshot::append_rows` and publication coalescing before full snapshot construction. Measured span-local target is 11.670 s in append rows and 12.617 s in `publish_render_snapshot`. This also reduces downstream frame/QSG pressure if fewer full snapshots reach the render side.

3. QSG text-resource churn. Target dirty-row text rebuilds, descriptor-identical dirty row reuse, stable row slots, or consuming compact row spans instead of per-cell runs. Measured span-local target is 7.986 s in `sync_text_resource_nodes` and 3.055 s in prepare/add layout. This should follow or pair with row-frame work because frame construction currently creates the per-cell/per-run volume QSG has to process.

4. Residual model-side broad comparisons. Target remaining `advance_row_content_generation_if_changed(row, before_cells)` producers with explicit attribution first. Measured span-local target is 4.584 s under `print_text` compare child and 772,753,829 compared cells by counter. This is real, but smaller than frame, QSG update, and snapshot append, and it does not reduce per-frame render costs.

5. Micro-optimizations inside dirty-row range lookup or model counters. These can support larger work but are not first-order ROI based on current timings.

## Dependency ordering

Render-frame work can start before a full incremental snapshot contract exists. A retained row-frame or a merged packed-data pass can consume the current full snapshot contract while using existing dirty-row ranges, cursor/IME affected rows, and retained line identity to avoid duplicate full-cell work.

QSG text work should not lead unless it is a very small targeted slice. The QSG layer already has clean-row skip, descriptor reuse, key reuse, and ASCII coalescing. Better frame inputs and row-level recomputation boundaries will make QSG text changes easier to validate and more effective.

Snapshot publication redesign is a larger contract change than frame pass merging. If the slice is small, first defer or suppress superseded full snapshot materialization before introducing a retained/incremental render snapshot contract. If the project opens a governed multi-batch migration, define the render-side retained row contract first, then let model/session publish row payload deltas into it.

Residual model comparison work should follow attribution. The span-local profile proves broad comparisons remain, but source shows several different producers. A safe next model-side pass should add or use producer-specific counters before converting broad mutators. Do not couple retained-line `content_generation` to primary repaint recovery; it remains a correctness identity consumed by retained lookup, selection leases, public projection, snapshots, history records, and QSG row cache identity.

## Recommended next two slices

1. Render-frame duplicate-pass slice. Merge `build_terminal_render_frame::packed_data` into the main `build_terminal_render_frame::cells` pass, or introduce retained row-frame products that rebuild only dirty/cursor/IME/selection affected rows. Validate against `build_terminal_render_frame`, `build_terminal_render_frame::cells`, `build_terminal_render_frame::packed_data`, `frame_cell_pass_input_cells`, and `frame_packed_pass_input_cells`. Expected best signal: `packed_data` drops sharply without an equal increase in `cells`.

2. Snapshot supersession/materialization slice. Move coalescing earlier so superseded backend publications do not each pay `render_snapshot::append_rows`, or defer full visible-row materialization until a render update is actually going to consume it. Validate against `render_snapshot_requests`, `render_snapshots_constructed`, `render_snapshot_rows_materialized`, `render_snapshot_cells_emitted`, `snapshots_superseded_before_render`, `frames_published`, and `paint_completed_frames`. Expected best signal: fewer full append-row calls or fewer materialized rows/cells per rendered frame.

If the next batch is constrained to model-only work, choose a smaller third-place slice instead: add producer-specific residual comparison counters, then convert `erase_visible_screen` and insert/delete cell paths to `advance_row_content_generation_with_change_flag` only where the selection-content change decision is already exact.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\content_generation_copy_compare_audit_codex.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\content_generation_copy_compare_audit_claude.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_profile_final_consolidated_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_renderer_qsg_report.md`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_renderer.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
