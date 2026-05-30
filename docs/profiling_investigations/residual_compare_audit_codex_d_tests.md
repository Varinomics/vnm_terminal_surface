# Residual full-row comparison audit - Focus D tests and parity strategy

## Executive summary

Existing tests already protect many consumers of retained-row `content_generation`: model operations, retained lookup, render snapshot provenance, public projection anchoring, selection behavior, Unicode shaping, and synchronized-output release paths all have targeted coverage. That coverage is useful but indirect for the next optimization because it checks outcomes after specific scenarios rather than proving that a new span-local change detector is equivalent to the current full-row selection-content comparison.

The smallest tests-first gate should be a model-level parity matrix that asserts one invariant for each candidate operation: if the row's selection-content signature changes, the retained row generation advances exactly once; if that signature is unchanged, generation does not advance. The signature must intentionally match the current comparator's contract: occupied state, text, display width, and wide-continuation state are included; style and hyperlink identity are excluded.

The next implementation should not start by touching all residual full-row comparisons. Start with printable text and no-autowrap clipping, because the profiles and previous audits identify that path as the largest row-width cost and because the operation has a local span contract. Keep erase/insert/delete, retained lookup, public projection, and QSG reuse as sentinels around the change rather than broadening the first batch.

Post-implementation profiling should compare the hardened and span-local Nelostie profiles in `C:\plms\varinomics\vnm_terminal\build\profiles`. The gate should require `print_text` row-copy and content-generation compare work to stop scaling with row width, while `content_generation` advances, retained lookup behavior, public projection release reconciliation, and QSG row cache reuse remain semantically stable.

## Existing coverage

| Area | Existing coverage observed | Audit note |
| --- | --- | --- |
| `content_generation` producer behavior | `tests\screen_operations\model_ops_tests.cpp` contains retained-line generation checks for idempotent rewrites, real ASCII changes, erase-character no-op behavior, wide-cell occupancy changes, wide-continuation overwrites, no-autowrap clipped identical rewrites, no-autowrap clipped last-cell changes, and combining sequence changes. | This is the strongest current precondition for replacing full-row comparisons. It covers important edge cases but is scenario-driven rather than an explicit full-row-versus-span-local parity oracle. |
| Printable text | `tests\screen_operations\model_ops_tests.cpp`, `tests\screen_basic\basic_model_tests.cpp`, parser/conformance fixtures, and backend-session tests exercise printable text publication, row retention, CR/LF boundaries, blank rows, and synchronized-output release behavior. | The existing tests prove visible and retained outcomes, but they do not isolate the generation-decision contract for ordinary span writes. |
| Wide cells | `tests\unicode_width\unicode_width_tests.cpp`, `tests\shaping_contract\shaping_contract_tests.cpp`, `tests\screen_operations\model_ops_tests.cpp`, `tests\backend_session\backend_session_tests.cpp`, and the `wide_combining_prompt.vnm_capture` fixture cover width classification, shaped ownership, overwrites, continuation selection exclusion, and conformance replay. | Wide start and continuation overwrites are covered at several layers. A parity test should still include wide-boundary ASCII overwrites because those are the highest-risk local span detector cases. |
| Combining marks | `tests\unicode_width\unicode_width_tests.cpp`, `tests\shaping_contract\shaping_contract_tests.cpp`, and `tests\backend_session\backend_session_tests.cpp` cover zero-width classification, cluster ownership, and selected text preserving `e\u0301`. | Existing coverage confirms behavior, but parity should add generation-specific base-plus-combining cases around printable overwrite and combining append. |
| No-autowrap | `tests\screen_operations\model_ops_tests.cpp` includes no-autowrap clipped identical rewrite and clipped last-cell change checks. | This is directly relevant to the current duplicate row-copy/full-compare shape in the clipped printable path. It should be part of the first gate. |
| Erase/insert/delete | `tests\screen_basic\basic_model_tests.cpp` checks styled whole-line and wide-line erase materialization; `tests\screen_operations\model_ops_tests.cpp` covers erase operations, wide damage, blank-fill current style, insert/delete lines, insert/delete cells, and tabs; `tests\backend_session\backend_session_tests.cpp` covers synchronized release movement across insert/delete line operations. | Coverage is broad, but these operations are more complex than printable text. Use them as guardrails first; defer implementation changes unless profiling still points at them after printable text is fixed. |
| Retained lookup | `tests\backend_session\backend_session_tests.cpp` includes flat-ring retained-history and lookup cache tests, including retained lookup cache rebuild/validation and handle resolution policies. `tests\screen_operations\model_ops_tests.cpp` also covers retained-line provenance lifecycle. | These tests are important consumer sentinels. Add one stale-handle mismatch case only if an existing test does not already assert generation mismatch directly after same retained id content mutation. |
| Public projection | `tests\backend_session\backend_session_tests.cpp` includes Phase 1 through Phase 8 public projection tests, including copied rows and metadata, viewport-only behavior, release reconciliation, wrapped fragment handling, multi-scroll release dirty behavior, policy latch cases, and public scroll using safe projection fields. | Public projection is well represented as a consumer. The missing piece is a small case that proves a missed generation bump would detach or fail anchoring predictably. |
| Render snapshot provenance | `tests\render_snapshot\render_snapshot_tests.cpp` checks visible-line provenance against model retained provenance, including `content_generation`, and uses retained identities for selection leases. | This is good indirect coverage for propagation from model to snapshot. It is not a replacement for a producer parity test. |
| QSG cache reuse | `src\qsg_terminal_renderer.cpp` keys row cache identity on active buffer, logical row, retained line id, and `content_generation`. `tests\qsg_text_node\qsg_text_node_tests.cpp` verifies the public QSGTextNode rendering route. | I did not find a focused test that asserts QSG row text/resource cache reuse across unchanged generations and invalidation across changed generations. This is the highest-value consumer-side gap. |
| Profiles | `nelostie_profile_hardened.txt` and `nelostie_profile_span_local.txt` exist under `vnm_terminal\build\profiles`; the hardened profile shows large-grid geometry and visible `print_text::row_copy` plus `advance_row_content_generation_if_changed::compare` scopes in timeline buckets. | These profiles are suitable for a before/after gate, but correctness must be gated by tests first because lower compare counts are not evidence of equivalent retained identity semantics. |

