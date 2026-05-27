# Selection Lifetime Phase 1 Inventory

Phase 1 scope is test-only and evidence-only. This inventory maps the selection
lifetime design contract to current observable tests, new Phase 1 repros, and
deferred evidence gates where a production hook or later implementation phase is
needed.

Evidence levels:

- `direct`: the test observes the contract item through public or existing
  internal test APIs.
- `indirect`: the test exercises adjacent behavior but does not prove the full
  contract item.
- `missing`: no current test can prove the item without a later hook or this
  phase adds a new repro.

## Baseline Boundary

Phase 1 starts from the current Phase 0 baseline in this working tree. That
baseline already includes the earlier minimal payload-preservation patch and
related tests. Phase 1 red evidence in this document is relative to that Phase 0
baseline unless a separate clean-baseline run is explicitly performed later.

Pre-existing dirty production/public-doc files not authored by Phase 1:

- `docs/public_surface.md`
- `include/vnm_terminal/internal/selection_contract.h`
- `src/selection_contract.cpp`
- `src/terminal_session.cpp`
- `src/vnm_terminal_surface.cpp`

Pre-existing rollout/document-contract risk:

- `docs/public_surface.md` still describes best-effort highlight drift, while
  the selection lifetime design requires stale spans to fail closed. That
  public-doc state was not authored by Phase 1 and must be resolved at the
  later public-doc rollout gate before finalizing the new public contract.

The Phase 1-authored changes are limited to test-only repros and this inventory
document. Rows below label payload-retention coverage that came from the
minimal payload patch as `Phase0/minimal-payload coverage` rather than
unqualified existing baseline coverage.

## Phase 1 Red-Failure Evidence

Baseline used for this evidence:

- Current working tree Phase 0 baseline with the pre-existing minimal
  payload-preservation implementation and related public-surface documentation
  changes already present.
- Phase 1 red evidence is relative to this Phase0/minimal-payload baseline, not
  a clean upstream baseline.
- A clean-baseline replay is optional future work if the Phase 0 patch is split
  into a separate commit and needs its own before/after evidence.

Focused build command:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build\validation-surface --target vnm_terminal_backend_session vnm_terminal_surface_host"
```

Focused test command:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && ctest --test-dir build\validation-surface -C Debug -R ""^vnm_terminal_(backend_session|surface_host)$"" --output-on-failure"
```

Before Phase 5, expected-red evidence mode was the default for these two
custom test binaries. The Phase 1 repros remained executed while the runner
recorded expected-red lines instead of failing the default green suite before
the owning later phase landed.

Phase 5 promoted the remaining stale-span repro predicates to normal green
tests. The custom expected-red helpers used before Phase 5 were removed from
the focused backend/session and surface-host runners.

