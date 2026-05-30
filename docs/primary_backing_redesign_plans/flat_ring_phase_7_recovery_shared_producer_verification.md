# Flat Ring History Phase 7 Evidence: Recovery Shared-Producer Verification

## Scope

Phase 7 verifies the recovery policy boundary after authoritative ring cutover.

Implemented scope:

1. Recovered rows are verified as retained-history ring rows with recovered
   provenance.
2. Accepted recovery is verified through the shared retained-history producer
   and primary-history append path.
3. Recovery-disabled normal scrollout is verified as ordinary terminal-storage
   retained history.

Out of scope:

1. No recovery heuristic redesign.
2. No storage format change.
3. No feature behavior change.
4. No storage fallback, mirror, `_v2`, `_legacy`, dual-write, or
   recovery-specific append path.

## Phase Gate Table

| Gate | Phase 7 result |
| --- | --- |
| Scope | Verification and hardening only. Phase 7 adds a ring-native screen-operations regression and durable evidence; it does not change recovery policy or storage format. |
| Behavior axis | Recovery-enabled shifted repaint still appends one recovered row; recovery-disabled normal terminal scrollout still appends one terminal-storage row. |
| Recovery state | Enabled recovery proves `RECOVERED_PRIMARY_REPAINT` provenance on a materialized ring row. Disabled recovery proves normal scrollout has no recovery proposal and keeps `TERMINAL_STORAGE` provenance. |
| Evidence | `test_flat_ring_phase7_recovery_shared_producer_boundary` covers recovered provenance, authoritative ring handle lookup, producer metadata, row-local hyperlinks, style ids, and recovery-disabled normal scrollout. Existing Phase R tests cover resize-adjacent recovery suppression and runtime recovery toggle behavior. Focused `vcvarsall` x64 storage/recovery/backend gate passed on 2026-05-30. |
| Baseline outcome | Phase 6B made ring storage authoritative and deleted the deque path. Phase 7 validates that the Phase R recovery policy still terminates at the same retained-history producer and ring append path. |
| Exit predicate | A recovered row resolves through an authoritative ring handle, materializes recovered provenance and shared-producer fields, and has no separate recovery storage append. Recovery-disabled normal scrollout resolves through the same ring handle path with terminal-storage provenance. |
| Deletion ownership | Phase 7 owns deletion of any recovery-only append bypass found by the static audit. The production negative audit found no recovery-only append, bypass, fallback, mirror, dual-write, `_v2`, or `_legacy` storage path. |
| Rollback mechanism | Revert the Phase 7 screen-operations regression, this evidence artifact, and the README entry. No production rollback is needed because Phase 7 does not change production code. |
| Split triggers | If recovery acceptance policy must change, split to a Phase R-owned plan amendment. If a missing provenance field requires format work, split to the codec/storage owner before changing format. |

## Verification Approach

The Phase 7 regression exercises two storage-policy axes:

1. Recovery-enabled shifted repaint:
   `accept_primary_repaint_recovery_proposal` produces one primary-history
   append delta, the recovered row resolves through an authoritative ring
   handle, and materialization returns recovered provenance plus producer-owned
   source width, style lifetime, wrap state, hyperlink metadata, and style ids.
2. Recovery-disabled normal scrollout:
   ordinary CRLF scrollout produces one primary-history append delta, emits no
   recovery proposal, resolves through an authoritative ring handle, and
   materializes terminal-storage provenance.

Existing Phase R screen-operations coverage remains part of the gate for
resize-adjacent recovery suppression, repeated-row ambiguity, blank-only
suppression, helper behavior, and runtime recovery toggle cancellation.

## Deletion And Orphan Audit

Phase 7 does not introduce production code. The expected static proof is:

1. Accepted recovery calls `append_scrollback_row` from
   `accept_primary_repaint_recovery_proposal`.
2. `append_scrollback_row` calls `seal_retained_row_record`.
3. `append_retained_history_record` converts the sealed value to a
   `Terminal_history_row_record` and commits it through
   `encode_terminal_history_row_record_to_ring`.
4. No recovery-only ring append, storage fallback, mirror, `_v2`, `_legacy`,
   or dual-write path is present.

Focused audit command:

```powershell
rg -n -S "accept_primary_repaint_recovery_proposal|append_scrollback_row|seal_retained_row_record|append_retained_history_record|encode_terminal_history_row_record_to_ring|RECOVERED_PRIMARY_REPAINT|(?i)(recovery.*append|append.*recovery|recovered.*ring|fallback|mirror|dual[-_ ]?write|_v2|_legacy)" include/vnm_terminal/internal/terminal_screen_model.h src/terminal_screen_model.cpp tests/screen_operations/model_ops_tests.cpp
```

Production negative audit command:

```powershell
rg -n -S "(?i)(recovery[-_ ]?only|recovery.*bypass|bypass.*recovery|recovery.*ring.*append|recovered.*ring.*append|recovery.*fallback|fallback.*recovery|recovery.*mirror|mirror.*recovery|dual[-_ ]?write|_v2|_legacy)" include/vnm_terminal/internal/terminal_screen_model.h src/terminal_screen_model.cpp include/vnm_terminal/internal/terminal_history_ring.h src/terminal_history_ring.cpp include/vnm_terminal/internal/terminal_history_row_record_codec.h src/terminal_history_row_record_codec.cpp
```

Production negative audit result:

1. No matches.

Shared path proof:

1. `accept_primary_repaint_recovery_proposal` calls `append_scrollback_row`.
2. `append_scrollback_row` calls `seal_retained_row_record`.
3. `append_scrollback_row` calls `append_retained_history_record`.
4. `append_retained_history_record` calls
   `encode_terminal_history_row_record_to_ring`.

## Focused Gate

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_screen_operations vnm_terminal_backend_session --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec|row_traversal)$|^vnm_terminal_(screen_operations|backend_session)$"" --output-on-failure"
```

Gate output summary:

1. `vnm_terminal_history_ring` passed.
2. `vnm_terminal_history_row_record_codec` passed.
3. `vnm_terminal_history_row_traversal` passed.
4. `vnm_terminal_screen_operations` passed.
5. `vnm_terminal_backend_session` passed.
6. `100% tests passed, 0 tests failed out of 5`.