## Missing high-value tests

| Priority | Test | Purpose |
| --- | --- | --- |
| 1 | Printable span generation parity matrix | For one retained row, apply same-text rewrite, changed-text rewrite, shorter/longer span overwrite, style-only rewrite, hyperlink-only rewrite, wide-base overwrite, wide-continuation overwrite, and ordinary empty-cell write. Assert selection-content signature change exactly matches generation advance. |
| 2 | No-autowrap clipped parity matrix | In no-autowrap mode at the right margin, assert identical clipped rewrites do not advance generation while a clipped last-cell text change does. This covers the second explicit row-copy/full-compare path. |
| 3 | Combining and wide-boundary parity | Add generation-specific cases for appending a combining mark to a base cell, overwriting a combining base, overwriting a wide base, and overwriting a wide continuation. This protects the subtle local cleanup semantics a span-local detector must account for. |
| 4 | Erase/insert/delete generation sentinel | Keep this smaller than the printable matrix: one erase no-op, one erase that changes occupancy/text, one insert-character shift, one delete-character shift, one insert-line, and one delete-line. Assert signature/generation agreement for the affected retained rows. |
| 5 | Retained lookup stale-handle sentinel | Capture a retained handle, mutate the same retained row's selection content, then assert the old handle resolves as content-generation mismatch rather than exact. This catches missed bumps at the consumer boundary. |
| 6 | Public projection anchor sentinel | Capture a projection from a safe basis, mutate a retained row's selection content while preserving retained id, then assert release/anchor reconciliation classifies the row as changed rather than exact. This catches missed bumps in projection parity. |
| 7 | QSG cache reuse/invalidation sentinel | Build two frames where a row's retained id and generation are unchanged and assert text resource reuse; then build a frame where generation changes and assert reuse is rejected for that row. This should be a non-image, lifecycle-recorder style test if possible. |

The first implementation batch only needs priorities 1 through 3 as hard preconditions. Priorities 4 through 7 are high-value sentinels, but they can be split if adding them would make the pre-implementation batch too large.

## Parity oracle proposal

Use a test-side row selection-content signature as the oracle, not wall-clock timing and not QSG output pixels. The signature should serialize only the fields currently compared by retained-line selection-content equality:

| Included in signature | Excluded from signature |
| --- | --- |
| `occupied` | `style_id` |
| `text` | `hyperlink_id` |
| `display_width` | foreground/background attributes |
| `wide_continuation` | dirty flags and renderer-local cache state |

For each test operation, capture before and after state:

| Captured value | Expected assertion |
| --- | --- |
| Retained line id before/after | If retained id changes, generation parity is not the relevant assertion for that row; record it as identity replacement. |
| `content_generation` before/after | Same signature means same generation; changed signature means generation advances once for ordinary in-place row mutation. |
| Selection-content signature before/after | This is the oracle for whether generation should advance. |
| Optional render snapshot provenance | Snapshot `content_generation` must match model provenance after publication. |

This oracle deliberately mirrors the current full-row comparison semantics while freeing the implementation from retaining a full-row before image. It also makes style-only or hyperlink-only rewrites explicit: they may be visually dirty, but they must not advance selection-content generation unless the contract is intentionally changed.

For transition confidence, the tests can run the same scenario through a helper that records the affected row signature before and after each operation. That catches both false negatives, where span-local detection misses a real text/occupancy/width change, and false positives, where span-local detection advances generation for style-only or identical-content rewrites.