Strict red-repro command:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && set VNM_TERMINAL_STRICT_PHASE1_RED_REPROS=1&& ctest --test-dir build\validation-surface -C Debug -R ""^vnm_terminal_(backend_session|surface_host)$"" --output-on-failure"
```

Strict mode now runs the same normal green tests as the default focused
backend/session and surface-host runners. Phase 3 turned
`test_selection_drag_rejects_snapshot_change` into normal green coverage by
binding drag hit-tests to a published source. Phase 5 turned the stale-span
repros into normal green coverage through visual lease/span compatibility
filtering.

Build result summary:

- `vnm_terminal_backend_session` built successfully.
- `vnm_terminal_surface_host` built successfully.
- The build emitted a non-fatal post-step message:
  `'pwsh.exe' is not recognized as an internal or external command`.

Backend red repro failures against the Phase0/minimal-payload baseline:

- `test_selection_spans_detach_when_selected_row_mutates` failed at
  `mutating selection detaches spans instead of highlighting replacement text`.
  Intended failing condition: after selected row text changes from `original` to
  `mutated!`, the retained copy payload remains `original`, but
  `selection_spans` are still emitted over the replacement row.
- `test_selection_spans_detach_after_synchronized_output_release` failed at
  `sync-release detaches spans instead of highlighting published replacement text`.
  Intended failing condition: synchronized output mutates the selected row while
  hidden, the retained copy payload remains `original`, but releasing
  synchronized output still emits spans over the published replacement row.

Default runner status after Phase 5: normal green coverage.

Backend resize evidence:

- `test_selection_spans_detach_when_resize_invalidates_selected_columns` is not
  red evidence after refinement. It is an inventory guard for an observable
  incompatible resize case where the selected columns no longer exist in the
  resized grid. It deliberately does not require detachment for benign
  column-count changes that a Phase 5 equivalence proof might preserve.

Surface-host red repro failures against the Phase0/minimal-payload baseline:

- `test_selection_visual_detach_after_row_mutation` failed at
  `surface mutation-detach emits no stale highlight over replacement text`.
  Intended failing condition: public `selected_text()` and Ctrl+C retain/copy
  `original`, public `selection_state()` remains `ACTIVE`, but the render
  snapshot still contains stale highlight spans over `mutated!`.
- `test_selection_drag_rejects_snapshot_change` now covers the Phase 3 normal
  green predicates:
  `snapshot-changing drag does not create selection state from incompatible sources`;
  `snapshot-changing drag exposes no mixed-source selected text`;
  `snapshot-changing drag emits no mixed-source selection spans`.
  Baseline failing condition from Phase 1: a drag anchored on one published
  snapshot is extended after a different snapshot is published, and the
  baseline combines incompatible sources into state, text, and spans instead of
  failing closed.

Default runner status after Phase 5: row-mutation stale highlight and
snapshot-changing drag are normal green tests.

New Phase 1 tests that were green on the Phase0/minimal-payload baseline:

- `test_selection_unicode_cluster_payloads`: wide-continuation, combining-mark,
  and variation-sequence selected-text payload checks are already satisfied.
- `test_no_payload_copy_fallback_states`: `DRAG_ARMED`, cancelled replacement,
  and active-empty copy fallback/consumption checks are already satisfied by the
  current baseline.

| Contract item | Evidence | Existing test coverage | Remaining gap | Disposition |
| --- | --- | --- | --- | --- |
| Public `selectionState` reports copyability rather than visual attachment | `direct` | `tests/surface_host/surface_host_tests.cpp::test_copy_shortcut_policy`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | No public visual-attachment state exists, so tests cannot distinguish `PAYLOAD_ONLY` except through no spans plus active copyability. | Covered by existing tests for `NONE` and `ACTIVE`; visual/copy split remains covered through new span-detach repros. |
| `selected_text()` payload retention after selected row mutation | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text` | Existing Phase 0 coverage did not prove stale spans disappear. | Covered for payload by Phase0/minimal-payload coverage; span gap covered by Phase 1 red repro `test_selection_spans_detach_when_selected_row_mutates`. |
| Ctrl+C copies retained payload after selected row mutation | `missing` | None before this phase. | Needed surface-level proof that copy payload and visual spans diverge safely. | Covered by new `test_selection_visual_detach_after_row_mutation`. |
| Selection spans are hidden when selected backing row mutates | `missing` | Existing tests showed spans for initial selection, clearing, alternate buffer, and eviction, but not row mutation over replacement text. | Manual drift bug class remained unpinned. | Covered by new red repros `test_selection_spans_detach_when_selected_row_mutates` and `test_selection_visual_detach_after_row_mutation`. |
| No-payload fallback for `NONE` | `direct` | `tests/surface_host/surface_host_tests.cpp::test_copy_shortcut_policy` | None for `COPY_SELECTION_OR_TERMINAL_INPUT`; `COPY_SELECTION_OR_IGNORE` intentionally consumes without copying. | Covered by existing test. |
| No-payload fallback for `DRAG_ARMED` | `missing` | None before this phase. | Existing tests began a drag and completed a selection, but did not press Ctrl+C while only armed. | Covered by new green characterization/guard coverage `test_no_payload_copy_fallback_states`. |
| Cancelled replacement clears prior payload and falls back for Ctrl+C | `missing` | None before this phase. | There is no separate cancellation hook; a press/release replacement with no drag is the observable surface gesture. | Covered by new green characterization/guard coverage `test_no_payload_copy_fallback_states`; if product later defines another cancel path, add Phase 2 state-transition coverage. |
| Active empty selection is copyable and consumes Ctrl+C as a local empty copy | `direct` | `tests/surface_host/surface_host_tests.cpp::test_copy_shortcut_policy` | None for the existing surface gesture that commits an empty payload. | Covered by existing test. |
| Synchronized-output hidden state is not selectable | `direct` | `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | Does not prove internal source identity, only public hidden-output behavior. | Covered for Phase 1 observability. |
| Synchronized-output retained payload while output is held | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text` | Existing Phase 0 coverage did not prove detachment when held replacement text is published. | Covered by new red repro `test_selection_spans_detach_after_synchronized_output_release`. |
| Detached visual state preserves copy payload | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | Row mutation and incompatible resize-column cases were missing. | Covered by Phase0/minimal-payload eviction/alternate-buffer tests and Phase 1 row-mutation, synchronized-release, and resize-invalidated-column repros. |
| Drag preview state | `indirect` | `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | Existing public API has no explicit `DRAG_PREVIEW` or preview payload hook. | Defer exact state-transition proof to Phase 2 internal state hook; Phase 1 adds snapshot-change drag repro. |
| Drag across incompatible snapshot change | `missing` | None before this phase. | Need proof that old anchor is not combined with a new incompatible extent. | Covered by normal green Phase 3 test `test_selection_drag_rejects_snapshot_change`. |
| Replacement gesture policy uses mouse-down clears | `indirect` | `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` covers terminal mouse takeover clearing selection. | Local replacement arming was not covered. | Covered by new green characterization/guard coverage `test_no_payload_copy_fallback_states`; complete enum transition table deferred to Phase 2. |
| Payload after viewport scroll/offscreen movement | `direct` | `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | Precise reattachment proof requires Phase 2 content-basis metadata and the Phase 5 visual lease/span compatibility gate. | Existing tests cover retained logical payload and visible spans while rows are observable. |
| Payload after row reuse/live scroll | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | Existing Phase 0 coverage used eviction/live scroll; visual drift over mutated same row was missing. | Covered by Phase0/minimal-payload eviction tests plus Phase 1 row-mutation repros. |
| Scrollback eviction keeps payload and hides spans | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text` | None for existing observable APIs. | Covered by Phase0/minimal-payload tests from the current baseline. |
| Alternate-buffer mismatch hides spans and keeps payload | `direct` | Phase0/minimal-payload coverage in `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text` | Surface-host alternate-buffer visual evidence, if needed, belongs to the Phase 5 visual lease/span gate after Phase 3 source identity is available. | Covered through backend/session render snapshots for Phase 1. |
| Resize/reflow incompatible or unproven geometry detaches visual spans | `direct` | Resize tests existed but not with active selection. | Benign geometry-only and reflow-equivalence behavior needs Phase 2 content-basis/reflow metadata and the Phase 5 visual lease/span gate. | Covered only for an observable incompatible case by `test_selection_spans_detach_when_resize_invalidates_selected_columns`; precise resize/reflow proof is deferred to Phase 5. |
| Unicode wide glyph selected once | `direct` | `tests/backend_session/backend_session_tests.cpp::test_selection_snapshot_and_visible_text` | Trailing continuation, combining, and variation cases were missing. | Existing wide-glyph coverage retained; missing cases covered by new `test_selection_unicode_cluster_payloads`. |
| Unicode trailing wide cell is not independent text | `missing` | None before this phase. | None through existing session selected-text API. | Covered by new `test_selection_unicode_cluster_payloads`. |
| Unicode combining marks stay attached to base cell | `missing` | None before this phase. | None through existing session selected-text API. | Covered by new `test_selection_unicode_cluster_payloads`. |
| Unicode variation sequences relevant to width stay attached | `missing` | None before this phase. | Width-classification provenance is not exposed, but selected payload is observable. | Covered by new `test_selection_unicode_cluster_payloads`; deeper width-identity compatibility deferred to Phase 5. |
| Payload capture comes from published source snapshot or a bit-equivalent source | `indirect` | `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_rejects_snapshot_change` | Payload is still extracted through the active model path after Phase 3 proves the drag source did not change. | Phase 4 owns render-snapshot payload extraction or an explicit bit-equivalence proof for every payload path. |
| Hidden synchronized output is not used as selectable source | `direct` | `tests/surface_host/surface_host_tests.cpp::test_selection_drag_and_selected_text`; `tests/surface_host/surface_host_tests.cpp::test_selection_drag_rejects_snapshot_change` | None for public behavior. | Covered for Phase 3; Phase 4 still owns payload-source proof internals. |
| Render span emission observes visual lease compatibility | `direct` | Existing render-snapshot `selection_spans` tests plus promoted Phase 5 stale-span tests and `test_selection_phase5_visual_lease_span_compatibility`. | Conservative unrelated-content detachment can be narrowed only by a future row/cell descriptor proof. | Covered by Phase 5 visual lease compatibility filtering and evidence doc. |
| Renderer remains passive | `missing` | No behavior-only test can prove renderer ownership boundaries. | Requires source review or instrumentation showing renderer only consumes emitted spans. | Defer to Phase 5 implementation review and Phase 6 validation/performance evidence gate. |
| No render-time full-range `selected_text()` comparison | `missing` | No Phase 1 test encodes this mechanism, by design. | Requires source review/performance evidence after implementation. | Defer to Phase 5 review and Phase 6 performance evidence gate. |

Deferred hooks and evidence gates:

- Phase 2 internal selection state hook: expose or otherwise test state
  transitions for `DRAG_ARMED`, `DRAG_PREVIEW`, `ATTACHED_VISIBLE`,
  `ATTACHED_HIDDEN`, and `PAYLOAD_ONLY` without adding public API unless
  product need is accepted.
- Phase 3 published snapshot-source hook: implemented by recording the source
  snapshot/content basis used for drag anchor and extent hit-tests so tests
  reject incompatible sources directly.
- Phase 4 payload extraction/equivalence gate: prove selected payload is
  extracted from the published hit-test source or from a bit-equivalent model
  path.
- Phase 5 visual lease/span gate: implemented by limiting span emission to
  compatible attached-visible leases and recording evidence in
  `selection_lifetime_phase5_visual_lease.md`.
- Phase 6 renderer/performance gate: review and measure that renderer paths do
  not perform full-range selected-text comparison per snapshot and remain
  passive consumers of spans.
