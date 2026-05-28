# Immediate public scrolling Phase 7 evaluation report

Date: 2026-05-28

Repos evaluated:

- `C:\plms\varinomics\vnm_terminal_surface`
- `C:\plms\varinomics\vnm_terminal`

## Decision

Default policy remains `DEFER_UNTIL_CONTENT_PUBLICATION`.

`IMMEDIATE_PUBLIC_PROJECTION` remains opt-in through the existing runtime/app
flag. The focused benchmark is clean for text/style/hyperlink/cursor/mode
hidden-state leakage and bounded wheel publication, but the evidence is not
strong enough to justify a default change: DECSET entry/projection capture is a
visible opt-in risk at 70.1585 ms median, mostly 68.8143-71.0501 ms with a
76.5040 ms max outlier, the 10000-row
copied projection had a median RSS delta of 54255616 bytes, total process RSS
continued to grow during wheel scrolling, manual GUI validation was documented
but not executed interactively in this run, and the existing manual transcript
corpus contains stale/divergent captures.

Changing the default still requires a separate reviewed proposal with measured
numbers, trace artifacts, and explicit approval.

## Added evaluation tooling

- `vnm_terminal_phase7_public_scroll_benchmark`
- `vnm_terminal_phase7_public_scroll_benchmark_validate` CTest. This is kept as
  a structural readiness smoke for benchmark invariants; it has no performance
  threshold and does not imply default-policy readiness.
- Optional transcript replay corpus runner:
  `tools/transcript_replay/run_transcript_corpus.cmake`
- Optional CTest hook controlled by
  `VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR`

The benchmark measures:

- synchronized-output DECSET entry and public projection capture cost, excluding
  hidden suffix parsing after the entry boundary;
- copied public rows, copied cells, and best-effort process RSS delta;
- high-frequency public scroll during a hold;
- public-scroll snapshot generation count during wheel input;
- release processing through release-snapshot observation, reconciliation result;
- hidden text, style, hyperlink, cursor, and mode sentinels during the hold.

## Build command

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake -S C:\plms\varinomics\vnm_terminal_surface -B C:\plms\varinomics\vnm_terminal_surface\build_phase7 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 -DVNM_TERMINAL_SURFACE_BUILD_TESTING=ON -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON && cmake --build C:\plms\varinomics\vnm_terminal_surface\build_phase7 --target vnm_terminal_phase7_public_scroll_benchmark vnm_terminal_backend_session vnm_terminal_transcript vnm_terminal_transcript_replay"
```

Result: build passed.

## Benchmark command and results

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build C:\plms\varinomics\vnm_terminal_surface\build_phase7 --target vnm_terminal_phase7_public_scroll_benchmark && set PATH=C:\Qt\6.10.1\msvc2022_64\bin;%PATH%&& C:\plms\varinomics\vnm_terminal_surface\build_phase7\benchmarks\phase7_public_scroll\vnm_terminal_phase7_public_scroll_benchmark.exe --iterations 7 --warmup 2 --scrollback-limit 10000 --rows 24 --columns 80 --wheel-events 240 --validate-json --quiet --output C:\plms\varinomics\vnm_terminal_surface\docs\immediate_public_scroll_phase7_benchmark_results.json && ctest --test-dir C:\plms\varinomics\vnm_terminal_surface\build_phase7 -R ""vnm_terminal_phase7_public_scroll_benchmark_validate"" --output-on-failure"
```

Result: benchmark passed and CTest validation passed.

Generated JSON report:
`docs/immediate_public_scroll_phase7_benchmark_results.json`

Summary:

- Entry/capture: median 70158500 ns, p95 71050100 ns, max 76504000 ns.
- Wheel during hold: 240 moved wheel events, 240 public scroll snapshots, 240 render generations.
- Wheel total: median 610677400 ns for 240 events.
- Wheel per event: median 2544489 ns, p95 2554291 ns, max 2558830 ns.
- Projection rows: 10024 stored rows for 10000 scrollback rows plus 24 visible rows.
- Projection cells: 791897 copied cells.
- Projection capture batches: 418 row-capture snapshots.
- RSS projection delta: median 54255616 bytes, p95 54493184 bytes, max 54603776 bytes.
- Post-wheel process RSS peak: 160968704 bytes. This is separate from the
  projection RSS delta above; it includes total working-set growth observed
  during 240 public scroll publications and allocator retention.
- Post-wheel RSS delta from post-projection RSS: median 47960064 bytes, p95
  48058368 bytes, max 48316416 bytes.
- Release processing through snapshot observation: median 8695400 ns, p95
  9531200 ns, max 10069000 ns.
- Release result: `exact_anchor`.
- Release snapshot basis/purpose: `LIVE_CONTENT` / `CONTENT`.
- Release offset from tail: 313, matching the deterministic expected offset of
  240 public wheel steps plus 73 hidden hold scroll-growth rows.
- Hidden text/style/hyperlink/cursor/mode leak during hold: false in all
  measured attempts.

Interpretation:

- Projection memory is structurally bounded to one copied projection in the
  measured case: 10024 copied rows for 10000 scrollback rows plus 24 active-grid
  rows, with at most 801920 row/cell slots at 80 columns. The measured 791897
  copied cells and 54255616-byte median RSS projection delta frame the current
  comparison point at about 68.5 RSS-delta bytes per copied cell, acknowledging
  that RSS includes allocator and runtime effects.
- Hidden-leak checks are no longer only projection self-comparisons. The
  benchmark compares projection and public-scroll snapshot text, style/color,
  hyperlink URI/identity metadata, cursor shape/visibility/mapped position, and
  mode state against the safe pre-hold content basis where that signal is
  available.
- The post-wheel RSS peak is a separate process working-set signal, not copied
  projection storage. It must be tracked independently in future renderer or
  event-loop benchmarks.
- High-frequency public scroll is linear in wheel events and did not generate
  more than one public scroll snapshot per moved wheel event.
- Release reconciliation now requires both `exact_anchor` and the expected
  release offset/basis fields; `exact_anchor` alone is not treated as sufficient
  readiness evidence.
- The measured headless wheel cost is acceptable for opt-in evaluation, but it
  is not enough by itself to change the default without renderer/manual evidence.

## Focused tests

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && ctest --test-dir C:\plms\varinomics\vnm_terminal_surface\build_phase7 -R ""vnm_terminal_backend_session|vnm_terminal_transcript"" --output-on-failure"
```

Result: 2/2 passed. Together with the benchmark CTest validation above, the
focused Phase 7 validation set was 3/3 passed.

- `vnm_terminal_phase7_public_scroll_benchmark_validate`
- `vnm_terminal_transcript`
- `vnm_terminal_backend_session`

The benchmark validate CTest is a permanent structural smoke for this Phase 7
tooling unless a future benchmark replacement removes it. It verifies JSON shape
and deterministic invariants only; it is not a performance-threshold test and
does not imply default readiness.

Terminal app policy tests from the Phase 7 evaluation baseline were not rerun in
this hardening pass because the Phase 6 app work was intentionally left
untouched. The recorded command was:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && ctest --test-dir C:\plms\varinomics\vnm_terminal\build -R ""synchronized_output_scroll_policy"" --output-on-failure"
```

Recorded baseline result: 4/4 passed.

- `vnm_terminal_rejects_bad_synchronized_output_scroll_policy`
- `vnm_terminal_rejects_empty_synchronized_output_scroll_policy`
- `vnm_terminal_help_mentions_synchronized_output_scroll_policy`
- `vnm_terminal_help_describes_synchronized_output_scroll_policy_values`

## Transcript replay and corpus coverage

The transcript unit/replay test target passed and covers generated old-schema
defaults plus immediate public-projection replay cases, including natural public
projection scroll snapshots and rejection of unmatched/corrupt public projection
scroll snapshots.

Manual corpus command recorded during the Phase 7 evaluation baseline:

```bat
cmd.exe /c "set PATH=C:\Qt\6.10.1\msvc2022_64\bin;%PATH%&& cmake -Dreplay_executable=C:\plms\varinomics\vnm_terminal_surface\build_phase7\vnm_terminal_transcript_replay.exe -Dcorpus_dir=C:\plms\varinomics\vnm_terminal_surface\manual_validation_runs -P C:\plms\varinomics\vnm_terminal_surface\tools\transcript_replay\run_transcript_corpus.cmake"
```

Latest hardening rerun result: failed for 8 of 12 existing manual transcripts.
This is an explicit corpus-quality finding, not a pass condition. The failures
are replay diagnostic divergences in older/manual captures, while the new
benchmark did not find hidden-content leakage. The existing
`manual_validation_runs` corpus is stale and unvetted for default-policy
evidence; it must be regenerated or curated before any default-change proposal,
and it should not be wired as a default CTest gate until that curation is
complete.

The corpus runner now prints per-file stderr and stdout diagnostic summaries for
failing transcripts, preserving first divergent snapshot fields and dirty-range
mismatch fields instead of only reporting the aggregate failure count.

Per-file manual transcript replay status:

- `codex_repro_20260526_001617.ndjson`: exit 3; recorded snapshot diagnostics
  diverged; first divergent fields were release/public viewport fields,
  row provenance, and viewport; first dirty mismatch was dirty row range shape.
- `codex_repro_20260526_013331.ndjson`: exit 3; recorded snapshot diagnostics
  diverged; first divergent field was row provenance; first dirty mismatch was
  dirty row range shape.
- `codex_repro_20260526_020421.ndjson`: exit 3; recorded snapshot diagnostics
  diverged; first divergent field was row provenance; first dirty mismatch was
  dirty row range shape.
- `codex_repro_20260526_020536.ndjson`: exit 3; recorded snapshot diagnostics
  diverged; first divergent field was snapshot sequence; first dirty mismatch
  was dirty row range shape.
- `wheel_active_output_trace_20260527_090427.ndjson`: exit 3; recorded snapshot
  diagnostics diverged; first divergent field was snapshot sequence; first
  dirty mismatch was dirty row range shape.
