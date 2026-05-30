# Flat Ring History Phase 4C Evidence: Traversal And Checkpoint Rebuild

## Checkpoint Decision

Phase 4C introduces no checkpoint records. The selected implementation adds a
row traversal layer and an in-memory row directory cache rebuilt from live row
records. Checkpoints remain optional future accelerators; row records remain
the only source of row existence.

## Phase 4C Scope

Implemented traversal contract:

1. Forward traversal advances by the validated live record byte length.
2. Backward traversal follows each row record's previous-row byte sequence and
   row sequence.
3. The in-memory row directory is disposable and rebuilds from live row records.
4. Directory hits and external cached handles validate the live row record
   identity before returning materialized content.

Non-goals preserved:

1. No production retained-line lookup migration.
2. No authoritative storage cutover.
3. No selection, viewport, or public projection policy change.
4. No materialization parity harness.
5. No row codec format change beyond traversal integration.
6. No performance tuning.

## Deletion And Orphan Audit

No authoritative checkpoint assumption, direct-index authority, retained-line
lookup migration, production storage mirror, fallback path, or replacement row
codec was introduced. The row directory is cache-only and can be discarded
without removing ring content.

Phase 5C remains the owner for production retained-line lookup cache migration.
Phase 6B remains the owner for authoritative ring storage cutover. A future
checkpoint phase must split checkpoint writing from cache rebuild if checkpoint
payloads grow beyond narrow landmark acceleration.

## Phase Gate

| Gate | Phase 4C outcome |
|---|---|
| Scope | Added isolated row traversal in `terminal_history_row_traversal` plus a focused traversal test target. No screen-model retained-history caller uses the traversal layer as production authority. |
| Behavior axis | Forward traversal reads the next row at `byte_sequence + record_bytes` and rejects non-monotonic row sequences in physical order. Backward traversal reads the previous row using the decoded previous-row byte sequence and row sequence. Directory cache hits and external cached handles validate epoch, byte sequence, row sequence, record size, and content generation through the row codec before returning content. |
| Recovery state | Recovery behavior is unchanged. Recovered-row provenance remains represented by the Phase 4B codec but Phase 4C does not change recovery acceptance or append policy. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S . -B build && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec vnm_terminal_history_row_traversal --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec|row_traversal)$"" --output-on-failure"` passed on 2026-05-30. |
| Baseline outcome | Focused primitive and codec coverage stayed green. Traversal coverage passed for forward and backward traversal across physical wrap, explicit directory drop/rebuild without content loss, stale missing directory rebuild after live bounds change, cached-handle identity validation, mismatched cached handles failing closed, and non-monotonic physical row-sequence chains failing closed. |
| Exit predicate | Phase 4C closes when traversal remains isolated behind `include/vnm_terminal/internal/terminal_history_row_traversal.h`, no production retained-line lookup or storage authority path depends on it, checkpoints are not treated as row existence, and the focused primitive/codec/traversal gate passes through the MSVC x64 environment. |
| Deletion ownership | No Phase 4C orphan was introduced. Phase 5C owns retained-line lookup migration and any deletion it orphans. Phase 6B owns authoritative storage cutover cleanup. A future checkpoint-writing phase owns deleting or narrowing any checkpoint writer that stops being a cache accelerator. |
| Rollback mechanism | Remove `terminal_history_row_traversal.h`, `terminal_history_row_traversal.cpp`, `tests/history_row_record_codec/history_row_traversal_tests.cpp`, this evidence artifact, the README entry, and the CMake target/source entries. |
| Split triggers | If checkpoint format grows beyond narrow landmarks, split checkpoint writing from cache rebuild. If production lookup migration, authoritative storage cutover, materialization parity, row codec format changes, selection/viewport/public projection policy, or performance tuning becomes necessary, stop and move that work to its owning phase. |
