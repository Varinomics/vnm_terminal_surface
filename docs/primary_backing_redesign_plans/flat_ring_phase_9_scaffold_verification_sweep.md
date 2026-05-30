# Flat Ring History Phase 9 Evidence: Scaffold Verification Sweep

## Scope

Phase 9 is verification-only. It performs the final scaffold sweep for the
flat-ring retained-history migration and records durable close evidence.

Verified axes:

1. No retained deque object-store scaffold remains.
2. No scrollback hyperlink refcount or side-map helper remains.
3. No retained resize-mutation helper remains.
4. No scan-only retained lookup helper remains.
5. No deleted eviction-delta viewport or selection repair helper remains.
6. No `_legacy`, `_v2`, retained-storage fallback, permanent mirror, storage
   switch, or dual-write path remains in retained-history scope.

Out of scope:

1. No first-time cleanup.
2. No production behavior change.
3. No production edits.

## Baseline Git State

Baseline command before Phase 9 evidence edits:

```powershell
git rev-parse HEAD
git status --short
```

Baseline revision:

```text
cb04e97cdf8d33906100edd00d097c6e3cb721eb
```

Baseline worktree state before Phase 9 evidence edits:

```text
 M CMakeLists.txt
 M docs/primary_backing_redesign_plans/README.md
 M include/vnm_terminal/internal/selection_contract.h
 D include/vnm_terminal/internal/terminal_backing_delta_viewport_sync.h
 M include/vnm_terminal/internal/terminal_public_projection.h
 M include/vnm_terminal/internal/terminal_screen_model.h
 M include/vnm_terminal/internal/terminal_session.h
 M include/vnm_terminal/internal/viewport_contract.h
 M src/selection_contract.cpp
 D src/terminal_backing_delta_viewport_sync.cpp
 M src/terminal_public_projection.cpp
 M src/terminal_screen_model.cpp
 M src/terminal_session.cpp
 M src/viewport_contract.cpp
 M tests/CMakeLists.txt
 M tests/backend_session/backend_session_tests.cpp
 M tests/render_snapshot/render_snapshot_tests.cpp
 M tests/screen_operations/model_ops_tests.cpp
 M tests/viewport/viewport_controller_tests.cpp
?? docs/primary_backing_redesign_plans/flat_ring_history_design_plan.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_1_history_handle_vocabulary.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_2a_selection_handle_resolution.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_2b_viewport_handle_resolution.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_2c_public_projection_handle_resolution.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_3_active_to_retained_record_producer.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_4a_flat_ring_primitive.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_4b_row_record_codec.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_4c_traversal_checkpoint_rebuild.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_4d_ring_materialization_parity_harness.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_5a_resize_visual_projection.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_5b_retained_hyperlink_metadata_authority.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_5c_retained_lookup_cache_replacement.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_6a_ring_cutover_readiness_gate.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_6b_authoritative_ring_cutover.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_7_recovery_shared_producer_verification.md
?? docs/primary_backing_redesign_plans/flat_ring_phase_8_benchmark_report.json
?? docs/primary_backing_redesign_plans/flat_ring_phase_8_encoding_performance_memory_tightening.md
?? include/vnm_terminal/internal/terminal_history_ring.h
?? include/vnm_terminal/internal/terminal_history_row_record_codec.h
?? include/vnm_terminal/internal/terminal_history_row_traversal.h
?? src/terminal_history_ring.cpp
?? src/terminal_history_row_record_codec.cpp
?? src/terminal_history_row_traversal.cpp
?? tests/history_ring/
?? tests/history_row_record_codec/
```

The baseline was already dirty from prior Phase 0-8 work. Phase 9 did not
revert or normalize those changes.

## Static Audit

Audit command:

```powershell
function Invoke-Audit($name, $pattern, [string[]]$paths) {
    Write-Output "---AUDIT: $name---"
    $result = & rg -n -S $pattern @paths -g '*.h' -g '*.cpp' -g 'CMakeLists.txt' 2>$null
    if ($LASTEXITCODE -eq 0) { $result }
    elseif ($LASTEXITCODE -eq 1) { Write-Output '(no matches)' }
    else { Write-Output "(rg failed with exit $LASTEXITCODE)"; exit $LASTEXITCODE }
}

Invoke-Audit `
    'deleted retained deque storage and Phase 4D fixture symbols' `
    '\b(scrollback_row_t|retained_history_row|append_retained_history_row|take_oldest_retained_history_row|retained_row_record_for_testing|terminal_retained_row_record_for_testing_t|terminal_retained_row_cell_for_testing_t|vnm_terminal_history_row_materialization_parity|history_row_materialization_parity_tests)\b|std::deque<retained_row_record_t>|retained_history\.push_back|retained_history\.pop_front|retained_history\.front' `
    @('include', 'src', 'tests', 'tests/CMakeLists.txt')

