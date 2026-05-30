# Flat Ring History Phase 4D Evidence: Ring Materialization Parity Harness

## Harness Boundary

Phase 4D adds an isolated parity harness that extracts already-sealed retained
producer records from current retained storage in tests, feeds those values into
the Phase 4B row codec and Phase 4C traversal layer, and compares the
materialized ring output with the current retained render output.

The harness boundary is explicit:

1. `retained_row_record_for_testing` is internal test support and exposes only
   primary retained rows that already exist in current storage.
2. The ring is allocated inside the parity test process.
3. No screen-model production append, recovery, selection, viewport, public
   projection, resize, or retained lookup path writes to or reads from the ring.
4. No production mirror, fallback, `_v2`, `_legacy`, or hidden dual-storage path
   is introduced.

## Phase 4D Scope

Implemented parity coverage:

1. Normal retained text rows.
2. Session-lifetime style ids.
3. Row-local hyperlink identity keys and current render hyperlink metadata.
4. Recovered repaint provenance.
5. Blank retained rows.
6. Wide spans and continuations.
7. Stale cached ring handles after live-window advancement.

Non-goals preserved:

1. No production dual writes.
2. No new public API.
3. No authoritative storage cutover.
4. No traversal directory replacement in production.
5. No selection, viewport, public projection, resize projection, or performance
   migration.

## Deletion And Orphan Audit

No production mirror state was added. The only new harness artifact is the
focused parity test plus the internal `for_testing` extractor used by that test.
Phase 6B owns deleting or narrowing the bounded parity fixture/extractor when
authoritative ring cutover removes the current retained-storage comparison
baseline.

No `_v2`, `_legacy`, fallback decoder, permanent dual-write path, retained ring
owner, production traversal lookup replacement, or resize/projection scaffold was
introduced.

## Phase Gate

| Gate | Phase 4D outcome |
|---|---|
| Scope | Added a bounded parity harness in `vnm_terminal_history_row_materialization_parity` plus an internal retained-row `for_testing` extractor. The harness reads current retained rows, encodes them into a test-local ring, materializes through traversal, and compares against current retained render output. |
| Behavior axis | Parity covers text, style ids, row-local hyperlinks, provenance, blank rows, wide spans, recovered rows, and stale handle validation. Ring identity uses test-local epoch and retained-line sequence values only inside the harness. |
| Recovery state | Recovery acceptance policy is unchanged. The recovery parity case uses the existing accepted primary repaint recovery path, then compares the recovered retained row after it has entered current storage through the shared producer. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S . -B build && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal vnm_terminal_history_row_materialization_parity vnm_terminal_screen_operations --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring\|row_record_codec\|row_traversal\|row_materialization_parity)$\|^vnm_terminal_screen_operations$"" --output-on-failure"` passed on 2026-05-30. |
| Baseline outcome | Existing ring, codec, traversal, and screen-operations producer coverage stayed green. The new parity target passed normal retained row, recovered row, and stale handle scenarios without production ring writes. |
| Exit predicate | Phase 4D closes when the parity harness remains test-only, current retained storage stays authoritative, producer values encode/decode through the row codec, materialized ring rows match current retained render output, and stale ring handles fail closed. |
| Deletion ownership | Phase 6B owns deleting or narrowing the bounded parity fixture/extractor when ring storage becomes authoritative and the current retained-storage comparison baseline is removed. |
| Rollback mechanism | Remove `retained_row_record_for_testing`, `terminal_retained_row_cell_for_testing_t`, `terminal_retained_row_record_for_testing_t`, `tests/history_row_record_codec/history_row_materialization_parity_tests.cpp`, the CMake target/list entries, this evidence artifact, and the README entry. |
| Split triggers | If parity requires production mirror state, stop and redesign as isolated fixtures. If parity needs production dual writes, new public APIs, storage cutover, traversal directory replacement, projection policy changes, resize migration, or performance work, move that work to its owning phase. |
