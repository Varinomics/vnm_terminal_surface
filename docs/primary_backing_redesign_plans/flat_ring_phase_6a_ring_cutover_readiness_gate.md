# Flat Ring History Phase 6A Evidence: Ring Cutover Readiness Gate

## Scope

Phase 6A proves that the prerequisite flat-ring retained-history pieces are
present before any authoritative storage cutover. This phase is evidence-only.

Validated prerequisite axes:

1. Phase 4A flat byte-ring primitive.
2. Phase 4B row-record codec.
3. Phase 4C traversal and rebuildable directory cache.
4. Phase 3 active-to-retained producer.
5. Phase 5A resize-as-visual-projection behavior.
6. Phase 5B row-local retained hyperlink metadata authority.
7. Phase 5C retained lookup cache replacement.
8. Phase 4D current-storage-to-ring materialization parity harness.

Out of scope:

1. No production storage switch.
2. No authoritative ring cutover.
3. No cleanup ownership beyond deleting readiness-only diagnostics added by
   this phase.
4. No `_v2`, `_legacy`, fallback, mirror, dual-write, or storage switch path.

## Baseline Git State

Baseline command:

```powershell
git rev-parse HEAD
git status --short
```

Baseline revision before Phase 6A evidence edits:

```text
cb04e97cdf8d33906100edd00d097c6e3cb721eb
```

Baseline worktree state before Phase 6A evidence edits:

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
?? include/vnm_terminal/internal/terminal_history_ring.h
?? include/vnm_terminal/internal/terminal_history_row_record_codec.h
?? include/vnm_terminal/internal/terminal_history_row_traversal.h
?? src/terminal_history_ring.cpp
?? src/terminal_history_row_record_codec.cpp
?? src/terminal_history_row_traversal.cpp
?? tests/history_ring/
?? tests/history_row_record_codec/
```

The baseline was already dirty from prior Phase 0-5C work. Phase 6A did not
revert or normalize that baseline.

## Static Audit

Audit command:

```powershell
function Invoke-Audit($name, $pattern, [string[]]$paths) {
    Write-Host "---AUDIT: $name---"
    $result = & rg -n -S $pattern @paths -g '*.h' -g '*.cpp' 2>$null
    if ($LASTEXITCODE -eq 0) {
        $result
    }
    elseif ($LASTEXITCODE -eq 1) {
        Write-Host '(no matches)'
    }
    else {
        Write-Host "(rg failed with exit $LASTEXITCODE)"
    }
}

Invoke-Audit `
    'production storage does not call ring/codec/traversal authority' `
    'Terminal_history_ring|terminal_history_row_record|terminal_history_row_traversal|encode_terminal_history_row_record|decode_terminal_history_row_record|materialize_terminal_history_row|Terminal_history_row_directory' `
    @(
        'include/vnm_terminal/internal/terminal_screen_model.h',
        'src/terminal_screen_model.cpp',
        'include/vnm_terminal/internal/terminal_session.h',
        'src/terminal_session.cpp',
        'src/terminal_public_projection.cpp',
        'src/selection_contract.cpp',
        'src/viewport_contract.cpp')

Invoke-Audit `
    'pointer-returning retained-row API names' `
    '\b(for_each_retained_history_row|retained_history_row_at|retained_row_at|retained_line_at|retained_row_ptr|retained_history_row_ptr|retained_row_pointer|retained_history_row_pointer|scrollback_row_at|scrollback_row_ptr)\b' `
    @('include', 'src')

Invoke-Audit `
    'retained resize mutation helpers' `
    '\b(resize_scrollback_rows|mutate_retained_history_rows|resize_retained_history_rows|mutate_scrollback_rows)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'scrollback hyperlink refcount/side-map helpers' `
    '\b(m_scrollback_hyperlink_identity_keys|m_scrollback_hyperlink_ref_counts|add_scrollback_hyperlink_refs|remove_scrollback_hyperlink_refs|for_each_retained_history_row|hyperlink_metadata_for_cells|retain_referenced_hyperlink_ids|hyperlink_id_for_identity)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'scan-only retained lookup markers' `
    '\b(scan_retained|retained_scan|scan_only|find_retained_line|find_retained_row|retained_line_descriptor_scan|selection_descriptor_lookup)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'governance-forbidden storage switch aliases' `
    '\b(_v2|_legacy|dual_write|dualwrite|storage_switch|ring_switch|retained_ring_owner|storage_mirror|ring_mirror)\b' `
    @('include', 'src', 'tests')

rg -n -S `
    '\b(retained_line_lookup|retained_history_handle_at_logical_row|discard_retained_lookup_cache_for_testing|ensure_retained_lookup_cache|rebuild_retained_lookup_cache)\b' `
    include/vnm_terminal/internal/terminal_screen_model.h `
    src/terminal_screen_model.cpp `
    include/vnm_terminal/internal/terminal_session.h `
    src/terminal_session.cpp `
    tests `
    -g '*.h' `
    -g '*.cpp'
```

