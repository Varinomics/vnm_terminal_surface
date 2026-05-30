# Focus C retained-line content_generation consumer audit

## Executive summary

The retained-line `content_generation` field is a correctness identity, not just a profiler artifact. Span-local printing can reduce row copies, but any optimization that suppresses or coarsens generation advancement risks stale retained-line identity leaking into selection leases, public projection anchors, render-snapshot provenance, QSG row-cache reuse, and history-row handle validation.

The current consumer chain expects this pair to identify mutable retained-row content:

- `retained_line_id` identifies the durable retained row object.
- `content_generation` identifies the current content version of that retained row.

A safe next optimization may avoid copying or reduce comparison scope, but it must still advance `content_generation` exactly when the externally visible retained-row content or retained-row render payload changes, and it must not advance it for pure cursor, viewport, or unrelated-row changes.

The two supplied profiles show why this area is tempting to optimize. In the hardened profile, printable ASCII performed `55572` row copies and copied `48493629` cells. In the span-local profile, row copies are eliminated and replaced by `5183956` local cell inspections. However, both profiles still show very large retained-content comparison pressure: `937538` comparisons over `818329995` cells in hardened versus `887639` comparisons over `772753829` cells in span-local. That remaining cost is real, but the consumers below make false negatives much more expensive than the saved comparisons.

## Consumer map

- Retained lookup: `Terminal_screen_model::retained_line_lookup` resolves `terminal_history_handle_t` values whose retained identity is `retained_line_id` plus `content_generation`. It distinguishes exact matches, stale row identity, invalid handles, nearest predecessor/successor fallback, and explicit `CONTENT_GENERATION_MISMATCH`. The lookup cache may index primarily by row sequence, but validation must include content generation before treating a row as exact.

- Selection leases: `terminal_selection_line_lease_t` stores a history handle per selected logical row. `retained_line_descriptors_match` and render-snapshot selection construction must reject stale leases when the retained row id still exists but `content_generation` differs. This is what prevents a copied selection payload from remaining visually attached to a row whose text changed in place.

- Public projection: `Terminal_public_projection_row` copies snapshot cells plus retained-line provenance and creates `history_handle` from `retained_line_id` and `content_generation`. Fragment counting and wrapped-row identity also use the retained id/generation pair. Projection release/reconciliation must not treat the same retained id with a newer generation as the same public row content.

- Render snapshots: `Terminal_render_snapshot::visible_line_provenance` projects each visible row as `logical_row`, `retained_line_id`, and `content_generation`. Snapshot validation rejects malformed provenance, and the renderer uses this provenance to stamp text runs. Synthetic snapshots may preserve provenance only when grid and mapping remain compatible; otherwise they must clear or suppress it.

- QSG cache identity: `qsg_terminal_renderer.cpp` builds row-cache identity from active buffer, logical row, retained line id, and content generation. Text rows and geometry rows use this identity for reuse, clean-skip, and removal/reorder behavior. If generation does not advance after a content mutation, dirty-row data can be skipped as clean and stale pixels/text nodes can survive. If generation advances on unchanged content, QSG cache reuse collapses.

- History row codec and traversal: the row-record header stores retained line id and retained-line content generation. Decode with an expected handle checks generation and reports `CONTENT_GENERATION_MISMATCH`; traversal cache validation rejects stale cached rows when the generation in the live record differs from the handle. This makes content generation part of serialized retained-history integrity.

- Transcript/public diagnostics: snapshot transcript output records visible-line `content_generation`, and transcript/replay tests include public projection scroll snapshots. This is a secondary consumer: incorrect generation can make replay diagnostics self-consistent but wrong unless comparison logic checks the actual rendered content and provenance together.

## Required invariants

- Every retained row with a nonzero `retained_line_id` must have a nonzero, monotonically advancing `content_generation` after the first content mutation that makes the row externally observable.

- `content_generation` must advance when any retained-row render payload changes: text, occupied state, display width, wide-continuation state, style id, hyperlink id, hyperlink identity metadata, or any other field emitted into history records, render snapshots, public projections, or QSG text/geometry resources.

- `content_generation` must not advance for pure cursor movement, viewport movement, render-only publication, selection-only publication, QSG repaint, transcript recording, or unrelated-row mutation.

- The retained identity tuple for exact matching is at least `(buffer_id, retained_line_id, content_generation)`. Consumers that also need row position or buffer state may extend the tuple, but must not drop `content_generation`.

- A reused `retained_line_id` across content versions is valid only when generation changes. A reused id with unchanged generation means consumers are allowed to reuse old selection leases, QSG row nodes, projection rows, and history handles.

- Retained lookup cache rebuilds may index by row sequence/id for speed, but every exact-match result must validate content generation before reporting `OK` or `exact_match`.

- Selection leases must remain fail-closed. Missing, incomplete, reordered, or generation-stale line descriptors may preserve finalized payload text, but must not publish visual selection spans as if the retained row were still current.

- Public projection rows must carry history handles derived from the same provenance as the copied row cells. A row copied from a snapshot and a handle fetched from the model must agree on retained id and generation before the projection treats it as exact.

- Wrapped-row fragment identity must key fragment ordinals by both retained id and content generation. Otherwise an in-place rewrite of a wrapped retained row can inherit stale fragment indexes.

- Render snapshots with non-empty selection spans must have valid visible-line provenance, because selection span suppression and QSG identity depend on it.

- Synthetic render snapshots may preserve visible-line provenance only when the preserved grid, viewport mapping, active buffer, and row ordering still describe the same visible rows. If geometry changes make that uncertain, provenance must be cleared and selection spans suppressed.

- QSG clean-row skip is permitted only for rows with valid retained provenance and an unchanged identity including content generation. Dirty rows without valid provenance must rebuild or use a fallback, never silently skip.

