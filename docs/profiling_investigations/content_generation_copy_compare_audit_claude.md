I have enough material. Producing the standalone audit report now.

# Review

Session: `01_content_generation_copy_compare_audit` — Content generation copy/compare and heuristic gating audit
Source-of-truth library: `C:\plms\varinomics\vnm_terminal_surface` @ `cb04e97c`
Hardened profile evidence: `vnm_terminal/build/profiles/nelostie_profile_hardened.txt` (233 × 871 grid, `apply_action::print_text` = 35.4 s / 472,802 calls — 98.1 % of `apply_parser_actions`).

## Findings

- **[severity=high] [category=production] [confidence=high] `src/terminal_screen_model.cpp:4648-4678`, `src/terminal_screen_model.cpp:4593-4624`, `src/terminal_screen_model.cpp:4698-4708` — `write_printable_ascii_span`, the no-autowrap clipped row path, and `write_printable_ascii_cell` each unconditionally deep-copy the whole row (`before_cells = screen_row.cells`, 4658/4601/4704) then re-walk the whole row inside `advance_row_content_generation_if_changed` (4252-4285) via `rows_have_same_selection_content` (4235-4250). The cost is O(row_width) per span/cell regardless of how few cells the span actually mutates.** Why it matters: this is the dominant Nelostie ingest cost (~35 s; lower-bound 411 M cell-copies + 411 M cell-compares at 871 cols). The work is also profile-instrumented inside `Terminal_screen_model::apply_action::print_text::row_copy` and `advance_row_content_generation_if_changed::compare` scopes (4599, 4656, 4265), so the hot path is already labelled. Follow-up: replace the copy/compare for these three call sites with span-local detection — for each cell in `[first_column, first_column+len)` compare just the four selection-content fields (`text`, `display_width`, `wide_continuation`, `occupied`) of the existing base cell plus any wide-base/continuation that `clear_cell_at` (4877-4893) will overwrite, accumulate a boolean, then call a new overload `advance_row_content_generation_if_changed(row, /*changed=*/flag)` that skips the row walk and only bumps + invalidates retained caches when the flag is set. Leave all other producers on the current full-row path.

- **[severity=high] [category=architecture] [confidence=high] `src/terminal_screen_model.cpp:4484-4496` and `include/vnm_terminal/internal/terminal_screen_model.h:56` — The full-row generation work cannot be gated by `recover_scrollback_from_primary_repaints`.** `content_generation` is a retained-line identity component with correctness consumers that are active regardless of the recovery flag, and the default config has the flag off (`recover_scrollback_from_primary_repaints = false`). Correctness consumers:
  - Retained handle match: `src/terminal_screen_model.cpp:100-142` returns `CONTENT_GENERATION_MISMATCH` when the same `row_sequence` has a different generation; `:3360-3370` propagates that into `Terminal_retained_line_lookup_result`.
  - Selection line leases: `selection_contract.h:138-142`, `:226-235` carry the generation into `terminal_history_handle_t`; `src/terminal_screen_model.cpp:3621-3636` rejects descriptors whose handle generation no longer matches.
  - Public projection same-fragment identity: `src/terminal_public_projection.cpp:45-60` (`provenance_describes_same_retained_fragment_source` requires generation equality; `projection_history_handle_from_provenance` builds the cross-process handle).
  - Session release reconciliation: `terminal_session.cpp:5735` distinguishes `DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED` from geometry/not-retained mismatches.
  - QSG row text cache identity: `src/qsg_terminal_renderer.cpp:1006-1042` (`row_cache_identity_t` includes `content_generation`) and `:3343-3353` (`text_resource_descriptor_run_is_eligible` requires generation equality).
  - Render snapshot: `src/terminal_screen_model.cpp:2918-2923` publishes `content_generation` per visible row provenance.
  - Persisted history row record: `terminal_history_row_record_codec.cpp` rejects on `CONTENT_GENERATION_MISMATCH` (tests at `tests/history_row_record_codec/history_row_record_codec_tests.cpp:437`, `…/history_row_traversal_tests.cpp:207`).

  Why it matters: this rules out the "just turn the comparison off when recovery is off" shortcut. Disabling the bump on the dominant write path silently corrupts retained handle matching, selection preservation, public projection reconciliation, QSG cache reuse, and ring serialization. Follow-up: keep `content_generation` semantics intact; reduce the *cost* of bump-decisioning on printable ASCII rather than the *frequency* of bumps.