Audit outcome on 2026-05-30:

1. No production screen-model, session, public-projection, selection, or
   viewport caller references the ring, codec, traversal, or row-directory
   authority surfaces.
2. No pointer-returning retained-row API names were found in production
   `include` or `src`.
3. No retained resize mutation helper names were found in production or tests.
4. No deleted scrollback hyperlink refcount or scrollback side-map helper names
   were found in production or tests.
5. No scan-only retained lookup marker names were found in production or tests.
6. No governance-forbidden storage switch alias names were found in production
   or tests.
7. The remaining retained lookup references are the intended cache-backed
   `retained_line_lookup`, `retained_history_handle_at_logical_row`,
   `discard_retained_lookup_cache_for_testing`, and
   `rebuild_retained_lookup_cache` seams in `Terminal_screen_model`,
   `Terminal_session`, and focused backend-session tests.

## Integrated Gate

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S . -B build -DVNM_TERMINAL_SURFACE_BUILD_TESTING=ON && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_history_row_materialization_parity vnm_terminal_screen_operations vnm_terminal_backend_session vnm_terminal_render_snapshot vnm_terminal_viewport vnm_terminal_terminal_modes --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec|row_traversal|row_materialization_parity)$|^vnm_terminal_(screen_operations|backend_session|render_snapshot|viewport|terminal_modes)$"" --output-on-failure"
```

Gate output summary on 2026-05-30:

```text
[vcvarsall.bat] Environment initialized for: 'x64'
-- Configuring done
-- Generating done
-- Build files have been written to: C:/plms/varinomics/vnm_terminal_surface/build
Test project C:/plms/varinomics/vnm_terminal_surface/build
1/9 Test  #7: vnm_terminal_history_ring .........................   Passed
2/9 Test  #8: vnm_terminal_history_row_record_codec .............   Passed
3/9 Test  #9: vnm_terminal_history_row_traversal ................   Passed
4/9 Test #10: vnm_terminal_history_row_materialization_parity ...   Passed
5/9 Test #16: vnm_terminal_render_snapshot ......................   Passed
6/9 Test #35: vnm_terminal_screen_operations ....................   Passed
7/9 Test #37: vnm_terminal_terminal_modes .......................   Passed
8/9 Test #38: vnm_terminal_viewport .............................   Passed
9/9 Test #39: vnm_terminal_backend_session ......................   Passed
100% tests passed, 0 tests failed out of 9
```

The configure step also printed non-fatal missing `WrapVulkanHeaders` messages.
They did not block configure, build, or test execution.

## Readiness Verdict

Phase 6A readiness verdict: green for cutover preparation.

No failed prerequisite was found. No work is sent back to an owning phase by
this gate.

## Phase Gate Table

| Gate | Phase 6A outcome |
|---|---|
| Scope | Evidence-only readiness gate over primitive, codec, traversal, producer, resize projection, row-local hyperlinks, lookup caches, and parity harness. No production storage switch was made. |
| Behavior axis | Existing behavior axes from Phases 1-5C remain covered by integrated tests: handle resolution, producer output, resize projection, row-local hyperlink materialization, cache-backed lookup, traversal validation, and materialization parity. |
| Recovery state | Recovery behavior is unchanged. Accepted recovered rows still enter retained history through the existing producer, and Phase 4D recovered-row parity passed. |
| Evidence | Static audits passed with no forbidden production dependencies. Focused integrated `vcvarsall` x64 build and 9-test CTest gate passed on 2026-05-30. |
| Baseline outcome | Baseline revision `cb04e97cdf8d33906100edd00d097c6e3cb721eb` had a dirty worktree from prior Phase 0-5C work. The dirty baseline was recorded before Phase 6A evidence edits. |
| Exit predicate | Ring cutover can be considered only after this evidence remains green and the next phase makes the storage-authority switch without adding fallback, mirror, dual-write, pointer-retained-row, resize-mutation, scrollback-refcount, or scan-only lookup dependencies. |
| Deletion ownership | Phase 6A added no readiness-only diagnostics. The only Phase 6A-owned durable artifact is this evidence document plus its README entry. Existing Phase 4D parity fixture/extractor ownership remains with Phase 6B as already recorded in the Phase 4D artifact. |
| Rollback mechanism | Delete this evidence artifact and remove its README entry. No production code rollback is needed for Phase 6A because no production code changed. |
| Split triggers | If primitive, codec, traversal, producer, parity, resize projection, hyperlink authority, or lookup-cache evidence fails, send the failure back to Phase 4A, 4B, 4C, 3, 4D, 5A, 5B, or 5C respectively. If cutover mechanics are needed, split to Phase 6B instead of expanding Phase 6A. |