## Minimal pre-implementation gate

Add a tests-only batch before implementation. Keep it small and local to the model layer:

| Gate item | Minimum cases |
| --- | --- |
| Printable ordinary span parity | Identical ASCII rewrite, changed ASCII rewrite, ASCII over empty cells, style-only rewrite with same text, hyperlink-only rewrite with same text. |
| No-autowrap clipped parity | Identical clipped rewrite and changed clipped right-margin cell. |
| Wide/combining parity | ASCII over wide base, ASCII over wide continuation, combining mark append to base, ASCII overwrite of combining base. |
| Snapshot provenance smoke | After one changed and one unchanged generation case, render a snapshot and assert visible-line provenance carries the model generation. |

Pass condition:

| Condition | Required result |
| --- | --- |
| Selection-content signature unchanged | Retained id remains valid and `content_generation` is unchanged. |
| Selection-content signature changed in-place | Retained id remains valid and `content_generation` increments exactly once. |
| Retained identity replaced by the operation | Test records replacement explicitly and does not misclassify it as a generation failure. |
| Snapshot captured after mutation | Snapshot line provenance matches model retained provenance. |

This gate is intentionally smaller than the full consumer matrix. It blocks the most likely correctness regressions before the first span-local implementation and avoids turning the optimization into a multi-subsystem migration.

## Post-implementation profile gate

Run both requested Nelostie profiles after the implementation and compare against the current artifacts in `C:\plms\varinomics\vnm_terminal\build\profiles`.

| Profile gate | Expected result |
| --- | --- |
| Hardened profile `Terminal_screen_model::apply_action::print_text::row_copy` | Calls and total time should drop to zero for the printable paths that no longer copy full rows, or be limited to explicitly retained fallback paths. |
| Hardened profile `Terminal_screen_model::advance_row_content_generation_if_changed::compare` | Calls/cells should no longer scale with printable span count times full row width. Remaining calls should be attributable to broad mutators that were not in the first implementation batch. |
| Span-local profile printable text scopes | New span-local detection work should scale with touched cells and boundary repairs, not with the 873-column row width seen in the profile geometry. |
| `Terminal_screen_model::apply_action::print_text` total and mean | Should drop materially without shifting comparable time into a new span-local scope. |
| `row_content_generation_advances` or equivalent counters | Should remain semantically consistent with the tests: no drop indicating missed bumps and no spike indicating false positives. |
| Render snapshot provenance | No regression in visible-line `content_generation` propagation. |
| Retained lookup and public projection tests | No new exact-match behavior for stale handles or anchors whose selection content changed. |
| QSG row cache metrics | Unchanged-generation rows should remain reusable; changed-generation rows should invalidate. If new reuse counters exist, text cache reuse should not fall after a pure model-side optimization. |

Do not accept a profile improvement alone. The implementation is correct only if the parity tests pass first and the profile shows the intended scaling improvement afterward.

## Files inspected

| Path | Use in audit |
| --- | --- |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md` | Local coding and review standards. |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md` | Local formatting and documentation style guidance. |
| `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md` | Review/audit scope guidance. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\content_generation_copy_compare_audit_codex.md` | Prior source and consumer audit for row copy/full compare behavior. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\content_generation_copy_compare_audit_claude.md` | Prior companion audit. |
| `C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_profile_final_consolidated_report.md` | Consolidated profiling plan and validation gates. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_hardened.txt` | Hardened profile artifact requested by the audit. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_span_local.txt` | Span-local profile artifact requested by the audit. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp` | Source matches for generation producers and printable/full-row comparison paths. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_public_projection.cpp` | Source matches for projection retained identity and generation matching. |
| `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp` | Source matches for QSG row cache identity and generation-keyed reuse. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h` | Source matches for profile counters and retained provenance structures. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\selection_contract.h` | Source matches for retained handles and selection leases. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h` | Source matches for render-line provenance. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\backend_session\backend_session_tests.cpp` | Session, retained lookup, public projection, wide/combining selection, and synchronized-output consumer coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\render_snapshot\render_snapshot_tests.cpp` | Snapshot provenance and retained identity coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\screen_basic\basic_model_tests.cpp` | Basic row materialization and styled erase coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\screen_operations\model_ops_tests.cpp` | Core model operation and retained-line generation coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\unicode_width\unicode_width_tests.cpp` | Width classification coverage for ASCII, combining marks, and wide characters. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\shaping_contract\shaping_contract_tests.cpp` | Wide and combining shaping/ownership coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\qsg_text_node\qsg_text_node_tests.cpp` | QSGTextNode route coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\qsg_render\qsg_render_tests.cpp` | QSG render test search target for cache/reuse coverage. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\conformance\capture_replay_conformance_tests.cpp` | Replay and dirty-cell conformance search target. |
| `C:\plms\varinomics\vnm_terminal_surface\tests\conformance\README.md` | Conformance test configuration context. |
