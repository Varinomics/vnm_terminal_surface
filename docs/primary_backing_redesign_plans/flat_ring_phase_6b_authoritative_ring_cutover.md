# Flat Ring History Phase 6B Evidence: Authoritative Ring Cutover

## Scope

Phase 6B makes ring row records the authoritative retained-history storage for
primary scrollback.

Implemented scope:

1. Primary retained rows append through the shared producer into the row-record
   codec and flat history ring.
2. The old retained row deque is deleted; the screen model keeps only a
   rebuildable ordinal handle cache over live ring rows.
3. Retained row materialization returns owned decoded values from the ring.
4. Explicit scrollback eviction advances ring live bounds by record boundary.
5. The Phase 4D current-storage parity target and test-only retained-row record
   extractor are deleted.

Out of scope:

1. No feature behavior changes.
2. No row encoding or performance-format change.
3. No deque fallback, storage mirror, dual write, or recovery heuristic change.
4. No selection, public projection, viewport, or resize policy change.

## Phase Gate Table

| Gate | Phase 6B result |
| --- | --- |
| Scope | Ring row records are now the retained-history authority; deque row storage and the Phase 4D parity mirror fixture are removed. |
| Behavior axis | Existing retained render, selection, resize projection, hyperlink metadata, lookup-cache, and backend/session behavior stay on the prior policy axes while materializing retained rows from ring records. |
| Recovery state | Accepted recovery rows still call `append_scrollback_row`, which now seals once and commits through the ring codec. No recovery acceptance heuristic changed. |
| Evidence | Focused `vcvarsall` x64 storage/backend gate passed; static audits found no production/test retained deque, retained-row pointer API, Phase 4D fixture, scan-only lookup, or storage alias/fallback remnants. |
| Baseline outcome | Prior Phase 6A readiness evidence was green; this phase consumes that prerequisite by switching production authority. |
| Exit predicate | No retained row deque storage, retained-row pointer API, Phase 4D bounded mirror fixture, fallback, storage switch alias, or hidden dual storage remains; focused storage/backend gates pass. |
| Deletion ownership | Phase 6B owns deletion of retained deque storage, retained-row pointer APIs, orphaned helpers, and Phase 4D parity fixture/test target. |
| Rollback mechanism | Revert this Phase 6B change set, including `terminal_screen_model`, ring discard support, test/CMake cleanup, this evidence artifact, and the README entry. |
| Split triggers | Stop and split if cutover requires selection/public projection/viewport policy changes, recovery heuristic redesign, row encoding changes, a deque fallback, or Phase 8 optimization work. |

## Cutover Design

The primary backing buffer now owns a flat history ring plus traversal cache.
Appending retained history converts the already sealed producer value into a
`Terminal_history_row_record` and commits it with
`encode_terminal_history_row_record_to_ring`. The only retained ordinal state in
the screen model is a vector of live `terminal_history_handle_t` values; content
is decoded from the ring on demand.

Explicit scrollback eviction uses `Terminal_history_ring::discard_oldest_records`
to advance the live byte lower bound on record boundaries. Natural byte
reclamation from ring capacity prunes stale ordinal handles after commit.

## Deletion And Orphan Audit

Deleted in this phase:

1. Retained row deque storage in `Terminal_screen_model::Primary_backing_buffer`.
2. `Primary_backing_buffer::retained_history_row`.
3. `Primary_backing_buffer::append_retained_history_row`.
4. `Primary_backing_buffer::take_oldest_retained_history_row`.
5. `terminal_retained_row_cell_for_testing_t`.
6. `terminal_retained_row_record_for_testing_t`.
7. `Terminal_screen_model::retained_row_record_for_testing`.
8. `vnm_terminal_history_row_materialization_parity`.
9. `tests/history_row_record_codec/history_row_materialization_parity_tests.cpp`.

No permanent mirror, fallback, `_v2`, `_legacy`, or hidden dual storage path is
introduced.

## Evidence