- **[severity=high] [category=architecture] [confidence=high] `src/terminal_screen_model.cpp:5839-5888`, `src/terminal_screen_model.cpp:5981-6005`, `src/terminal_screen_model.cpp:6127-6136` — Primary repaint recovery uses a separate signal (`visible_row_identity_ambiguous`, raised by `mark_terminal_content_changed`) and a separate row-text comparison (`primary_repaint_recovery_shift_rows` ? `terminal_repaint_recovery.cpp`). It does NOT consume `content_generation`.** Why it matters: this confirms there is no design coupling that prevents replacing the printable ASCII full-row compare with a cheaper local detector. The recovery candidate enable gates at `:5844-5854` and the guard arms at `:5748-5754` / `:5814-5818` already follow `recover_scrollback_from_primary_repaints`; nothing flows back into `advance_row_content_generation_if_changed`. Follow-up: in the proposed span-local writer, continue to invoke `mark_terminal_content_changed()` (i.e., keep the path through `write_printable_ascii_cell_content` at 4710-4724 unchanged) so the recovery heuristic still sees ambiguity events.

- **[severity=medium] [category=production] [confidence=high] `src/terminal_screen_model.cpp:4970-4988` — `erase_visible_screen` non-rebuild branch performs a full-row copy + full-row compare per row even though the new contents are uniformly `erased_cell()`.** When the recovery candidate is inactive, every row is copied, refilled with the same erased cell, and then re-walked. For an idempotent CLS this is pure waste; for a populated screen the "changed" decision is provable from "was any cell occupied or did `erased_cell()` change" — both cheap row-local checks. Why it matters: this path is the second most natural full-row hotspot after printable ASCII, and is the simplest broad mutator to convert to the same `advance_row_content_generation_if_changed(row, bool)` overload. Follow-up: short-circuit by scanning whether any row cell differs from `erased_cell()` and from its prior contents in a single sweep, or trust a "row had any occupied/styled cell" precomputed during fill.

- **[severity=medium] [category=production] [confidence=high] `src/terminal_screen_model.cpp:5086-5119`, `:5121-5159` — `insert_cells` and `delete_cells` already know unambiguously that the row content shifted (the `std::move_backward`/`std::move` is a row-content mutation by construction).** Today they still pay `before_cells = row` + `advance_row_content_generation_if_changed` walk to "decide" something that is almost always true and is invariant when `count == 0` (already excluded by `std::clamp(count, 1, …)`). Why it matters: not on the dominant Nelostie shape, but a no-cost win once the boolean overload exists. Follow-up: pass `/*changed=*/true` unconditionally — these mutators always change selection-content fields by construction when `count >= 1`. (Verify against the existing `idempotent erase-character does not increment generation` test in `tests/screen_operations/model_ops_tests.cpp:3148-3150` — that test exercises `erase_row_range`, not `insert_cells`/`delete_cells`, so the change is safe.)

- **[severity=medium] [category=consolidation] [confidence=medium] `include/vnm_terminal/internal/terminal_screen_model.h:627-629` — `advance_row_content_generation_if_changed(row, const std::vector<Cell>& before_cells)` is the only generation gate today.** If span-local detection is added, the function will be called both ways. Why it matters: overloading the same name for "compare snapshots" and "trust this flag" is a footgun: a caller that switches from snapshot-based to flag-based loses the cell walk that previously caught subtle wide-boundary effects. Follow-up: add a distinct entry point — `advance_row_content_generation(row, bool changed)` or `advance_row_content_generation_with_change_flag(row, bool)` — and keep `advance_row_content_generation_if_changed` strictly for snapshot-based callers. Both must funnel through the same cache-invalidate + overflow-check tail (currently 4277-4284).

- **[severity=medium] [category=missing-coverage] [confidence=high] `tests/screen_operations/model_ops_tests.cpp:3102-3181` is strong but does not cover (a) ASCII overwriting a wide-CONTINUATION column (only the wide-base case at 3152-3157), (b) hyperlink-only overwrite — only style-only is covered (3173-3178), (c) the no-autowrap clipped row path (no isolated test), (d) public projection / session reconciliation triggered by ASCII span-local mutation specifically (vs the broad backend_session tests around `backend_session_tests.cpp:12548-12557, 12656-12667`).** Why it matters: these are exactly the contract corners a span-local rewrite is most likely to mis-handle. Follow-up: add unit tests for the four cases above before introducing the optimization so the proposed change has a regression gate.

