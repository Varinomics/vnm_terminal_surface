# Flat Ring History Phase 4B Evidence: Row Record Codec

## Encoding Choice

Phase 4B selects one dense row encoding. Each retained row record stores every
cell in column order with its UTF-8 text cluster, display width,
wide-continuation flag, occupied flag, session-lifetime style id, and row-local
hyperlink id. Row-local hyperlink identity keys are encoded once after the cell
stream.

No run-length, alternate compact, `_v2`, `_legacy`, fallback, production mirror,
or second encoder path is introduced. Encoding tightening remains Phase 8 work
and requires measurements before changing this representation.

## Phase 4B Scope

Implemented codec contract:

1. Row records encode/decode canonical content, wide cells, text clusters,
   row-local hyperlinks, session-lifetime style ids, provenance, source width,
   hard-boundary wrap metadata, content generation, row sequence, epoch, and
   previous-row traversal metadata.
2. The codec writes row payloads into the Phase 4A ring primitive framing and
   validates the ring-framed byte sequence and record byte count on decode.
3. Decode accepts only a bounded ring read scope or a caller-owned payload view
   and returns owned materialized row values.
4. Records remain self-contained under the Phase 3
   `SESSION_LIFETIME_STYLE_ID` policy.

Non-goals preserved:

1. No production retained-history storage authority.
2. No traversal directory replacement.
3. No checkpoint rebuild.
4. No materialization parity harness.
5. No authoritative cutover.
6. No selection, viewport, or public projection policy change.
7. No performance tuning beyond avoiding duplicate text conversion inside one
   encode operation.

## Deletion And Orphan Audit

No obsolete test encoders, temporary format helpers, dense/run alternatives,
style adapters, production dual paths, hidden storage mirrors, or fallback
decoders remain from this phase. No temporary style adapter was introduced, so
there is no style-adapter deletion owner.

Phase 6B remains the owner for deleting deque-backed retained storage at
authoritative cutover. Phase 8 owns any measured encoding replacement and must
delete this dense encoding in the same batch if it stops being the selected
representation.

## Phase Gate

| Gate | Phase 4B outcome |
|---|---|
| Scope | Added the isolated internal row-record codec in `terminal_history_row_record_codec` plus a dedicated codec test target. The codec can write test records into the Phase 4A primitive and decode from bounded read scopes, but no screen-model production retained-history path calls it. |
| Behavior axis | Dense row payloads preserve blank and occupied cells, wide spans and continuations, UTF-8 text clusters, session-lifetime style ids, row-local hyperlink identity keys, terminal-storage or recovered provenance, source width, hard-boundary wrap metadata, row sequence, epoch, content generation, and previous-row metadata. Header, footer, ring byte-sequence, ring record-size, and expected-handle validation fail closed. |
| Recovery state | Recovery behavior is unchanged. The codec represents `RECOVERED_PRIMARY_REPAINT` provenance for accepted recovery rows, but recovery acceptance and production append policy remain in the Phase 3/current-storage path. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S . -B build && cmake --build build --target vnm_terminal_history_ring vnm_terminal_history_row_record_codec --config Debug && ctest --test-dir build -C Debug -R ""^vnm_terminal_history_(ring|row_record_codec)$"" --output-on-failure"` passed on 2026-05-30. |
| Baseline outcome | Focused primitive coverage stayed green. Focused codec coverage passed for dense and blank rows, wide spans and continuations, combining/cluster text, styles, row-local hyperlinks, recovered provenance, source width and wrap metadata, header/footer validation failures, ring-framed record-size validation, expected content-generation validation, and owned materialization after read-scope end and ring eviction. |
| Exit predicate | Phase 4B closes when the codec remains isolated behind `include/vnm_terminal/internal/terminal_history_row_record_codec.h`, no production retained-history caller depends on it as authority, records are self-contained under session-lifetime style ids, decode returns owned values rather than ring pointers/views, and the focused codec/primitive target passes through the MSVC x64 environment. |
| Deletion ownership | No phase-local temporary encoder or style adapter remains. Phase 8 owns deleting/replacing the dense encoding if measured tuning selects a different single representation. Phase 6B owns deletion of deque storage at authoritative cutover. |
| Rollback mechanism | Remove `terminal_history_row_record_codec.h`, `terminal_history_row_record_codec.cpp`, `tests/history_row_record_codec/history_row_record_codec_tests.cpp`, this evidence artifact, the README entry, and the CMake target/source entries. |
| Split triggers | If row records exceed the one-octile commit window for realistic retained rows, split row-size-limit or chunking policy into a reviewed plan amendment. If row-local styles or a versioned append-only style catalog replace session-lifetime style ids, split the style policy change before altering the codec. If traversal directories, checkpoints, parity harnesses, production cutover, or projection policies become necessary, stop and move that work to Phases 4C, 4D, 6B, or the owning projection phase. |