- `wheel_consolidated_codex_20260526_223616.ndjson`: exit 3; recorded snapshot
  diagnostics diverged; first divergent fields were cursor, release/public
  viewport fields, snapshot sequence, and viewport; first dirty mismatch was
  dirty row range shape.
- `wheel_indicator_codex_20260526_213225.ndjson`: exit 3; recorded snapshot
  diagnostics diverged; first divergent fields were cell count, cursor,
  release/public viewport fields, row provenance, snapshot sequence, viewport,
  and visible rows; first dirty mismatch was dirty row range shape.
- `wheel_sync_scroll_fix_validation_20260527_095133.ndjson`: exit 0
- `wheel_timing_codex_20260526_232911.ndjson`: exit 0
- `wheel_timing_codex_20260526_233159.ndjson`: exit 0
- `wheel_timing_no_snapshots_codex_20260526_235432.ndjson`: exit 0
- `wheel_trace_codex_20260526_202658.ndjson`: exit 3; recorded snapshot
  diagnostics diverged; first divergent field was snapshot sequence; first
  dirty mismatch was dirty row range shape.

Passing-subset corpus command recorded during the Phase 7 evaluation baseline:

```powershell
$corpus='C:\plms\varinomics\vnm_terminal_surface\build_phase7\phase7_manual_replay_passing_corpus'
New-Item -ItemType Directory -Force -Path $corpus | Out-Null
'wheel_sync_scroll_fix_validation_20260527_095133.ndjson','wheel_timing_codex_20260526_232911.ndjson','wheel_timing_codex_20260526_233159.ndjson','wheel_timing_no_snapshots_codex_20260526_235432.ndjson' | ForEach-Object { Copy-Item -LiteralPath (Join-Path 'C:\plms\varinomics\vnm_terminal_surface\manual_validation_runs' $_) -Destination (Join-Path $corpus $_) -Force }
$env:PATH='C:\Qt\6.10.1\msvc2022_64\bin;' + $env:PATH
cmake -Dreplay_executable=C:\plms\varinomics\vnm_terminal_surface\build_phase7\vnm_terminal_transcript_replay.exe "-Dcorpus_dir=$corpus" -P C:\plms\varinomics\vnm_terminal_surface\tools\transcript_replay\run_transcript_corpus.cmake
```

Recorded baseline result: passed for 4 transcript(s).

## Manual validation checklist

Status: documented only. The interactive GUI/manual matrix below was not
executed in this run and is not counted as completed evidence.

The Phase 6 repro script is:
`C:\plms\varinomics\vnm_terminal\tools\synchronized_output_scroll_policy_repro.ps1`

The GUI interaction is not fully automatable because the user must scroll during
the synchronized-output hold and inspect visible content. Use these exact manual
commands against a deployed `vnm_terminal.exe`.

Immediate opt-in run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\plms\varinomics\vnm_terminal\tools\synchronized_output_scroll_policy_repro.ps1 -TerminalExe C:\plms\varinomics\vnm_terminal\build\Release\vnm_terminal.exe -Policy immediate-public -CaptureTranscript C:\plms\varinomics\vnm_terminal_surface\manual_validation_runs\phase7_manual_immediate.ndjson -WheelTrace -TranscriptSnapshotDiagnostics -PublicRows 160 -HoldSeconds 12
```

Expected immediate checks:

- During the hold, wheel input visibly scrolls public-prefix rows immediately.
- During the hold, no `HIDDEN-*` sentinel text, style, hyperlink, cursor, or mode
  state is visible before release.
- The scrollbar range and thumb movement reflect the public projection, not the
  hidden live buffer.
- Release keeps the intended public anchor or sticky-tail position without a
  multi-second stall.
- After release, hidden content may publish normally and
  `public-suffix-after-release` appears after `DECRST 2026`.

Deferred default run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\plms\varinomics\vnm_terminal\tools\synchronized_output_scroll_policy_repro.ps1 -TerminalExe C:\plms\varinomics\vnm_terminal\build\Release\vnm_terminal.exe -Policy defer -CaptureTranscript C:\plms\varinomics\vnm_terminal_surface\manual_validation_runs\phase7_manual_defer.ndjson -WheelTrace -TranscriptSnapshotDiagnostics -PublicRows 160 -HoldSeconds 12
```

Expected deferred checks:

- During the hold, wheel input remains visually deferred.
- Hidden sentinel content remains invisible until release.
- Release publishes accumulated content and applies the deferred public scroll
  intent according to the deferred policy.

## Residual risks and follow-ups

- The focused benchmark is headless. It does not include scene graph rendering,
  user input dispatch, or real GUI event-loop contention.
- Process RSS is a coarse memory signal and includes allocator retention, but
  copied-row counts show the projection bound directly.
- The existing manual transcript directory needs curation or regeneration; 8 of
  12 captures diverge under current replay diagnostics and cannot support a
  default-change proposal until refreshed.
- The manual validation checklist is documented but was not executed
  interactively in this run.
- A higher-rate renderer-level wheel benchmark would be useful before any
  default-change proposal.
- Default change remains blocked on stronger renderer/manual/corpus evidence, a
  separate reviewed proposal, and explicit approval.