- **[severity=low] [category=production] [confidence=high] `src/terminal_screen_model.cpp:4789-4791` — Combining-mark wrap-on-overflow also performs a full-row copy + compare.** Frequency is small relative to printable ASCII. Why it matters: the Codex audit explicitly recommends keeping this on the conservative path during phase 1; flagging for the record so it doesn't get accidentally rewritten under the same pass. Follow-up: leave on the current full-row path.

- **[severity=low] [category=consolidation] [confidence=medium] `src/terminal_screen_model.cpp:4252-4275` — Profile counters for "comparison" and "advances" are well-instrumented, but there is no counter for *cells span-locally inspected*.** Why it matters: once span-local detection lands, the existing counters will reflect the absence of full-row compares but will lose visibility into how much per-cell work the new detector does. Follow-up: add `printable_ascii_span_local_cells_inspected` (or similar) to `Terminal_screen_model_profile_stats` (header :238-268) so before/after Nelostie comparisons remain attributable.

## Checksum / fingerprint — answer

Not as the first fix for the printable ASCII hot path. To match the existing `cells_have_same_selection_content` contract (`src/terminal_screen_model.cpp:4225-4233`) a fingerprint would still need:

- *Both* a before- and after-row scan (same row-width work, just with different constant factors), or
- Incremental cell contributions, which require knowing the old cell contribution for every touched index plus every wide-base/continuation cleared by `clear_cell_at`/`clear_cell_span` — the same accounting problem as direct span-local detection.

Hash collisions are unacceptable for a correctness gate driving retained handle identity, so a fingerprint would need either a structural digest (more expensive than `cells_have_same_selection_content`) or a fast-positive/fast-negative followed by exact compare on equality (which keeps the row walk on the hot path).

Where a fingerprint *does* earn its keep: (a) as a debug parity oracle to validate span-local detection during migration, and (b) eventually for the broad mutators that genuinely walk a row anyway (resize_rows, erase_visible_screen, install_cell_span). Defer until span-local detection lands.

## Can the mechanism be disabled when the heuristic is disabled? — answer

No, not as a blanket rule. The default-on consumers above are independent of `recover_scrollback_from_primary_repaints`. The disable switch at `src/terminal_screen_model.cpp:4484-4496` correctly cancels only recovery guards/candidates and leaves `advance_row_content_generation_if_changed` reachable. What *can* be gated:

- All entry points already gated: candidate begin (5844-5854), resize guard arming (5748-5754, 5814-5818), erase-visible candidate rebuild (4974-4985, branches by `m_primary_repaint_recovery_candidate.active`).
- The `visible_row_identity_ambiguous` plumbing in `mark_terminal_content_changed` (6127-6136) is already conditional on `m_primary_repaint_recovery_candidate.active`.

Nothing in the full-row compare path is recovery-specific.

## Recommended implementation plan

1. **Baseline measurement.** Enable `Terminal_screen_model_profile_stats` on the Nelostie hardened profile; record `printable_ascii_span_calls`, `printable_ascii_row_copies`, `printable_ascii_row_copy_cells`, `row_content_generation_comparisons`, `row_content_generation_comparison_cells`, `row_content_generation_advances` (header :242-250).

2. **Add missing tests first** (gate, not chaser): in `tests/screen_operations/model_ops_tests.cpp` near the existing `test_retained_line_content_generation_mutations` (3102-3181):
   - ASCII over wide-continuation column ? bump.
   - Hyperlink-only rewrite of identical text ? no bump (symmetric to the existing style-only test at 3173-3178).
   - No-autowrap clipped row: identical content ? no bump; last-column character change ? bump.
   - Public projection same-fragment matching across a span-local ASCII write that does not change selection content (uses `provenance_describes_same_retained_fragment_source` at `terminal_public_projection.cpp:45-52`).

3. **Introduce a strictly additive helper** in `terminal_screen_model.h`/`.cpp`:
   ```
   void advance_row_content_generation_with_change_flag(Terminal_screen_row& row, bool changed);
   ```
   Both this and the existing snapshot-based overload share a private tail that does the cache invalidate + overflow check + bump (`terminal_screen_model.cpp:4271-4284`). Add a small `printable_ascii_intended_cell_selection_content(QChar)` helper that returns the 4-tuple a final ASCII cell will hold.

