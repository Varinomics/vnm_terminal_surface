# Flat Ring History Phase 4A Evidence: Flat Ring Primitive

## Backend Form Decision

Phase 4A selects a terminal-owned local non-IPC variant derived from Sintra's
direct ring concepts instead of using `sintra::Ring_W` / `sintra::Ring_R`
directly.

Concrete incompatibility: the direct Sintra helper is the correct lineage for
the ringbuffer mechanics, but its usable surface is tied to shared-memory IPC
files, control blocks, process/reader slots, semaphore wakeups, slow-reader
eviction, and attach/detach lifecycle. Those concepts are irrelevant to
terminal retained-history storage and must not leak into terminal history APIs
or tests outside the internal backend boundary.

Selected subset/variant:

1. In-process byte storage with absolute monotonic byte sequences.
2. Sintra-aligned capacity helper semantics: page/allocation alignment plus an
   octile-compatible capacity.
3. Sintra-lineage one-octile maximum write window, translated into an explicit
   `OVERSIZE_RECORD` status before publication.
4. Two-span physical wrap access instead of double virtual mapping.
5. Terminal-local reservation, commit, live-window, read-scope, and snapshot
   status vocabulary.

The implementation has no dependency on `sintra/sintra.h`, no high-level Sintra
IPC dependency, and no direct-Sintra adapter scaffold left behind.

## Phase 4A Scope

Implemented primitive contract:

1. Absolute byte sequence model with monotonic `oldest_live_byte_sequence` and
   `head_byte_sequence`.
2. Two-span physical wrap reads and writes; no padding records are emitted.
3. Primitive record framing with header, payload, and footer bytes.
4. Reservation/commit publication where uncommitted reservation bytes remain
   invisible until commit succeeds.
5. Tail advancement only by committed record boundaries.
6. Rebuildable live record-boundary cache from the live byte range.
7. Terminal-local backend snapshot failure translation.

Non-goals preserved:

1. No terminal row codec.
2. No production retained-history writes.
3. No production dual-write mirror.
4. No authoritative storage cutover.
5. No selection, viewport, public projection, checkpoint, or retained-row policy
   migration.

## Phase Gate

| Gate | Phase 4A outcome |
|---|---|
| Scope | Added the isolated internal flat byte-ring primitive in `terminal_history_ring` plus a dedicated primitive test target. Production retained-history append/read paths are not connected to the primitive. |
| Behavior axis | Records use absolute byte sequences; physical offsets are derived only inside the backend and never become identity. Writes reserve a full framed record, publish only on commit, hard-fail records beyond one octile, and advance tail by whole record boundaries. Physical wrap uses two-span copy/read, not padding records. |
| Recovery state | Recovery behavior is unchanged. Recovered rows still target current retained storage through the Phase 3 producer; the new primitive has no recovery policy entry point. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S . -B build && cmake --build build --target vnm_terminal_history_ring --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_ring$"" --output-on-failure"` passed on 2026-05-30. |
| Baseline outcome | Focused primitive coverage passed for capacity alignment and one-octile boundary behavior, oversize hard failure, partial-record invisibility, physical wrap traversal, tail advancement to record boundaries, rebuild after record-boundary cache drop from the live range, read-scope boundary rejection, and terminal-local snapshot status translation. |
| Exit predicate | Phase 4A closes when the primitive remains isolated behind `include/vnm_terminal/internal/terminal_history_ring.h`, no production retained-history caller writes to it, and the focused primitive target passes through the MSVC x64 environment. |
| Deletion ownership | No experimental primitive variant or direct-Sintra adapter scaffold remains. The local non-IPC variant is the selected backend form. Later Phase 4B owns row codec integration; Phase 4C owns traversal/checkpoint rebuild beyond primitive record-boundary cache needs. |
| Rollback mechanism | Remove `terminal_history_ring.h`, `terminal_history_ring.cpp`, `tests/history_ring/history_ring_tests.cpp`, this evidence artifact, the README entry, and the CMake target/source entries. |
| Split triggers | If double mapping becomes necessary for performance, split backend strategy from semantics. If one-octile record size is too small for retained rows, split row chunking or row-size-limit policy into a reviewed plan amendment. If reader eviction or IPC lifecycle becomes correctness-relevant, redesign the adapter boundary instead of importing those semantics. |
