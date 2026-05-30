# Content Generation Copy/Compare Audit - Codex

## Executive summary

Measured evidence from the Nelostie profiling reports shows `Terminal_screen_model::apply_action::print_text` as the dominant captured ingest/apply cost: 35.398 s over 472,802 calls, 98.1% of `apply_parser_actions`, on a 233 x 871 grid (`docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:20`, `docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:42`, `docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:52`). Source evidence confirms the printable ASCII path copies a whole row into `before_cells`, writes the span, then calls `advance_row_content_generation_if_changed`, which performs a whole-row selection-content comparison (`src/terminal_screen_model.cpp:4652`, `src/terminal_screen_model.cpp:4658`, `src/terminal_screen_model.cpp:4675`, `src/terminal_screen_model.cpp:4676`, `src/terminal_screen_model.cpp:4252`, `src/terminal_screen_model.cpp:4266`).

Source-based inference: this gives printable ASCII writes a `span_count * row_width` copy/compare shape even when the actual mutation is a short local span. On the measured 871-column profile, the previous investigation estimated at least 411.8 million `Cell` slots copied and at least 411.8 million compared before counting real per-character writes (`docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:79`). That estimate is a lower bound and should be treated as inference from measured call counts plus source structure, not a direct child-scope measurement.

The comparison should not be disabled merely because primary repaint recovery is disabled. `content_generation` is a retained-line identity component with correctness consumers in retained history lookup, selection leases, public projection anchoring/reconciliation, render snapshot provenance, and QSG row cache identity. The primary repaint recovery heuristic has its own enablement gates and marks row identity ambiguity through `mark_terminal_content_changed`, not through the full-row `advance_row_content_generation_if_changed` comparison (`src/terminal_screen_model.cpp:6127`, `src/terminal_screen_model.cpp:6133`, `src/terminal_screen_model.cpp:5844`).

Recommended direction: keep `content_generation` semantics, but replace the printable ASCII full-row copy/compare with span-local change detection. A checksum/fingerprint can be useful as a row-level invariant or fallback for broad mutators, but it is not the best first fix for printable ASCII because incremental fingerprints still require correct old/new cell accounting across wide spans, combining marks, erases, resize repairs, and style/hyperlink-excluded semantics.

## Current mechanism

`Terminal_retained_line_provenance` carries `retained_line_id`, `content_generation`, and source (`include/vnm_terminal/internal/terminal_screen_model.h:77`). New retained identities are assigned by `replace_retained_line_id`, which invalidates retained lookup caches and resets `content_generation` to zero (`src/terminal_screen_model.cpp:4200`, `src/terminal_screen_model.cpp:4204`, `src/terminal_screen_model.cpp:4207`).

`advance_row_content_generation_if_changed` is the row mutation gate for existing retained line identities (`src/terminal_screen_model.cpp:4252`). It increments profiling counters for row comparisons and cells compared (`src/terminal_screen_model.cpp:4258`), then calls `rows_have_same_selection_content` under a dedicated compare profile scope (`src/terminal_screen_model.cpp:4266`). If the rows match, it returns without bumping generation. If they differ, it invalidates retained lookup caches, checks overflow, and increments `row.retained_line_provenance.content_generation` (`src/terminal_screen_model.cpp:4277`, `src/terminal_screen_model.cpp:4284`).

The comparison deliberately excludes style and hyperlink identity. `cells_have_same_selection_content` compares only `text`, `display_width`, `wide_continuation`, and `occupied` (`src/terminal_screen_model.cpp:4220`). This is consistent with a selection-content generation, not a visual-generation or full-cell-generation. A pure style or hyperlink rewrite can still be terminal content and dirty for rendering, but should not necessarily invalidate selection content anchored by retained text.

Printable ASCII path:

- `put_text` groups consecutive printable ASCII characters and dispatches them to `put_printable_ascii_text` (`src/terminal_screen_model.cpp:4520`, `src/terminal_screen_model.cpp:4536`).
- `put_printable_ascii_text` writes runs row-by-row, handling autowrap/no-autowrap boundaries (`src/terminal_screen_model.cpp:4555`).
- The normal span case calls `write_printable_ascii_span` (`src/terminal_screen_model.cpp:4637`).
- `write_printable_ascii_span` copies the entire current row (`src/terminal_screen_model.cpp:4654`, `src/terminal_screen_model.cpp:4658`), writes only the span (`src/terminal_screen_model.cpp:4675`), compares whole-row selection content via `advance_row_content_generation_if_changed` (`src/terminal_screen_model.cpp:4676`), then marks the row dirty (`src/terminal_screen_model.cpp:4677`).
- The no-autowrap clipped-row case repeats the same row-copy and generation compare pattern (`src/terminal_screen_model.cpp:4597`, `src/terminal_screen_model.cpp:4601`, `src/terminal_screen_model.cpp:4619`).

The profile counters already distinguish row copies and content-generation comparisons: `printable_ascii_row_copies`, `printable_ascii_row_copy_cells`, `row_content_generation_comparisons`, `row_content_generation_comparison_cells`, and `row_content_generation_advances` (`include/vnm_terminal/internal/terminal_screen_model.h:246`, `include/vnm_terminal/internal/terminal_screen_model.h:248`). Source-based inference: these counters are the right existing instrumentation to confirm the exact child cost of the row-wide work before changing behavior.

## All producers and consumers

Producers and mutators of row `content_generation`:

- `replace_retained_line_id` resets generation to zero for a new retained identity (`src/terminal_screen_model.cpp:4200`, `src/terminal_screen_model.cpp:4207`).
- `advance_row_content_generation_if_changed` conditionally increments generation when row selection content changes (`src/terminal_screen_model.cpp:4252`, `src/terminal_screen_model.cpp:4284`).
- `resize_rows` preserves retained identities for existing rows, resizes/repairs the cell vector, and calls `advance_row_content_generation_if_changed` only for existing retained rows (`src/terminal_screen_model.cpp:4167`, `src/terminal_screen_model.cpp:4175`, `src/terminal_screen_model.cpp:4185`).
- Printable ASCII no-autowrap clipping calls it after local row writes (`src/terminal_screen_model.cpp:4597`, `src/terminal_screen_model.cpp:4619`).
- Normal printable ASCII spans call it after span writes (`src/terminal_screen_model.cpp:4652`, `src/terminal_screen_model.cpp:4676`).
- Single printable ASCII cell writes call it (`src/terminal_screen_model.cpp:4704`, `src/terminal_screen_model.cpp:4707`).
- Combining/zero-width scalar overflow handling can clear the old base cell and call it before wrapping/reinstalling content (`src/terminal_screen_model.cpp:4789`, `src/terminal_screen_model.cpp:4791`).
- `install_cell_span`, used for spacing scalars and combined cells, calls it after installing base and continuation cells (`src/terminal_screen_model.cpp:4826`, `src/terminal_screen_model.cpp:4848`).
- Erase operations call it through `erase_row_range` and full visible-screen erase when not rebuilding a primary repaint candidate (`src/terminal_screen_model.cpp:4945`, `src/terminal_screen_model.cpp:4950`, `src/terminal_screen_model.cpp:4982`, `src/terminal_screen_model.cpp:4984`).
- Insert/delete character operations call it after row shifts and wide-span repair (`src/terminal_screen_model.cpp:5092`, `src/terminal_screen_model.cpp:5117`, `src/terminal_screen_model.cpp:5127`, `src/terminal_screen_model.cpp:5157`).

Consumers of row `content_generation`:

- Retained history handles encode retained-line identity as epoch, row sequence, record size, and `content_generation` (`include/vnm_terminal/internal/selection_contract.h:77`, `include/vnm_terminal/internal/selection_contract.h:85`, `include/vnm_terminal/internal/selection_contract.h:98`).
- Retained lookup compares expected handles against live row handles and reports `CONTENT_GENERATION_MISMATCH` when the same retained line id has changed content (`src/terminal_screen_model.cpp:100`, `src/terminal_screen_model.cpp:3360`, `src/terminal_screen_model.cpp:3369`).
- The retained lookup cache is keyed by row sequence and stores full `terminal_history_handle_t`, so generation changes are part of cache validity (`include/vnm_terminal/internal/terminal_screen_model.h:467`, `include/vnm_terminal/internal/terminal_screen_model.h:474`, `src/terminal_screen_model.cpp:3435`, `src/terminal_screen_model.cpp:3443`, `src/terminal_screen_model.cpp:3565`).
- Selection line leases are constructed from retained identity plus `content_generation` (`include/vnm_terminal/internal/selection_contract.h:226`, `include/vnm_terminal/internal/selection_contract.h:233`) and are resolved through `retained_line_lookup` (`src/terminal_screen_model.cpp:3626`).
- Render snapshots publish `Terminal_render_line_provenance` with `logical_row`, `retained_line_id`, and `content_generation` (`src/terminal_screen_model.cpp:2921`, `include/vnm_terminal/internal/render_snapshot.h:344`).
- Public projection treats matching retained fragments as same source only when retained line id and `content_generation` match (`src/terminal_public_projection.cpp:47`) and builds history handles from snapshot provenance (`src/terminal_public_projection.cpp:55`).
- Public projection copied rows carry `history_handle` derived from provenance (`src/terminal_public_projection.cpp:326`).
- Session public projection release reconciliation surfaces a content-generation mismatch as `DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED` (`src/terminal_session.cpp:5735`).
- QSG row text/resource cache identity includes active buffer, logical row, retained line id, and `content_generation` (`src/qsg_terminal_renderer.cpp:1011`, `src/qsg_terminal_renderer.cpp:1022`, `src/qsg_terminal_renderer.cpp:1041`). Text runs receive generation from render line provenance (`src/qsg_terminal_renderer.cpp:6252`), and row text grouping/resource reuse requires generation equality (`src/qsg_terminal_renderer.cpp:3351`).
- Transcript diagnostics serialize visible row provenance including `content_generation` (`src/terminal_transcript.cpp:201`, `src/terminal_transcript.cpp:205`), and transcript replay prints it for diagnostics (`tools/transcript_replay/terminal_transcript_replay.cpp:348`, `tools/transcript_replay/terminal_transcript_replay.cpp:1195`).
- History row record codec serializes `record.provenance.content_generation` into both header generation fields and validates they match on read (`src/terminal_history_row_record_codec.cpp:591`, `src/terminal_history_row_record_codec.cpp:593`, `src/terminal_history_row_record_codec.cpp:679`).

Related but distinct producer: `Terminal_session::advance_selection_content_basis_for_model_result` increments a session-level selection content basis on any terminal content or active-buffer change (`src/terminal_session.cpp:4881`). This is not the same field as retained-line `content_generation`, but it interacts with selection validation and visual lease advancement.

## Heuristic-only vs correctness consumers

Correctness consumers:

- Retained history handles and lookup require `content_generation` to distinguish same retained line id with changed selection content (`include/vnm_terminal/internal/selection_contract.h:85`, `src/terminal_screen_model.cpp:100`, `src/terminal_screen_model.cpp:3369`). If a changed row fails to bump generation, a stale handle can resolve as exact.
- Selection line leases use these handles to prove selected logical rows still refer to the same retained content (`include/vnm_terminal/internal/selection_contract.h:226`, `src/terminal_screen_model.cpp:3626`). A missed bump can incorrectly preserve selection; an unnecessary bump can unnecessarily reject or degrade selection/projection paths.
- Public projection anchoring and release reconciliation use generation mismatches to distinguish exact retained anchors from changed anchors (`src/terminal_public_projection.cpp:47`, `src/terminal_session.cpp:5735`).
- QSG row text cache identity uses `content_generation` for row-level text resource reuse (`src/qsg_terminal_renderer.cpp:1011`, `src/qsg_terminal_renderer.cpp:3351`). A missed bump can permit stale resource reuse if dirty-row publication or per-cell rebuild does not otherwise force replacement; an over-eager bump can reduce cache reuse.
- History row record serialization and validation persist the generation as part of row identity (`src/terminal_history_row_record_codec.cpp:591`, `src/terminal_history_row_record_codec.cpp:679`).

Heuristic-only consumers:

- Primary repaint recovery is controlled by `recover_scrollback_from_primary_repaints` and begins candidates only when enabled, primary, non-origin, cursor-hidden, full scroll region, and resize guard inactive (`src/terminal_screen_model.cpp:5844`).
- The recovery shift detector compares row text vectors derived from candidate/current cells, not retained `content_generation` (`src/terminal_screen_model.cpp:5981`, `src/terminal_screen_model.cpp:5994`, `src/terminal_screen_model.cpp:6000`, `src/terminal_repaint_recovery.cpp:16`, `src/terminal_repaint_recovery.cpp:71`).
- The heuristic tracks ambiguous visible row identity separately: any `mark_terminal_content_changed` while a candidate is active in primary marks `visible_row_identity_ambiguous` (`src/terminal_screen_model.cpp:6127`, `src/terminal_screen_model.cpp:6133`). Candidate finish/cancel then replaces retained line ids if ambiguity cannot be safely resolved (`src/terminal_screen_model.cpp:5901`, `src/terminal_screen_model.cpp:5910`, `src/terminal_screen_model.cpp:5931`).
- Recovery proposal metadata records whether visible row identity was ambiguous (`include/vnm_terminal/internal/terminal_screen_model.h:163`, `src/terminal_screen_model.cpp:5976`).

Source-based conclusion: the row full-copy/full-compare path is not primarily a scrolling/repaint-recovery heuristic. It is the current production mechanism for retained-line selection-content identity. The recovery heuristic is adjacent and can force identity replacement, but disabling recovery does not remove the correctness consumers above.

## Can this be disabled when the heuristic is disabled?

No, not as a blanket rule. Disabling `advance_row_content_generation_if_changed` when `recover_scrollback_from_primary_repaints` is false would break retained-line correctness because the generation is consumed outside primary repaint recovery.

Evidence:

- The default model config has `recover_scrollback_from_primary_repaints = false` (`include/vnm_terminal/internal/terminal_screen_model.h:56`), and many tests/tools explicitly run with recovery disabled (`tests/helpers/primary_backing_test_config.h:18`, `tests/conformance/parser_libfuzzer_harness.cpp:35`, `tools/transcript_replay/terminal_transcript_replay.cpp:642`). Those paths still need retained lookup, selection, snapshots, and renderer cache correctness.
- Surface/session expose primary repaint recovery as a runtime property (`include/vnm_terminal/vnm_terminal_surface.h:45`, `src/vnm_terminal_surface.cpp:1979`, `src/terminal_session.cpp:2454`). The session forwards it to the model and the model cancels only recovery guards/candidates on disable (`src/terminal_session.cpp:2466`, `src/terminal_screen_model.cpp:4491`). There is no source evidence that disabling the property is intended to disable retained identity semantics.
- Primary recovery gates are local to guard/candidate entry points (`src/terminal_screen_model.cpp:5748`, `src/terminal_screen_model.cpp:5814`, `src/terminal_screen_model.cpp:5844`). `advance_row_content_generation_if_changed` itself has no recovery-config guard because it serves broader retained-line identity.

What can be disabled or gated:

- Recovery candidate capture, resize repaint guards, scrollback synthesis, and ambiguity handling can and already do follow the recovery enablement flag (`src/terminal_screen_model.cpp:4484`, `src/terminal_screen_model.cpp:5748`, `src/terminal_screen_model.cpp:5844`).
- If future profiling proves a specific recovery-only row identity operation remains active when recovery is disabled, gate that specific operation. The full-row `content_generation` compare is not such an operation.

## Checksum/fingerprint option

A row fingerprint could replace whole-row comparison for broad mutators if it tracks exactly the same selection-content fields as `cells_have_same_selection_content`: `text`, `display_width`, `wide_continuation`, and `occupied` (`src/terminal_screen_model.cpp:4220`). It must intentionally exclude `style_id` and `hyperlink_id` unless the contract changes from selection-content identity to visual/content identity.

Benefits:

- For full-row or hard-to-localize mutations, comparing a stored fingerprint before/after can avoid a second full cell walk.
- A debug-only or assert-mode fingerprint can validate span-local detection during migration.
- It centralizes the definition of row selection-content identity if implemented as a shared cell contribution/hash helper.

Costs and risks:

- Incremental fingerprints require old and new cell contributions. For printable ASCII writes, obtaining old contributions only for touched cells and affected wide-span boundaries is essentially the same problem as span-local detection.
- Whole-row recomputation of a fingerprint before and after would still be row-width work and would not fix the Nelostie hot-path shape.
- Hash collisions are unacceptable for correctness unless the fingerprint is used only as a fast negative/positive with a fallback exact compare on equality or is constructed as a non-colliding structural digest, which is unlikely to be simpler than local exact checks.
- Wide spans, continuation repairs, erase-with-current-style behavior, resize repair, combining marks, and no-autowrap margin overwrites make update accounting non-trivial.

Recommendation: do not make fingerprinting the first printable ASCII fix. Consider it as a secondary helper for broad row operations, debug validation, or a future row metadata cache after span-local exact detection is in place.

## Printable ASCII span-local option

Printable ASCII has a strong local contract: it writes one-cell occupied ASCII cells with current style/hyperlink metadata, but selection-content generation only cares about text, width, continuation, and occupancy. The normal span writer knows `row`, `first_column`, and `text` (`src/terminal_screen_model.cpp:4648`). That is enough to decide whether selected-content fields changed for most cells without copying the full row.

Proposed shape:

- Before writing each cell in the ASCII span, inspect only the affected base cell and any wide-span cells that `clear_cell_at` will erase.
- Track a local `selection_content_changed` boolean when old selection-content differs from the intended final ASCII cell or when clearing a wide base/continuation changes any participating cell.
- Write the span using the existing content helpers initially, but skip the full-row `before_cells` copy and call a new generation helper such as `advance_row_content_generation_if_changed(row, selection_content_changed)`.
- Preserve current dirty marking and terminal-content marking behavior.
- Handle no-autowrap clipping with the same local check, because it also writes a bounded row-local set of cells (`src/terminal_screen_model.cpp:4597`, `src/terminal_screen_model.cpp:4619`).

Important edge cases:

- Overwriting an identical printable character with a different style or hyperlink should not bump retained-line `content_generation` under the current `cells_have_same_selection_content` contract, but it must remain terminal content/dirty for rendering.
- Overwriting a wide glyph base or continuation with ASCII changes selection content for the affected wide span, even if the target column receives a visually similar character. `clear_cell_at` expands through `cell_base_position` and `clear_cell_span` (`src/terminal_screen_model.cpp:4877`).
- No-autowrap clipping writes the last input character into the last column after optionally writing a prefix span (`src/terminal_screen_model.cpp:4612`, `src/terminal_screen_model.cpp:4617`). Local detection must match that exact final-state behavior, not the skipped characters.
- Combining marks handled through `append_zero_width_scalar` and `install_cell_span` should remain on the conservative existing path until a separate local detector is designed (`src/terminal_screen_model.cpp:4753`, `src/terminal_screen_model.cpp:4820`).
- Erase, insert/delete cells, resize, and wide repair can keep the full-row path initially because they are less likely to dominate the measured printable ASCII profile and have broader row effects.

Source-based inference: this option directly attacks the measured hot shape while preserving all correctness consumers, because it changes only how the boolean “selection content changed” is produced for ASCII spans.

## Recommended implementation plan

1. Add or confirm child-scope/counter measurement for row copy, row comparison, span calls, copied cells, compared cells, and generation advances on the same Nelostie profile. Existing counters already exist in `Terminal_screen_model_profile_stats` (`include/vnm_terminal/internal/terminal_screen_model.h:246`).
2. Introduce a small helper defining exact selection-content equality for one cell and, if useful, intended printable ASCII cell selection content. Reuse the current semantic fields from `cells_have_same_selection_content` (`src/terminal_screen_model.cpp:4220`).
3. Implement span-local selection-content change detection only for `write_printable_ascii_span` and the no-autowrap clipped row path. Leave other callers on full-row copy/compare in the first implementation batch.
4. Replace the printable ASCII full-row `before_cells` allocation/copy with a local changed flag and a generation bump helper. Keep `mark_terminal_content_changed`, dirty marking, current style/hyperlink writes, and cursor behavior unchanged.
5. Keep `advance_row_content_generation_if_changed` for broad row operations. Rename or overload carefully to avoid confusing row-snapshot comparison with boolean-known mutation decisions.
6. Add targeted tests before broadening the optimization: identical ASCII overwrite, style-only overwrite, hyperlink-only overwrite, ASCII over wide glyph base, ASCII over wide continuation, no-autowrap clipping, autowrap boundary, combining mark adjacency, retained history lookup mismatch, selection lease invalidation/preservation, public projection detached anchor reconciliation, and QSG row cache reuse.
7. Re-profile. Expected direction: `printable_ascii_row_copies` and `printable_ascii_row_copy_cells` should fall sharply for printable ASCII spans; `row_content_generation_comparison_cells` should fall with them; `row_content_generation_advances` should remain semantically equivalent.
8. Consider fingerprinting only after the span-local change is validated, and only for remaining broad-row hotspots.

