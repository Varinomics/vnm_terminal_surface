# Flat Ring History Phase 8 Evidence: Encoding, Performance, And Memory Tightening

## Scope

Phase 8 measures the authoritative flat-ring retained-history representation
after the semantic migration and recovery verification have closed.

Implemented scope:

1. Add a focused benchmark target for append, materialization, traversal, resize
   projection, selection extraction, cache rebuild, and hyperlink-heavy output.
2. Add a codec oversize regression proving row-record oversize hard-fails before
   publication.
3. Document retained-history ring size, row-size formula, and hard-fail limits.
4. Record the representation decision and benchmark artifact.

Out of scope:

1. No semantic storage model change.
2. No fallback to deque or any old representation.
3. No production compact encoder, chunking, mirror, `_v2`, or `_legacy` path.
4. No unrelated selection, viewport, public projection, or recovery behavior
   change.

## Representation Decision

Phase 8 keeps the current dense row-record encoding as the only production
representation.

The benchmark target measures the dense implementation directly and includes a
benchmark-only byte projection for a possible run-length representation. That
projection is not an encoder, is not linked from production, and cannot decode
or publish records. It is present only to make the dense-versus-compact decision
auditable without adding a second format path.

Dense remains selected because the measured storage and operation costs fit the
current ring budget, the current screen-model row limits are far below the
one-octile record window for ordinary rows, and a compact format would add a
second production representation without a Phase 8 measurement forcing it. If a
future workload proves ring pressure or row-size pressure, that is a format
phase with a reviewed single replacement encoding, not a Phase 8 fallback.

## Ring And Row Limits

The authoritative screen-model retained-history ring is configured at
64 MiB. The ring backend enforces the Sintra-lineage one-octile commit window,
so a single retained-history record may consume at most one eighth of the
aligned ring capacity.

For the current 64 MiB configuration:

1. Ring capacity: 67,108,864 bytes.
2. Maximum ring record: 8,388,608 bytes.
3. Ring framing overhead: 40 bytes.
4. Maximum row-codec payload: 8,388,568 bytes.

The dense row-codec payload size is:

```text
108 row-header bytes
+ 32 row-footer bytes
+ sum(cells, 24 cell-header bytes + UTF-8 text bytes)
+ sum(row-local hyperlinks, 12 hyperlink-header bytes + identity-key bytes)
```

The ring record size is the row-codec payload plus the 40-byte ring framing
overhead.

The screen model accepts at most 4,096 rows, 4,096 columns, and 1,048,576 active
cells. A retained row produced by the screen model therefore has at most 4,096
cells under the current geometry limits. Pathological cell text or row-local
hyperlink identity data can still exceed the row payload window; that condition
hard-fails explicitly. There is no silent truncation, no old-storage fallback,
and no unreviewed chunking.

Oversize behavior:

1. `Terminal_history_ring::reserve_record` returns
   `Terminal_history_ring_status::OVERSIZE_RECORD`.
2. `encode_terminal_history_row_record_to_ring` reports
   `Terminal_history_row_record_codec_status::RING_RESERVE_FAILED` and carries
   the ring oversize status.
3. No partial row record is published and the ring head does not advance.
4. Production storage treats failed publication as a retained-history storage
   failure rather than degrading to another representation.

## Benchmark Artifact

The benchmark/report artifact is:

1. `flat_ring_phase_8_benchmark_report.json`

The CTest target runs the benchmark without a report output path. The JSON
report artifact was generated with this direct invocation:

```cmd
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && build\tests\vnm_terminal_history_phase8_benchmark.exe docs\primary_backing_redesign_plans\flat_ring_phase_8_benchmark_report.json"
```

The benchmark target is:

1. `vnm_terminal_history_phase8_benchmark`

Measured axes:

| Axis | Benchmark case |
| --- | --- |
| Append | `append_dense_rows` |
| Materialization | `materialization_dense_rows` |
| Traversal | `traversal_forward_rows` |
| Resize projection | `resize_projection_retained_rows` |
| Selection extraction | `selection_extraction_retained_rows` |
| Cache rebuild | `cache_rebuild_row_directory` |
| Hyperlink-heavy output | `hyperlink_heavy_output_and_snapshot` |

## Measurement Summary

Generated on 2026-05-30 through the MSVC x64 environment.

Ring limits:

1. Ring capacity: 67,108,864 bytes.
2. Maximum record bytes: 8,388,608.
3. Maximum payload bytes: 8,388,568.

Row-size projection:

| Scenario | Dense bytes | Benchmark-only run-length projection bytes |
| --- | ---: | ---: |
| `blank_120_columns` | 3,180 | 213 |
| `dense_ascii_120_columns` | 3,180 | 4,140 |
| `repeated_ascii_120_columns` | 3,180 | 213 |
| `hyperlink_120_columns` | 8,950 | 9,910 |

Benchmarks:

| Case | Operations | ns/op | Bytes or metadata count | Result |
| --- | ---: | ---: | ---: | --- |
| `append_dense_rows` | 2,000 | 16,138.60 | 6,360,000 | ok |
| `materialization_dense_rows` | 2,000 | 12,622.60 | 6,360,000 | ok |
| `traversal_forward_rows` | 2,000 | 24,514.15 | 6,360,000 | ok |
| `cache_rebuild_row_directory` | 20,000 | 11,882.41 | 63,600,000 | ok |
| `resize_projection_retained_rows` | 64 | 5,439,781.25 | 158,976 | ok |
| `selection_extraction_retained_rows` | 200 | 3,517,289.50 | 1,746,000 | ok |
| `hyperlink_heavy_output_and_snapshot` | 220 | 118,768.64 | 24 | ok |

## Phase Gate Table

| Gate | Phase 8 result |
| --- | --- |
| Scope | Adds benchmark/evidence coverage, one oversize regression, and limit documentation only. Production retained-history storage remains the authoritative flat ring with the existing dense row codec. |
| Behavior axis | No selection, viewport, resize, projection, recovery, or storage-semantics behavior changes. Resize remains projection, caches remain rebuildable, row records remain immutable, and oversize remains a hard failure. |
| Recovery state | Recovery behavior is unchanged from Phase 7. Recovered rows still append through the shared producer and dense row codec; Phase 8 does not change recovery acceptance or provenance. |
| Evidence | Focused MSVC x64 gate passed on 2026-05-30: `vnm_terminal_history_ring`, `vnm_terminal_history_row_record_codec`, `vnm_terminal_history_row_traversal`, `vnm_terminal_history_phase8_benchmark`, `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, `vnm_terminal_render_snapshot`, and `vnm_terminal_viewport`; `100% tests passed, 0 tests failed out of 8`. The benchmark command wrote `flat_ring_phase_8_benchmark_report.json`. |
| Baseline outcome | Dense encoding remains the baseline and selected representation. The benchmark-only compact byte projection does not justify adding a production compact format in this phase. |
| Exit predicate | Regression targets pass, the benchmark report is generated, row/ring limits are documented, oversize behavior is tested, and no production fallback, mirror, `_v2`, `_legacy`, chunking, or second encoder path exists. |
| Deletion ownership | Phase 8 introduces no production experimental encoding path. The benchmark-only run-length byte projection is evidence code in the Phase 8 benchmark target, not a retained production encoder. If a future format phase selects compact encoding, that phase owns replacing dense with one production representation and deleting the superseded format in the same batch. |
| Rollback mechanism | Remove `tests/history_row_record_codec/history_row_record_phase8_benchmark.cpp`, the `vnm_terminal_history_phase8_benchmark` CMake target/test entries, the Phase 8 oversize codec regression, this evidence artifact, the benchmark JSON report, and the README entries. No production rollback is required. |
| Split triggers | If measurements require chunking, split to a reviewed behavior/format phase. If a compact encoding is selected, split to a single-representation codec replacement phase. If performance work needs selection, viewport, public projection, recovery, or resize behavior changes, move that work to the owning behavior phase. |

## Deletion And Orphan Audit

Phase 8 intentionally leaves no production dense/compact dual path. The
reproducible production-path negative audit is scoped to the retained-history
ring, row codec, traversal, screen-model storage boundary, and focused storage
tests/CMake entries:

```powershell
rg -n -S "(?i)\b\w+(_v2|_legacy)\b|\b(retained|scrollback|storage|ring|row|codec|encoder)[-_ ]?(legacy|v2|fallback|mirror|switch)\b|\b(legacy|v2|fallback|mirror|switch)[-_ ]?(retained|scrollback|storage|ring|row|codec|encoder)\b|\bdual[-_ ]?write\b|\bdualwrite\b|\bcompact[-_ ]?encoder\b|\brun[-_ ]?length[-_ ]?encoder\b" include/vnm_terminal/internal/terminal_history_ring.h src/terminal_history_ring.cpp include/vnm_terminal/internal/terminal_history_row_record_codec.h src/terminal_history_row_record_codec.cpp include/vnm_terminal/internal/terminal_history_row_traversal.h src/terminal_history_row_traversal.cpp include/vnm_terminal/internal/terminal_screen_model.h src/terminal_screen_model.cpp tests/history_row_record_codec tests/history_ring tests/CMakeLists.txt
```

This command returned no matches on 2026-05-30.

A broader repository grep for `fallback`, `mirror`, `chunk`, `_legacy`, `_v2`,
and related words is not a clean negative audit because it intentionally finds
historical phase-plan prose, parser/backend chunk terminology, renderer
software-fallback terms, and test assertion messages such as the Phase 8
oversize message that says no fallback record is published. Those broader hits
are not production retained-history storage paths and are not used as the Phase
8 close predicate.