- History row codec round-trips must preserve `content_generation`, and expected-handle validation must continue returning `CONTENT_GENERATION_MISMATCH` for generation drift even when byte sequence and row sequence match.

- Any future span-local optimization that replaces full-row comparison must have a proof that untouched cells, wide-boundary repair effects, hyperlink side tables, style changes, and blank/occupied transitions cannot change outside the inspected span.

## False-negative risks

- Stale selection attachment: a row mutates in place, generation does not advance, and visual leases still resolve. The user sees a selection highlight over content that no longer matches the copied payload.

- Stale QSG pixels: row-cache identity remains unchanged after content mutation, so text or geometry nodes are reused or clean-skipped. This can produce visible stale text even though model state is correct.

- False exact public projection release: synchronized-output release reconciles to a retained id whose content changed while hidden, causing hidden content or wrong scroll anchor semantics to be treated as public-safe.

- Corrupted history handle semantics: traversal or decode accepts a handle for an older row body because id/sequence matches and generation was not checked or not advanced.

- Replay/debug blind spot: transcript events can appear internally consistent if both the copied content and provenance are stale in the same way, masking a live rendering bug.

- Wrapped-row fragment confusion: fragment ordinals can be matched against a same-id but different-generation retained line, especially around reflow, public projection, and release reconciliation.

## False-positive costs

- QSG cache churn: unnecessary generation advances force row resource replacement, destroy text descriptor reuse, and increase child-node cleanup. The hardened profile already shows `8511` cumulative text rebuilds; extra advances would push more clean rows into rebuild paths.

- Public projection churn: harmless generation bumps make exact anchors look stale, causing nearest-predecessor/successor fallback or deferred scroll behavior even when content is unchanged.

- Selection detach churn: leases become payload-only too often, removing visual selection attachment across benign events such as cursor-only publications or viewport moves.

- History-cache misses: traversal cache entries are invalidated unnecessarily, increasing decode and directory lookup work.

- Profile noise: generation advances become less correlated with actual row content changes, making future profiling less actionable.

## Tests that prove consumers

- `tests/render_snapshot/render_snapshot_tests.cpp`: `test_model_snapshots_publish_visible_line_provenance` checks snapshot provenance against model-retained provenance and verifies content mutation keeps retained id but advances generation. `test_visible_line_provenance_validation` rejects missing, zero-id, or wrong logical-row provenance. Selection request tests reject incomplete, stale-generation, and reordered retained-line leases.

- `tests/backend_session/backend_session_tests.cpp`: `test_flat_ring_phase1_history_handle_resolution_statuses` and `test_flat_ring_phase5c_retained_lookup_cache_rebuild_and_validation` check retained lookup generation mismatches and cache rebuild behavior. `test_flat_ring_phase2a_selection_handle_resolution_policy` proves stale retained handles preserve payload but drop visual selection. `test_flat_ring_phase2c_public_projection_handle_resolution_policy` checks public projection rows carry history handles.

- `tests/history_row_record_codec/history_row_record_codec_tests.cpp`: `test_dense_and_blank_rows_round_trip`, `test_wide_clusters_styles_hyperlinks_and_recovery_round_trip`, and `test_header_footer_and_handle_validation_failures` prove row-record encoding preserves provenance and rejects expected-handle generation mismatch.

- `tests/history_row_record_codec/history_row_traversal_tests.cpp`: `test_cache_hits_validate_live_identity` proves traversal cache hits validate content generation and report `CONTENT_GENERATION_MISMATCH` through codec status.

- `tests/screen_operations/model_ops_tests.cpp`: `test_retained_line_content_generation_mutations` and retained provenance lifecycle tests prove retained ids remain stable while generation changes on content mutation, and that resize/recovery paths preserve or replace identity according to retained-row semantics.

- `tests/qsg_render/qsg_render_tests.cpp`: QSG row-cache tests cover active-buffer identity, exact content descriptors, dirty/clean skip safety, stale-pixel changes for blank scroll identity, and retained-line provenance pixels. These prove that row-cache reuse is identity-sensitive and that invalid provenance must rebuild instead of clean-skipping.

- `tests/transcript/transcript_tests.cpp`: writer/reader and public-projection replay tests cover serialized visible-line provenance and public projection scroll snapshot diagnostics, including rejection of unmatched public projection scroll snapshots before release.

## Missing tests

- A focused span-local regression test where a printable span changes only part of a retained row, with untouched prefix/suffix containing style, hyperlink, wide-continuation, and occupied/blank transitions, proving generation advances exactly once and only for the changed row.

- A QSG test that changes retained-row content while preserving `retained_line_id`, then asserts the old row-cache entry is not clean-skipped because only `content_generation` changed.

- A public projection test that mutates a retained row behind synchronized output while preserving retained id, then verifies release does not report exact anchor when generation differs.

- A selection lease test that uses a same-id/different-generation handle across wrapped visual fragments, proving stale fragment descriptors suppress visual spans rather than remapping to a newer fragment.

- A history traversal test that covers retained identity handles created by `terminal_history_handle_from_retained_identity`, not only normal row-record handles, and verifies same-id generation mismatch is rejected after cache rebuild.

- A transcript/replay test that corrupts only visible-line `content_generation` while leaving row text unchanged, proving diagnostics notice provenance drift separately from text drift.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\selection_contract.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_render_frame.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_public_projection.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_history_row_record_codec.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_public_projection.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_row_record_codec.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_row_traversal.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_history_ring.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_transcript.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\render_snapshot\render_snapshot_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\backend_session\backend_session_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\history_row_record_codec\history_row_record_codec_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\history_row_record_codec\history_row_traversal_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\screen_operations\model_ops_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\qsg_render\qsg_render_tests.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\tests\transcript\transcript_tests.cpp`