## Validation requirements

Functional validation:

- Retained row generation bumps when ASCII changes text, occupancy, display width, or wide-continuation state.
- Retained row generation does not bump for pure style or hyperlink changes when selection-content fields are unchanged.
- Retained lookup reports `CONTENT_GENERATION_MISMATCH` for stale handles after real selection-content mutation (`src/terminal_screen_model.cpp:3369`).
- Selection visual lease and line lease behavior remains correct across dirty rows and retained provenance (`src/terminal_screen_model.cpp:3626`, `src/terminal_session.cpp:4881`).
- Public projection release reconciliation still distinguishes exact anchors, content-generation changes, geometry changes, and not-retained anchors (`src/terminal_session.cpp:5735`).
- QSG row cache identity updates when row text content changes and remains stable when only non-selection visual metadata changes if the renderer has separate dirty-cell handling (`src/qsg_terminal_renderer.cpp:1011`, `src/qsg_terminal_renderer.cpp:3351`).
- Primary repaint recovery enabled and disabled tests both pass, including candidate ambiguity and recovery-disabled non-scroll-source tests. Recovery-disabled paths must still produce correct retained generation.

Performance validation:

- Re-run the same Nelostie profile and compare `apply_action::print_text` total/mean/max (`docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:42`).
- Compare `printable_ascii_span_calls`, `printable_ascii_row_copies`, `printable_ascii_row_copy_cells`, `row_content_generation_comparisons`, `row_content_generation_comparison_cells`, and `row_content_generation_advances` before/after (`include/vnm_terminal/internal/terminal_screen_model.h:242`, `include/vnm_terminal/internal/terminal_screen_model.h:246`, `include/vnm_terminal/internal/terminal_screen_model.h:248`).
- Confirm parser ingest and style mutation totals do not regress materially; they were not the dominant measured cost (`docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:40`, `docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md:44`).

Review validation:

- Verify every optimized write path preserves the exact current `cells_have_same_selection_content` contract.
- Verify all broad row operations still invalidate retained lookup caches and update generation correctly.
- Verify the implementation does not couple retained generation to `recover_scrollback_from_primary_repaints`.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `include/vnm_terminal/internal/terminal_screen_model.h`
- `src/terminal_screen_model.cpp`
- `include/vnm_terminal/internal/selection_contract.h`
- `include/vnm_terminal/internal/render_snapshot.h`
- `include/vnm_terminal/internal/qsg_terminal_render_frame.h`
- `src/terminal_public_projection.cpp`
- `src/vnm_terminal_surface.cpp`
- `include/vnm_terminal/vnm_terminal_surface.h`
- `src/terminal_session.cpp`
- `include/vnm_terminal/internal/terminal_session.h`
- `src/qsg_terminal_renderer.cpp`
- `src/terminal_history_row_record_codec.cpp`
- `src/terminal_transcript.cpp`
- `tools/transcript_replay/terminal_transcript_replay.cpp`
- `include/vnm_terminal/internal/terminal_repaint_recovery.h`
- `src/terminal_repaint_recovery.cpp`
- `tests/helpers/primary_backing_test_config.h`
- `tests/conformance/parser_libfuzzer_harness.cpp`
- `tests/conformance/capture_replay_conformance_tests.cpp`
- `docs/profiling_investigations/nelostie_codex_ingest_print_text_report.md`
- `docs/profiling_investigations/nelostie_codex_ingest_cross_review.md`
- `docs/profiling_investigations/nelostie_profile_final_consolidated_report.md`
- `docs/profiling_investigations/nelostie_codex_snapshot_cross_review.md`