Focused gate command:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_screen_operations vnm_terminal_backend_session vnm_terminal_render_snapshot vnm_terminal_viewport vnm_terminal_terminal_modes --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec|row_traversal)$|^vnm_terminal_(screen_operations|backend_session|render_snapshot|viewport|terminal_modes)$"" --output-on-failure"
```

Final gate output summary:

1. `vnm_terminal_history_ring` passed.
2. `vnm_terminal_history_row_record_codec` passed.
3. `vnm_terminal_history_row_traversal` passed.
4. `vnm_terminal_render_snapshot` passed.
5. `vnm_terminal_screen_operations` passed.
6. `vnm_terminal_terminal_modes` passed.
7. `vnm_terminal_viewport` passed.
8. `vnm_terminal_backend_session` passed.
9. `100% tests passed, 0 tests failed out of 8`.

Validation iterations before the final green gate:

1. The first build attempt failed because the new `unique_ptr` ring owner made
   `Terminal_screen_model` copy assignment implicitly deleted. Phase 6B fixed
   this by adding deep-copy semantics that decode source ring records and
   re-encode them into independent ring storage.
2. The second gate attempt built successfully, but
   `vnm_terminal_backend_session` failed one retained-identity compatibility
   status expectation. Phase 6B tightened compatibility-handle comparison so
   deliberate retained-identity record-size probes still report
   `RECORD_SIZE_MISMATCH`, while actual ring handles remain strict.

Static audit command:

```powershell
function Invoke-Audit($name, $pattern, [string[]]$paths) {
    Write-Output "---AUDIT: $name---"
    $result = & rg -n -S $pattern @paths -g '*.h' -g '*.cpp' -g 'CMakeLists.txt' 2>$null
    if ($LASTEXITCODE -eq 0) { $result }
    elseif ($LASTEXITCODE -eq 1) { Write-Output '(no matches)' }
    else { Write-Output "(rg failed with exit $LASTEXITCODE)" }
}

Invoke-Audit `
    'deleted retained deque storage and Phase 4D fixture symbols' `
    '\b(scrollback_row_t|retained_history_row|append_retained_history_row|take_oldest_retained_history_row|retained_row_record_for_testing|terminal_retained_row_record_for_testing_t|terminal_retained_row_cell_for_testing_t|vnm_terminal_history_row_materialization_parity|history_row_materialization_parity_tests)\b|std::deque<retained_row_record_t>|retained_history\.push_back|retained_history\.pop_front|retained_history\.front' `
    @('include', 'src', 'tests', 'CMakeLists.txt')

Invoke-Audit `
    'pointer-returning retained-row API names' `
    '\b(for_each_retained_history_row|retained_history_row_at|retained_row_at|retained_line_at|retained_row_ptr|retained_history_row_ptr|retained_row_pointer|retained_history_row_pointer|scrollback_row_at|scrollback_row_ptr)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'retained deque storage in screen model' `
    '<deque>|std::deque|deque<|retained_history\.clear\(|retained_history\.empty\(|retained_history\.size\(' `
    @('include/vnm_terminal/internal/terminal_screen_model.h', 'src/terminal_screen_model.cpp')

Invoke-Audit `
    'scan-only retained lookup markers' `
    '\b(scan_retained|retained_scan|scan_only|find_retained_line|find_retained_row|retained_line_descriptor_scan|selection_descriptor_lookup)\b' `
    @('include', 'src', 'tests')

Invoke-Audit `
    'governance storage alias/fallback spellings in terminal storage/backend scope' `
    '(?i)\b\w+(_v2|_legacy)\b|\b(retained|scrollback|storage|ring|deque)[-_ ]?(legacy|v2|fallback|mirror|switch)\b|\b(legacy|v2|fallback|mirror|switch)[-_ ]?(retained|scrollback|storage|ring|deque)\b|\bdual[-_ ]?write\b|\bdualwrite\b' `
    @('include/vnm_terminal/internal/terminal_screen_model.h', 'src/terminal_screen_model.cpp', 'include/vnm_terminal/internal/terminal_history_ring.h', 'src/terminal_history_ring.cpp', 'include/vnm_terminal/internal/terminal_history_row_record_codec.h', 'src/terminal_history_row_record_codec.cpp', 'include/vnm_terminal/internal/terminal_history_row_traversal.h', 'src/terminal_history_row_traversal.cpp', 'tests/history_ring', 'tests/history_row_record_codec', 'tests/screen_operations', 'tests/backend_session', 'tests/CMakeLists.txt')
```

Static audit result:

1. Deleted retained deque storage and Phase 4D fixture symbols: no matches in
   `include`, `src`, `tests`, or `tests/CMakeLists.txt`.
2. Pointer-returning retained-row API names: no matches.
3. Retained deque storage in `Terminal_screen_model`: no matches.
4. Scan-only retained lookup markers: no matches.
5. Governance storage alias/fallback spellings in terminal storage/backend
   scope: no matches.

A broader repository alias sweep also matched three unrelated surface-host test
labels containing `legacy mouse wheel`; those are input-mode labels, not
retained storage aliases.