4. **Convert three call sites:**
   - `write_printable_ascii_span` (4648-4678): drop `before_cells`, accumulate a `selection_content_changed` boolean across `[first_column, first_column+text.size())`, then call the flag-based helper. For each cell: if `cell_base_position(target)` differs from the target, the base display_width > 1, or `cells_have_same_selection_content` between the existing cell and the intended ASCII cell is false, set the flag.
   - No-autowrap clipped path (4593-4625): same shape, plus the final-column character (4616-4618) must be inspected against its current cell.
   - `write_printable_ascii_cell` (4698-4708): trivial single-cell version of the above.
   - Keep `mark_terminal_content_changed` / `mark_dirty` / `write_printable_ascii_cell_content` exactly as today so the recovery heuristic still sees ambiguity events and dirty-row stats are unaffected.

5. **Leave broad mutators on the current full-row path for this phase.** Specifically: `resize_rows` (4162-4187), `erase_row_range` (4932-4952), `erase_visible_screen` (4970-4988), `install_cell_span` (4816-4850), `insert_cells`/`delete_cells` (5086-5159), combining-mark wrap (4789-4791).

6. **Re-run Nelostie.** Verify `printable_ascii_row_copies` and `printable_ascii_row_copy_cells` drop ~3 orders of magnitude (one row copy per broad-mutator call is expected to remain). Verify `row_content_generation_advances` is unchanged for the equivalent sequence (semantic parity). Verify `apply_action::print_text` total + mean drop materially.

7. **Optional second pass** — only if subsequent profiling demands:
   - `erase_visible_screen` non-rebuild branch (4982-4985): trust `/*changed=*/true` unless the row was already filled with the current `erased_cell()`; cheap scan replaces full compare.
   - `insert_cells`/`delete_cells`: pass `/*changed=*/true` directly.
   - Fingerprinting as a debug oracle if any broad mutator becomes dominant.

## Test Assessment

The contract is well-anchored at `tests/screen_operations/model_ops_tests.cpp:3102-3181` (`test_retained_line_content_generation_mutations`) — printable span / idempotent / cursor / insert-cell / delete-cell / erase-blank / erase-mutate / erase-idempotent / wide-occupancy / combining / variation / style-only-no-bump — and at `tests/backend_session/backend_session_tests.cpp:12548-12557, 12656-12667` (`CONTENT_GENERATION_MISMATCH` propagates through retained_line_lookup and through the cache hit). History-record codec generation parity is covered by `tests/history_row_record_codec/history_row_record_codec_tests.cpp:437` and `…/history_row_traversal_tests.cpp:207`. `tests/screen_basic/basic_model_tests.cpp:451-500` (`test_printable_ascii_span_semantics`) covers autowrap, wide-base overlap, combining, and styled space — but does not assert generation behavior on these paths.

Most important missing coverage for this scope:
- ASCII writing *over a wide-continuation column* — only the wide-base case is tested.
- Hyperlink-only rewrite of identical text (symmetric to the existing style-only test).
- The no-autowrap clipped row path (covered for cursor/text behavior in basic_model_tests, but not for `content_generation`).
- A span-local-vs-row-snapshot parity oracle: no test directly asserts that the row-snapshot detector and any future span-local detector produce identical `content_generation` sequences across a varied byte stream. This is the single highest-value test to add before optimizing.

Misleading test signal: `test_retained_line_content_generation_mutations` exercises 1-row, 4-8 column grids. The cost being optimized is O(row_width), so functional tests will pass identically pre- and post-optimization without re-validating the original full-row compare; they cannot prevent regressions in the *frequency* of bumps. Pair the new parity oracle with the existing counters (`row_content_generation_advances`) and assert counter equality between the two paths.

## Suggested Follow-ups

1. Land the four missing-coverage tests (ASCII-over-wide-continuation, hyperlink-only, no-autowrap clip identity, no-autowrap clip last-cell change) and a span-local-vs-row-snapshot parity oracle, all before any production change.
2. Implement the span-local detector + new `advance_row_content_generation_with_change_flag` helper for the three printable ASCII call sites (`write_printable_ascii_span`, no-autowrap clip path, `write_printable_ascii_cell`). Keep all other producers untouched.
3. Re-profile Nelostie hardened; confirm counter direction and `apply_action::print_text` improvement; record before/after in `docs/profiling_investigations/`.
4. Defer `erase_visible_screen` non-rebuild branch and `insert_cells`/`delete_cells` boolean-flag conversions to a second batch, only if measurable.
5. Defer fingerprinting; revisit only if a broad mutator becomes the next dominant cost.
6. Add `printable_ascii_span_local_cells_inspected` (or equivalent) to `Terminal_screen_model_profile_stats` so post-change profiles remain attributable.
7. Do not couple `advance_row_content_generation_*` to `recover_scrollback_from_primary_repaints` — the audit and the source confirm there is no valid gate there.