Invoke-Audit `
    'pointer-returning retained-row API names' `
    '\b(for_each_retained_history_row|retained_history_row_at|retained_row_at|retained_line_at|retained_row_ptr|retained_history_row_ptr|retained_row_pointer|retained_history_row_pointer|scrollback_row_at|scrollback_row_ptr)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'retained resize mutation helper names' `
    '\b(resize_scrollback_rows|mutate_retained_history_rows|resize_retained_history_rows|mutate_scrollback_rows|rewrite_retained_rows_for_resize|reflow_retained_history_rows|retained_resize_mutation)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'scrollback hyperlink refcount and side-map helpers' `
    '\b(m_scrollback_hyperlink_identity_keys|m_scrollback_hyperlink_ref_counts|add_scrollback_hyperlink_refs|remove_scrollback_hyperlink_refs|retain_referenced_hyperlink_ids|hyperlink_id_for_identity|scrollback_hyperlink_refcount|hyperlink_refcount)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'scan-only retained lookup markers' `
    '\b(scan_retained|retained_scan|scan_only|find_retained_line|find_retained_row|retained_line_descriptor_scan|selection_descriptor_lookup)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'deleted eviction-delta viewport/selection repair helpers' `
    '\b(Terminal_backing_delta_viewport_sync|terminal_backing_delta_viewport_sync|sync_scrollback_rows|selection_backing_eviction|repair_selection_after_eviction|repair_viewport_after_eviction|eviction_delta)\b' `
    @('include', 'src', 'tests', 'CMakeLists.txt')

Invoke-Audit `
    'retained-history storage alias fallback mirror dual-path spellings' `
    '(?i)\b\w+(_v2|_legacy)\b|\b(retained|scrollback|storage|ring|row|codec|encoder|deque)[-_ ]?(legacy|v2|fallback|mirror|switch)\b|\b(legacy|v2|fallback|mirror|switch)[-_ ]?(retained|scrollback|storage|ring|row|codec|encoder|deque)\b|\bcompatibility[-_ ]?fallback\b|\bdual[-_ ]?write\b|\bdualwrite\b' `
    @('include/vnm_terminal/internal/terminal_screen_model.h', 'src/terminal_screen_model.cpp', 'include/vnm_terminal/internal/terminal_history_ring.h', 'src/terminal_history_ring.cpp', 'include/vnm_terminal/internal/terminal_history_row_record_codec.h', 'src/terminal_history_row_record_codec.cpp', 'include/vnm_terminal/internal/terminal_history_row_traversal.h', 'src/terminal_history_row_traversal.cpp', 'include/vnm_terminal/internal/terminal_session.h', 'src/terminal_session.cpp', 'include/vnm_terminal/internal/terminal_public_projection.h', 'src/terminal_public_projection.cpp', 'tests/history_ring', 'tests/history_row_record_codec', 'tests/screen_operations', 'tests/backend_session', 'tests/CMakeLists.txt')

Invoke-Audit `
    'expected non-storage broad hits for documentation classification' `
    '(?i)\b(std::deque|fallback|legacy|mirror|compatibility_evicted_scrollback_rows|is_retained_identity_compatibility_handle|m_deferred_synchronized_evicted_scrollback_rows)\b' `
    @('include/vnm_terminal', 'src', 'tests')
```

Negative audit output:

```text
---AUDIT: deleted retained deque storage and Phase 4D fixture symbols---
(no matches)
---AUDIT: pointer-returning retained-row API names---
(no matches)
---AUDIT: retained resize mutation helper names---
(no matches)
---AUDIT: scrollback hyperlink refcount and side-map helpers---
(no matches)
---AUDIT: scan-only retained lookup markers---
(no matches)
---AUDIT: deleted eviction-delta viewport/selection repair helpers---
(no matches)
---AUDIT: retained-history storage alias fallback mirror dual-path spellings---
(no matches)
```

Expected broad-hit classification:

| Hit group | Classification |
| --- | --- |
| `std::deque` in `Terminal_history_ring` descriptor storage | Rebuildable record-descriptor index/cache, not retained row object storage and not a fallback path. |
| `std::deque` in session, backend, and session-contract command queues | Unrelated command/write queues outside retained-history storage. |
| `fallback` in public-projection test labels and release helper names | Documented public projection reconciliation behavior, not old retained storage fallback. |
| `fallback` in `tests/history_row_record_codec/history_row_record_codec_tests.cpp` | Oversize assertion proves no partial or fallback record is published; it is not hidden dual-path evidence. |
| `fallback` in renderer, shaping, transcript, conformance, parser, input, and surface-host tests | Unrelated software-rendering, font, parsing, input, transcript, or test-default terminology. |
| `legacy` in mouse/input and transcript fixtures | Unrelated protocol or transcript-compatibility fixtures. |
| `mirror` in surface-host resized-grid label | Unrelated test prose. |
| `is_retained_identity_compatibility_handle` and `compatibility_evicted_scrollback_rows` | Compatibility vocabulary for existing retained identity/result surfaces; no compatibility fallback or dual retained-history path was found. |
| `m_deferred_synchronized_evicted_scrollback_rows` | Deferred row-origin accounting for synchronized-output release, not retained-history storage or eviction repair scaffold. |

## Focused Gate

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_history_phase8_benchmark vnm_terminal_screen_operations vnm_terminal_backend_session vnm_terminal_render_snapshot vnm_terminal_viewport --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec|row_traversal|phase8_benchmark)$|^vnm_terminal_(screen_operations|backend_session|render_snapshot|viewport)$"" --output-on-failure"
```

Gate output summary on 2026-05-30:

```text
[vcvarsall.bat] Environment initialized for: 'x64'
ninja: no work to do.
Test project C:/plms/varinomics/vnm_terminal_surface/build
1/8 Test  #7: vnm_terminal_history_ring ...............   Passed
2/8 Test  #8: vnm_terminal_history_row_record_codec ...   Passed
3/8 Test  #9: vnm_terminal_history_row_traversal ......   Passed
4/8 Test #10: vnm_terminal_history_phase8_benchmark ...   Passed
5/8 Test #16: vnm_terminal_render_snapshot ............   Passed
6/8 Test #35: vnm_terminal_screen_operations ..........   Passed
7/8 Test #38: vnm_terminal_viewport ...................   Passed
8/8 Test #39: vnm_terminal_backend_session ............   Passed
100% tests passed, 0 tests failed out of 8
```

## Verification Verdict

Phase 9 close verdict: green.

No orphaned production scaffold was found. No owning migration phase must be
reopened by this verification sweep.

## Phase Gate Table

| Gate | Phase 9 result |
| --- | --- |
| Scope | Verification-only scaffold sweep. Phase 9 added this durable evidence artifact and the README entry only. |
| Behavior axis | No selection, viewport, public projection, resize, recovery, storage, codec, traversal, or performance behavior changed. |
| Recovery state | Recovery behavior is unchanged from Phase 7 and Phase 8. The focused gate still covers recovery through `vnm_terminal_screen_operations` and backend/session integration. |
| Evidence | Static audits found no forbidden retained-history scaffold, no retained deque object-store symbols, no hyperlink refcount helpers, no resize mutation helpers, no scan-only lookup helpers, no deleted eviction-delta repair helpers, and no retained-history fallback/mirror/dual-path aliases. The focused MSVC x64 gate passed 8/8 tests on 2026-05-30. |
| Baseline outcome | Baseline revision was `cb04e97cdf8d33906100edd00d097c6e3cb721eb` with a dirty worktree from prior Phase 0-8 work. Phase 9 preserved that worktree and performed no production cleanup. |
| Exit predicate | The plan can close when the static audit remains negative, the focused gate passes, and no production scaffold needs to be sent back to an owning migration phase. This Phase 9 sweep satisfies that predicate. |
| Deletion ownership | None. Phase 9 found no orphaned production scaffold. If a later review reclassifies a broad-hit item as retained-history scaffold, cleanup belongs to the phase that orphaned or introduced that item, not Phase 9. |
| Rollback mechanism | Delete this evidence artifact and remove its README entry. No production rollback is required because Phase 9 changed no production code. |
| Split triggers | Any actual cleanup need would block Phase 9 and return to the owning phase. No such split trigger fired. |
