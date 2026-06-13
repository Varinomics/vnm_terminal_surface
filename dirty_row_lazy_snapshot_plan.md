# Dirty-Row Lazy Snapshot Refactor Plan

Status: user-requested planning artifact, revised after one six-agent review
round.

Repository: `C:\plms\varinomics\vnm_terminal_surface`

Primary objective: remove the full-viewport snapshot publication cost from
stable sparse dirty-row updates without weakening render snapshot correctness,
selection/copy behavior, transcript/replay evidence, public projection
contracts, or renderer invalidation safety.

This file exists because the user explicitly requested this Markdown plan and
then requested full implementation under this plan. That request is approval to
use this specific file, `dirty_row_lazy_snapshot_plan.md`, as the repo-tracked
control artifact for this refactor. It is not generic approval to commit other
plans, reviews, or scratch notes.

That approval becomes durable only after a selected-path commit of this file, or
after another approved tracked handoff records the same control state. While it
remains untracked, it is an approved local planning artifact rather than a
durable repository control plane.

This file is not a product contract. Any implementation batch that changes
behavior must still update the actual contract documentation and tests in the
same batch.

## 1. Background

The recent block-character mitigation improved per-cell and per-glyph work:

- `include/vnm_terminal/internal/terminal_render_cell_text.h` recognizes common
  terminal graphic BMP code units before the more expensive Unicode
  classification path.
- `src/qsg_terminal_renderer.cpp` routes inline terminal graphics away from the
  text-width validation path and coalesces contiguous same-style full block
  cells into wider graphic rectangles.
- `tests/render_frame/render_frame_tests.cpp` adds coverage for full-block
  graphic rectangle coalescing.

Those changes reduce important costs, but they do not address the larger
publication-stage problem. The existing snapshot publication path can still
construct a full visible-grid snapshot, reserve full cell capacity, scan every
visible row, and append cells for every visible column even when only a small
number of rows changed.

Session-side dirty-row coalescing happens after snapshot construction. That
means it can reduce downstream repaint metadata, but it cannot prevent the
full-snapshot construction work that has already happened.

## 2. Problem Statement

ASCII text performed better because the renderer already has compact fast paths
for printable ASCII:

- simple BMP/text classification;
- no Qt shaping or width validation for the common inline ASCII case;
- contiguous ASCII cells coalesced into longer text runs;
- less per-cell graphics primitive churn.

Block characters performed worse because dense terminal graphics used Unicode
terminal graphic code points rather than printable ASCII:

- every cell could pay more classification work;
- graphics-like Unicode cells could still approach text validation paths;
- dense full-block output could produce one graphic primitive per cell;
- every update still paid the full snapshot publication cost before downstream
  dirty-row handling could help.

The previous mitigation improves the first three bullets. The remaining major
cost is the full-snapshot publication path. The intended fix is to make stable
sparse dirty-row content updates proportional to the number of dirty visible
rows, not proportional to the full visible grid.

## 3. Non-Goals

- Do not add more glyph-specific rendering special cases as the primary fix.
- Do not require a user-facing flag to get acceptable baseline performance.
- Do not publish incomplete snapshots to existing consumers that still expect a
  full row-major `snapshot.cells` vector.
- Do not let QSG consume snapshot rows through a second renderer path that
  bypasses the `Terminal_render_frame` contract.
- Do not borrow mutable model rows into published render snapshots.
- Do not change public projection semantics as part of the first lazy snapshot
  enablement.
- Do not keep superseded helpers, speculative diagnostics, or abandoned
  benchmark artifacts after the batch that makes them unnecessary.

## 4. Core Design

The refactor introduces a private row-content view as the canonical internal
way for production consumers to read snapshot content during the migration.
The existing flat `snapshot.cells` vector remains valid until every production
consumer is migrated or fenced behind an explicit materialization boundary.

The final production shape should support row payloads that are either owned by
the new snapshot or borrowed from a previous published immutable snapshot. A
borrowed row must never point into active model storage or a temporary local
buffer created during snapshot construction.

The lazy producer belongs at the session/content publication boundary. Generic
model snapshot construction must remain able to build detached full snapshots
for arbitrary viewport windows and public projection workflows.

The optimized path applies first only to `LIVE_CONTENT` / `CONTENT` snapshots
when all eligibility checks pass:

- same grid dimensions;
- same visible viewport;
- same active buffer class;
- stable dirty-row mutation identity;
- compatible row-origin generation;
- compatible style/color/mode metadata;
- style and hyperlink ids remapped into the receiving snapshot metadata
  namespace;
- previous published content snapshot available;
- no public projection or detached projection contract in use.

When eligibility fails, a full materialization fallback is allowed only if the
caller receives exactly the same contract as before and the fallback is counted
with a specific reason. If a future caller explicitly requests a lazy/borrowed
contract, failing to deliver that contract must be a hard failure rather than a
silent full fallback.

The first implementation must keep the existing rule that `cell.style_id` and
`cell.hyperlink_id` resolve through the receiving `Terminal_render_snapshot`.
Row-local style or hyperlink metadata would be a separate contract change and
is not part of the initial lazy publication work.

## 5. Consumer Audit

Before production lazy publication is enabled, these consumers must either use
the row-content view or cross an explicit counted materialization boundary:

- frame building in `src/qsg_terminal_renderer.cpp`;
- render snapshot validation and selection helpers in
  `include/vnm_terminal/internal/render_snapshot.h`;
- selection/copy proof and geometry adaptation in `src/terminal_session.cpp`;
- public projection row copying in `src/terminal_public_projection.cpp`;
- transcript diagnostics in `src/terminal_transcript.cpp`;
- replay comparison in `tools/transcript_replay/terminal_transcript_replay.cpp`;
- surface selection and drag-content validation in
  `src/vnm_terminal_surface.cpp`;
- render snapshot, render frame, backend session, parser randomized, QSG atlas,
  and capture/replay tests that assume direct flat-cell access.

This audit must be repeated with `rg` in each migration batch. Deferred call
sites must be assigned to a named successor batch with a concrete reason they
cannot move in the active batch.

Every batch and every material plan amendment must also include these invariant
gates:

- targeted build and tests for the files touched by the batch;
- no-orphan `rg`/`git grep` sweep for helpers, counters, diagnostics, fixtures,
  docs, and benchmark artifacts touched by the batch;
- same-batch deletion for newly orphaned code, or a named current owner and
  materialization boundary;
- independent review before the batch is considered complete;
- explicit transcript-enabled configure/test lane whenever transcript or replay
  behavior is claimed as evidence;
- diagnostics/profile schema validation whenever diagnostic, profile, or
  counter keys are added, renamed, removed, or behaviorally changed.

## 6. Batch 0: Governance And Baseline

Purpose: make the refactor restartable and attributable before changing code.

Work:

- Record the baseline git state for `vnm_terminal_surface`, including existing
  dirty files and untracked plan/review artifacts.
- Decide the durable control mechanism. This plan can serve that role only
  because the user requested this file and then requested implementation under
  it. The control state becomes durable only after this file is tracked and
  committed, unless another explicitly approved tracked handoff artifact
  replaces it.
- Record the performance hypothesis:
  `snapshot -> frame -> QSG` work for stable sparse dirty-row updates should
  scale with dirty visible rows, while ASCII performance and correctness remain
  unchanged.
- Record representative workloads, build type, renderer, dimensions, font,
  QSG environment, instrumentation mode, and hardware.
- Define independent review requirements for every batch.
- Define the per-batch no-orphan sweep and cleanup evidence expected in the
  batch record.

Gates:

- `git status --short --branch` captured in the batch record.
- Existing block-character mitigation either committed, reverted, or recorded
  as the baseline under test.
- Stale build/source check recorded for any selected benchmark build, or
  explicitly marked N/A when Batch 0 records no benchmark evidence.

Batch 0 record, captured 2026-06-13:

- Baseline repository state:
  - Repository: `C:\plms\varinomics\vnm_terminal_surface`.
  - Branch/upstream: `master...origin/master`.
  - HEAD: `8bd794357d3b5d01bed39c5cdc172e0ffc66cb2f`.
  - Staged changes: none. `git diff --cached --stat` was empty.
  - `git status --short --branch`:

    ```text
    ## master...origin/master
     M include/vnm_terminal/internal/terminal_render_cell_text.h
     M src/qsg_terminal_renderer.cpp
     M tests/render_frame/render_frame_tests.cpp
    ?? dirty_row_lazy_snapshot_plan.md
    ?? vnm_terminal_review_roadmap.md
    ```

  - Existing dirty source/test baseline before this Batch 0 amendment:
    `include/vnm_terminal/internal/terminal_render_cell_text.h`,
    `src/qsg_terminal_renderer.cpp`, and
    `tests/render_frame/render_frame_tests.cpp`, with pre-existing diff stat
    of 135 insertions and 19 deletions across those three files. These files
    were captured as the block-character mitigation baseline. Batch 3 second
    amendment now explicitly owns and integrates these three-file hunks as part
    of the Batch 3 implementation diff, using the stable patch-id below as the
    inherited-hunk identity.
  - Aggregate dirty source patch identity:
    - Stable patch-id command:

      ```powershell
      $paths = @(
          'include/vnm_terminal/internal/terminal_render_cell_text.h',
          'src/qsg_terminal_renderer.cpp',
          'tests/render_frame/render_frame_tests.cpp'
      )
      git diff -- $paths | git patch-id --stable
      ```

    - Stable patch-id output:

      ```text
      86b130e7d4f22cad4e88986ffcca17d6d853f805 0000000000000000000000000000000000000000
      ```

    - No byte-for-byte SHA-256 over `git diff` text is recorded here. The
      Batch 0 dirty source baseline is identified by the stable patch-id,
      the exact path list, the diff stat above, and the hunk ownership
      boundaries below.
  - Dirty source hunk and ownership boundaries:
    - `terminal_render_cell_text.h`, private text helpers:
      adds `is_terminal_graphic_source_cell_code_unit()` at current line 367
      and changes `is_single_bmp_source_cell_code_unit()` at current lines
      376-377 so box/block graphic BMP code units bypass the combining-class
      rejection. Owner after Batch 3 second amendment: Batch 3 integration of
      the prior block-character mitigation.
    - `qsg_terminal_renderer.cpp`, `classify_terminal_simple_content_cell()`:
      adds current lines 1084-1093, routing inline single-BMP terminal graphics
      to the `NON_ASCII_TEXT` path after the strict printable-ASCII bypass and
      before inline-BMP width/shaping validation. Owner after Batch 3 second
      amendment: Batch 3 integration of the prior block-character mitigation.
    - `qsg_terminal_renderer.cpp`, `build_terminal_render_frame()`:
      adds `Open_full_block_graphic_run` state at current lines 1751-1759 and
      replaces the old immediate terminal-graphic branch at current lines
      `src/qsg_terminal_renderer.cpp:1889-1953` with full-block U+2588
      graphic-rect coalescing before text-run construction. Owner after
      Batch 3 second amendment: Batch 3 integration of the prior
      block-character mitigation.
    - `render_frame_tests.cpp`: adds
      `test_full_block_graphic_rects_coalesce_contiguous_cells()` at current
      lines 267-308 and wires it into `main()` at current line 2255. Owner
      after Batch 3 second amendment: Batch 3 integration of the prior
      block-character mitigation test coverage.
  - Later workers must treat the mitigation hunks above as Batch 3-owned
    integrated changes unless a reviewed batch intentionally changes them. This
    includes `include/vnm_terminal/internal/terminal_render_cell_text.h`,
    `src/qsg_terminal_renderer.cpp`, and `tests/render_frame/render_frame_tests.cpp`.
  - Existing untracked artifact baseline:
    - `dirty_row_lazy_snapshot_plan.md` was the approved local control artifact
      before Batch 0; it became tracked by the Batch 0 selected-path durability
      commit. Later amendments are staged only when owned by the active batch.
    - `vnm_terminal_review_roadmap.md` is pre-existing untracked scratch for a
      separate review-remediation roadmap. Future dirty-row workers must not
      stage, commit, delete, or `.gitignore` it as part of this refactor unless
      the user explicitly assigns that cleanup.

- Durable control mechanism:
  - User approval record: the user requested this Markdown plan and later
    requested full implementation under this plan. That is approval to use this
    specific file as a tracked control artifact for this refactor; it is not
    approval to commit arbitrary transient plans or review notes.
  - This file remains the canonical control artifact for the dirty-row lazy
    snapshot refactor because it already contains the batch sequence, gates,
    risks, and review focus. A separate Batch 0 file would duplicate control
    state and weaken the single-plan handoff.
  - Batch 0 durability is active in commit `eb992ef`, which selected only
    `dirty_row_lazy_snapshot_plan.md` after clean independent review and the
    lightweight checks. That historical Batch 0 commit did not stage the three
    dirty mitigation source/test files or `vnm_terminal_review_roadmap.md`.
  - Batch 3 second amendment supersedes that historical no-stage restriction
    for `include/vnm_terminal/internal/terminal_render_cell_text.h`,
    `src/qsg_terminal_renderer.cpp`, and
    `tests/render_frame/render_frame_tests.cpp` by explicitly owning and
    integrating the inherited block-character mitigation hunks in the Batch 3
    implementation diff. `vnm_terminal_review_roadmap.md` remains unowned
    scratch and must stay unstaged.
  - If review requires another material amendment, repeat the review and checks
    before the selected-path commit for the active batch.
  - Every later batch must start by reconstructing state from `git status`,
    this plan's latest committed/tracked form, and the previous batch record.
    If those sources disagree, the batch stops for orchestration direction
    rather than inferring state from chat history.

- Performance hypothesis and measurement boundary:
  - Hypothesis: for eligible stable sparse dirty-row content updates,
    `snapshot -> frame -> QSG` work should scale with dirty visible rows rather
    than with `visible_rows * columns`.
  - Expected win: snapshot publication avoids scanning and emitting clean rows,
    and later frame/QSG batches avoid rebuilding clean-row descriptors and
    instance data.
  - Non-regression requirement: ASCII workloads, selection/copy behavior,
    public projection behavior, transcript/replay evidence, validation, and
    renderer invalidation correctness remain unchanged.
  - Cost-shift guard: a faster snapshot stage is not sufficient. Each
    enablement batch must report snapshot construction, frame building, QSG
    prepare, paint/FPS, memory retention, fallback counts, and materialization
    counts for the same workload and build configuration.

- Representative workload and build lanes:
  - Baseline machine captured for later comparison: Windows 11 Enterprise
    10.0.22631, AMD Ryzen 7 7840U with Radeon 780M Graphics, AMD Radeon 780M
    driver 32.0.31007.1017, `QTDIR=C:\Qt\6.10.1`.
  - Build lane `bench-release-profile-off`: fresh MSVC x64 Ninja `Release`
    build initialized through `vcvarsall.bat`, with
    `-DVNM_TERMINAL_BUILD_BENCHMARKS=ON` and
    `-DVNM_TERMINAL_ENABLE_PROFILING=OFF`.
  - Build lane `bench-release-profile-on`: same generator and build type, with
    `-DVNM_TERMINAL_BUILD_BENCHMARKS=ON` and
    `-DVNM_TERMINAL_ENABLE_PROFILING=ON`.
  - Existing model/snapshot/frame stress lane:
    `vnm_terminal_surface_stress_benchmark`. It has no QSG or paint stage, so
    it is stage evidence only. Use its current fixed cell metrics
    `width=8`, `height=16`, `ascent=12`, `descent=4`.
  - Stress benchmark decision matrix unless a reviewed amendment changes it:
    `--frames 180 --warmup-frames 10 --rows 235 --cols 873`, matched
    `--text-pattern ascii --graphics-every 0` and
    `--text-pattern block --graphics-every 0`, `--style-period 8`,
    `--dirty-rows` in `{1, 8, 32, 235}`, `--dirty-row-stride` in `{1, 7}`,
    and column sweeps with `--cols` in `{160, 320, 873}` at 235 rows.
    Profiling-on runs add `--model-profile-stats`.
  - Existing embedded QQuick/QSG lane:
    `vnm_terminal_embedded_benchmark`. On Windows its CTest environment uses
    `QT_QPA_PLATFORM=windows`, `QSG_RENDER_LOOP=threaded`,
    `QSG_RHI_BACKEND=d3d11`, scale factor 1, and the benchmark code calls
    `QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi)`.
    It uses the atlas renderer, the surface default/bundled monospace font,
    and 13 px font size. Defaults are `--grid 24x80`,
    `--window-size 800x480`, `--iterations 3`, and `--warmup 1`.
  - Batch 1 must add end-to-end embedded scenarios before using QSG/paint
    evidence for this refactor. Required new scenarios are sparse-dirty ASCII,
    sparse-dirty dense U+2588 block graphics, cursor-only overlay, selection
    snapshot contract, resize fallback, viewport-change fallback,
    alternate-buffer fallback, style/color/mode fallback, hyperlink fallback,
    public-projection boundary, and transcript/replay boundary when transcript
    evidence is claimed.
  - The full-size end-to-end decision-grade sparse-dirty embedded matrix uses
    Direct3D 11 RHI, the default/bundled monospace font at 13 px, grid
  `235x873`, and window size `6984x3760`, extending the benchmark's current
  grid limit if needed. On this Windows workstation that requested window is
  clamped by the desktop/window manager and cannot close Batch 1. Batch 1's
  executable local D3D11 gate is therefore `--grid 48x160 --window-size
  1280x768`, matching the benchmark's 8x16 cell assumption while staying
  runnable on this machine. The full-size matrix is deferred to the Batch 8
  pre-enable end-to-end performance gate, after Batches 2-7 satisfy the
  row-view, materialization-boundary, lazy-composer, and QSG descriptor-reuse
  predecessor gates. Its file list is
  `benchmarks/embedded_terminal/CMakeLists.txt`,
  `benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp`,
  `docs/repository_guide.md`, and this plan. The predecessor blocker for Batch
  1 is the Windows desktop/window-manager clamp; Batch 8 must run on a
  machine/desktop setup that can actually create the requested window instead
  of silently substituting a smaller grid.
  - Transcript-enabled correctness is required whenever transcript/replay
    behavior is claimed as evidence.

- Independent review process:
  - Each batch and each material plan amendment requires review by an
    independent reader: a different model/person from the implementer, given
    the diff, this plan, the Varinomics standards, and the relevant gate output.
  - Review must classify each finding as blocker, owning-batch risk, or note.
    Current blockers are sequencing contradictions, broken contracts, skipped
    gates, unexplained deterministic failures, orphaned artifacts, silent
    degradation, or unsafe default enablement.
  - The batch round-trips until blocker-clean. The reviewer also verifies that
    the claimed tests, benchmarks, drift checks, and orphan sweeps actually ran.

- No-orphan cleanup evidence expected in every later batch:
  - The batch record must name every public or private helper, counter, profile
    key, diagnostic key, benchmark flag, fixture, and documentation fragment
    added, renamed, removed, or behaviorally changed by the batch.
  - The executor must run targeted `rg` or `git grep` sweeps for touched symbols
    and old names. Newly orphaned code or artifacts are deleted in the same
    batch, or the batch record names the current owner and the explicit
    materialization/control boundary that keeps the artifact live.
  - False-hypothesis experiments, speculative diagnostics, benchmark-only
    switches, and stale generated artifacts are removed when the hypothesis
    fails or when the owning mechanism is removed.

- Stale build/source checks required before Batch 1 and later measurement:
  - Batch 0 stale-build checking is N/A for this amendment because no build,
    test, or benchmark result is used as evidence and no benchmark build
    directory is selected.
  - Use a fresh clean build directory for benchmark evidence, or record why an
    existing directory is acceptable.
  - Before a benchmark or test lane is used as evidence, record the build
    directory, generator, build type, `VNM_TERMINAL_BUILD_BENCHMARKS`,
    profiling/instrumentation options, Qt path, source directory from
    `CMakeCache.txt`, and source HEAD/dirty baseline.
  - Verify the benchmark executable and touched libraries were rebuilt after
    the recorded source changes. If timestamps, CMake cache source paths, or
    revision evidence do not line up, rebuild before collecting evidence.
  - For app-driven end-to-end evidence, verify the app is consuming the intended
    `vnm_terminal_surface` source checkout rather than an installed or stale
    package.
  - Batch 1 cannot close until both selected benchmark build lanes record the
    build directory, cache source path, generator, build type, profiling mode,
    Qt path, HEAD, dirty baseline, executable rebuild evidence, renderer/QSG
    backend, and exact benchmark command lines.

## 7. Batch 1: Measurement And Counters

Purpose: make the bottleneck and later improvements observable before changing
the publication contract.

Work:

- Add counters for:
  - full snapshot publications;
  - visible rows visited;
  - dirty visible rows;
  - cells scanned by snapshot construction;
  - cells emitted into flat storage;
  - rows built from model storage;
  - previous-snapshot borrowed rows unavailable in Batch 1; the live numeric
    counter is future-owned by the borrowed-row/lazy-payload work;
  - rows borrowed from model row accessors inside the full builder;
  - rows materialized by consumers;
  - explicit materialization reason by caller;
  - fallback reason by eligibility check;
    Batch 1 records unavailable placeholder reason fields only; true lazy
    eligibility reason counters are owned by Batch 5.
  - snapshots constructed but superseded before render;
  - snapshots consumed by the bridge/renderer;
  - frame input cells considered;
  - frame row/layer descriptor counter schema boundaries. Batch 1 must not
    invent descriptor counters from visible rows or unowned layer booleans;
    true row/layer descriptor build and reuse counters are owned by Batch 7.
  - retained snapshot payload bytes;
  - retained snapshot generation count.
- Add profiling scopes for snapshot construction, row materialization, frame
  building, QSG prepare, and paint/update stages.
- Add matched ASCII and block-character sparse-dirty workloads at realistic
  CMDG dimensions.
- Add a sparse-dirty end-to-end surface/session benchmark that drives the
  parser/session publication path. A model-only `render_snapshot()` /
  frame-builder benchmark and a synthetic snapshot-bridge benchmark are useful
  diagnostics, but they are not sufficient production-path evidence.
- Add dirty-row-count and column-count sweeps so proportionality can be judged
  by slope, not by a single workload.
- Use the pinned Batch 0 benchmark lanes and matrix unless a reviewed material
  plan amendment changes them. Batch 1 may add commands to the matrix, but it
  must not silently substitute build type, profiling mode, renderer/backend,
  dimensions, font, dirty-row counts, or text patterns.
- For `vnm_terminal_surface_stress_benchmark`, the required first matrix is:
  `Release`, profiling off/on builds, `--frames 180 --warmup-frames 10`,
  `--rows 235 --cols 873`, matched ASCII and dense U+2588 block patterns,
  `--dirty-rows` in `{1, 8, 32, 235}`, `--dirty-row-stride` in `{1, 7}`,
  column sweeps in `{160, 320, 873}`, `--style-period 8`, and
  `--model-profile-stats` in the profiling-on lane.
- For `vnm_terminal_embedded_benchmark`, the required local Batch 1 QSG gate is:
  Windows Direct3D 11 RHI, threaded render loop, atlas renderer, default
  bundled monospace font at 13 px, `--grid 48x160`,
  `--window-size 1280x768`, and the new sparse-dirty/smoke-boundary scenarios
  named in Batch 0. This is the executable baseline for this workstation because the
  previously planned `--grid 235x873 --window-size 6984x3760` lane is clamped by
  Windows here and cannot close. Existing full-dirty or snapshot-bridge
  scenarios may supplement this lane, but cannot replace it. The full-size
  `235x873` / `6984x3760` matrix is deferred to the Batch 8 pre-enable
  end-to-end performance gate, after Batches 2-7 satisfy the row-view,
  materialization-boundary, lazy-composer, and QSG descriptor-reuse predecessor
  gates. The concrete successor file list is
  `benchmarks/embedded_terminal/CMakeLists.txt`,
  `benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp`,
  `docs/repository_guide.md`, and this plan. The predecessor blocker for this
  workstation is the Windows desktop/window-manager clamp that prevents creating
  the requested top-level `6984x3760` window.
- Define the benchmark decision rule before later batches use it:
  - at least two warmup runs per variant;
  - at least ten measured runs per variant;
  - interleaved order, alternating which variant runs first;
  - no cherry-pick removal of outliers, only documented process failures;
  - median and p95 reported;
  - regression if the median worsens by more than the larger of 5 percent or
    twice the measured baseline median absolute deviation.

Gates:

- Release profiling-off validation runs for runnable local FPS/timing lanes.
- Profiling-on validation runs for counters and stage timings across the Batch
  1 sparse, selection, public projection, resize, viewport, alternate-buffer,
  style/color/mode, and hyperlink smoke-boundary scenario set.
- The interleaved A/B decision rule above is recorded in Batch 1 but not run as
  a Batch 1 closure gate. Decision-grade repeated A/B runs are owned by the
  Batch 8 pre-enable end-to-end performance gate named above.
- Diagnostics/profile schema and JSON key validation updated for every
  diagnostic, profile, or counter key added, renamed, removed, or behaviorally
  changed, including per-consumed-update normalization where applicable.
- No production optimization enabled by this batch.

Batch 1 amendment record, 2026-06-13:

- Scope: review amendment only. No row-view, lazy payload, lazy composer, or
  production optimization was enabled.
- Build/source evidence:
  - HEAD during amendment: `eb992ef`.
  - Profile-off lane: `build_codex_renderer_graphics_probe`, Ninja, `Release`,
    `VNM_TERMINAL_BUILD_BENCHMARKS=ON`,
    `VNM_TERMINAL_ENABLE_PROFILING=OFF`,
    `Qt6_DIR=C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6`, source
    `C:/plms/varinomics/vnm_terminal_surface`.
  - Profile-on lane: `build_codex_batch1_profile_on`, Ninja, `Release`,
    `VNM_TERMINAL_BUILD_BENCHMARKS=ON`,
    `VNM_TERMINAL_ENABLE_PROFILING=ON`,
    `Qt6_DIR=C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6`, source
    `C:/plms/varinomics/vnm_terminal_surface`.
  - The first fresh profile-on configure without `Qt6_DIR` failed to find Qt6;
    the recorded profile-on lane is the corrected configure with the explicit
    Qt path above.
- Counter/schema amendments:
  - `snapshots_consumed_by_bridge` now counts one actual advancing bridge sync,
    not skipped generation deltas.
  - `snapshots_superseded_before_render` now counts the latest unsynced
    publication superseded by a new publication, not the whole backlog on each
    publish.
  - Selection-derived blocked publications update the same request,
    constructed, full publication, selection publication, and retained snapshot
    counters as other published snapshots.
  - Embedded benchmark schema 21 removes misleading numeric
    `frame_row_descriptors_*` and `qsg_layer_descriptors_*` counters and
    exposes `descriptor_counters.available=false` with semantics
    `unavailable_until_batch_7_descriptor_reuse`.
  - Surface-stress output removes `frame_row_descriptors_*` and emits
    `frame_row_descriptor_counters_available=false` with the same Batch 7
    ownership semantics.
  - Lazy fallback/decision-boundary labels are smoke boundaries in Batch 1.
    Schema 21 exposes unavailable placeholder reason fields under
    `lazy_snapshot_fallback_reason_counters`; true lazy eligibility/fallback
    reason counters are owned by Batch 5.
- Benchmark amendments:
  - `surface_session_public_projection_boundary` sets
    `IMMEDIATE_PUBLIC_PROJECTION`, enters DECSET 2026, and scrolls while live
    publication is blocked. The profile gate requires nonzero
    `public_projection_scroll_requests` and
    `public_projection_scroll_publications`; the recorded profile output had
    both counters equal to 1.
  - `surface_session_geometry_derived_boundary` enters DECSET 2026 and resizes
    under synchronized-output hold so the geometry-derived direct-output
    counters are proven without using the public-projection scenario as evidence.
  - Sparse embedded surface/session workloads accept `--dirty-rows` and
    `--dirty-row-stride`.
  - Surface-stress sparse runs seed an unmeasured full-grid baseline before
    warmup/measured sparse frames.
- Observed embedded benchmark failure provenance:
  - The review-observed `vnm_terminal_embedded_benchmark_validate` /
    `single_row_geometry_update` `INVALID_CELL_ORDER` failure belonged to the
    Batch 1 benchmark fixture. `make_single_row_geometry_update_snapshot()`
    appended synthetic cells in free-column discovery order, which could be
    non-row-major. The fixture now sorts cells by row and column before
    publishing the synthetic snapshot. Owner: Batch 1 benchmark fixture, not a
    production renderer optimization.
- Commands and results:
  - `git diff --check` passed.
  - `cmd.exe /d /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake -S . -B build_codex_batch1_profile_on -G Ninja -DCMAKE_BUILD_TYPE=Release -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON -DBUILD_TESTING=ON -DQt6_DIR=C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6"` passed.
  - `cmd.exe /d /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark vnm_terminal_render_snapshot vnm_terminal_profile_text"` passed.
  - `cmd.exe /d /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark vnm_terminal_render_snapshot vnm_terminal_profile_text"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_validate)$" --output-on-failure` passed: 3/3.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_profile_validate)$" --output-on-failure` passed: 3/3.
  - `cmake -E env "PATH=C:\Qt\6.10.1\msvc2022_64\bin;$env:PATH" build_codex_renderer_graphics_probe\benchmarks\surface_stress\vnm_terminal_surface_stress_benchmark.exe --frames 5 --warmup-frames 2 --rows 24 --cols 80 --dirty-rows 4 --dirty-row-stride 7 --text-pattern block --graphics-every 0` passed and printed `frame_row_descriptor_counters_available=false`.
  - `cmake -E env "PATH=C:\Qt\6.10.1\msvc2022_64\bin;$env:PATH" build_codex_batch1_profile_on\benchmarks\surface_stress\vnm_terminal_surface_stress_benchmark.exe --frames 5 --warmup-frames 2 --rows 24 --cols 80 --dirty-rows 4 --dirty-row-stride 7 --text-pattern ascii --graphics-every 0 --model-profile-stats` passed and printed model profile counters.
  - `cmd.exe /d /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_diagnostics_schema_sync && ctest --test-dir build_codex_renderer_graphics_probe -R \"^vnm_terminal_diagnostics_schema_sync$\" --output-on-failure"` passed.

Batch 1 second-amendment record, 2026-06-13:

- Scope: second-round review amendment only. No row-view, lazy payload, lazy
  composer, or production optimization was enabled.
- Decision-grade gate amendment:
  - The `235x873` / `6984x3760` embedded Direct3D 11 matrix is not closeable on
    this Windows workstation because the requested top-level window is clamped.
    The Batch 1 local executable gate is `--grid 48x160 --window-size
    1280x768`; the full-size matrix is owned by the Batch 8 pre-enable
    end-to-end performance gate on a setup that can create that window, with
    Windows clamp as the Batch 1 predecessor blocker.
  - Embedded benchmark schema 21 validates `requested_rows`,
    `requested_columns`, actual `rows`/`columns`,
    `actual_grid_matches_request`, and `grid_semantics`. Surface/session
    scenarios may report `surface_session_actual_grid_from_qquick_surface_metrics`;
    snapshot-bridge scenarios may not silently substitute a different grid.
- Benchmark semantics amendments:
  - CTest embedded gates now run the Batch 1 scenarios at `48x160` and
    `1280x768`, with `--dirty-rows 5 --dirty-row-stride 3`.
  - `surface_session_selection_overlay` was reclassified as
    `surface_session_selection_snapshot`. It validates
    `selection_snapshot_spans_observed`; renderer overlay counters are no
    longer claimed as its evidence.
  - The fallback-labeled Batch 1 scenarios were reclassified as smoke-boundary
    scenarios because Batch 1 has no lazy composer or fallback producer:
    `surface_session_resize_smoke_boundary`,
    `surface_session_viewport_change_smoke_boundary`,
    `surface_session_alternate_buffer_smoke_boundary`,
    `surface_session_style_color_mode_smoke_boundary`, and
    `surface_session_hyperlink_smoke_boundary`.
  - `surface_session_style_color_mode_smoke_boundary` now emits reverse-video
    mode transitions in addition to style/color changes.
  - Sparse dirty-row payload generation fails validation if the requested dirty
    row count cannot be touched exactly for the actual grid and stride.
  - Surface-stress descriptor-unavailable semantics are checked by
    `vnm_terminal_surface_stress_descriptor_unavailable_contract`.
- Schema/profile amendments:
  - Descriptor and lazy fallback placeholder objects now validate exact key
    sets and unavailable semantics, so stale numeric descriptor fields cannot
    pass.
  - Consumer materialization counters are available under the Batch 3 schema
    once real materialization-boundary producers exist. The exported counter
    set is the geometry-derived snapshot direct-output calls, rows, and cells.
  - The future previous-snapshot borrowed-row counter is not emitted as a live
    numeric Batch 1 profile counter. Profile text tests assert the available
    materialization schema keys, owner batch, retained max, and generation
    counter keys.
- Retained-memory amendments:
  - Retained payload accounting includes vector capacity and
    `Terminal_render_cell_text` fallback `QString` object/capacity payload.
  - Enabling profile/dirty-row stats refreshes retained snapshot gauges from
    the current retained snapshot state.
- Direct invocation provenance:
  - Plain PowerShell direct launch from the repository root:
    `.\build_codex_renderer_graphics_probe\benchmarks\embedded_terminal\vnm_terminal_embedded_benchmark.exe --scenario surface_session_selection_snapshot --iterations 1 --warmup 0 --grid 48x160 --window-size 1280x768 --quiet --validate-json`
    exited `-1073741511` before benchmark output. This is not a supported
    Windows benchmark gate.
  - The supported Windows invocation is the CTest wrapper or this equivalent
    PowerShell command from the repository root:
    `cmake -E env "QT_QPA_PLATFORM=windows" "QSG_RENDER_LOOP=threaded" "QSG_RHI_BACKEND=d3d11" "QT_SCALE_FACTOR=1" "QT_SCREEN_SCALE_FACTORS=" "QT_AUTO_SCREEN_SCALE_FACTOR=0" "QT_DEVICE_PIXEL_RATIO=" "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough" "PATH=C:\Qt\6.10.1\msvc2022_64\bin;$env:PATH" cmd.exe /d /c call ".\build_codex_renderer_graphics_probe\benchmarks\embedded_terminal\vnm_terminal_embedded_benchmark.exe" --scenario surface_session_selection_snapshot --iterations 1 --warmup 0 --grid 48x160 --window-size 1280x768 --quiet --validate-json`.
    The CTest-equivalent single-scenario command for
    `surface_session_selection_snapshot` exited `0`, reported requested
    `48x160`, actual surface grid `56x177`, semantics
    `surface_session_actual_grid_from_qquick_surface_metrics`, and
    `selection_snapshot_spans_observed=2`.
- Commands and results:
  - `git diff --check` passed.
  - `cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_embedded_benchmark'` passed.
  - `cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_embedded_benchmark'` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_validate|vnm_terminal_surface_stress_descriptor_unavailable_contract)$" --output-on-failure` passed: 4/4.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_profile_validate|vnm_terminal_surface_stress_descriptor_unavailable_contract)$" --output-on-failure` passed: 4/4.
  - `cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_diagnostics_schema_sync vnm_terminal_diagnostics_text_layout'` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_diagnostics_schema_sync|vnm_terminal_diagnostics_text_layout)$" --output-on-failure` passed: 2/2.

Batch 1 third-amendment record, 2026-06-13:

- Scope: third-round review blocker amendment only. No row-view, lazy payload,
  lazy composer, or production optimization was enabled.
- Source/build provenance:
  - HEAD during amendment: `eb992ef`.
  - Relevant status remained dirty with the Batch 1 files and pre-existing
    baseline hunks; no staging or commit was performed. `git status --short
    --branch` reported `master...origin/master [ahead 1]`, the Batch 1 dirty
    files, pre-existing mitigation hunks in
    `include/vnm_terminal/internal/terminal_render_cell_text.h`,
    `src/qsg_terminal_renderer.cpp`, `tests/render_frame/render_frame_tests.cpp`,
    and untracked `vnm_terminal_review_roadmap.md`.
  - Profile-off lane: `build_codex_renderer_graphics_probe`, Ninja, `Release`,
    source `C:/plms/varinomics/vnm_terminal_surface`, existing
    `Qt6_DIR=C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6`,
    `VNM_TERMINAL_BUILD_BENCHMARKS=ON`,
    `VNM_TERMINAL_ENABLE_PROFILING=OFF`.
  - Profile-on lane: `build_codex_batch1_profile_on`, Ninja, `Release`, same
    source and Qt path, `VNM_TERMINAL_BUILD_BENCHMARKS=ON`,
    `VNM_TERMINAL_ENABLE_PROFILING=ON`.
  - Both selected build lanes re-ran CMake during the target rebuild after the
    CMake scenario-list changes, so the generated CTest commands match this
    source tree rather than stale build metadata.
- Governance amendments:
  - The full-size `235x873` / `6984x3760` Direct3D 11 matrix is assigned to the
    Batch 8 pre-enable end-to-end performance gate. The concrete file list is
    `benchmarks/embedded_terminal/CMakeLists.txt`,
    `benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp`,
    `docs/repository_guide.md`, and this plan. The predecessor blocker for
    Batch 1 is the Windows desktop/window-manager clamp on this workstation.
  - Batch 1 does not claim interleaved A/B decision-rule runs. It establishes
    lanes, schema, counters, sparse observed-counter validation, and local
    smoke/profile CTest gates. Repeated interleaved A/B decision runs are owned
    by the named Batch 8 performance gate.
- Benchmark/schema amendments:
  - Profile-on CTest now covers the Batch 1 sparse ASCII, sparse block, cursor
    overlay, selection snapshot, public projection, resize, viewport,
    alternate-buffer, style/color/mode, and hyperlink smoke-boundary scenarios.
  - Fallback-labeled scenarios were renamed to smoke-boundary scenarios because
    Batch 1 has no lazy fallback producer:
    `surface_session_resize_smoke_boundary`,
    `surface_session_viewport_change_smoke_boundary`,
    `surface_session_alternate_buffer_smoke_boundary`,
    `surface_session_style_color_mode_smoke_boundary`, and
    `surface_session_hyperlink_smoke_boundary`.
  - Resize, alternate-buffer, style/color/mode, and hyperlink smoke-boundary
    scenarios now emit observed boundary counters and validation fails if the
    named boundary is not observed. Viewport smoke continues to use the existing
    viewport offset/content structural checks.
  - Public-projection profile validation requires nonzero public-projection
    boundary counters. Geometry-derived snapshot materialization calls, rows,
    and cells are validated by `surface_session_geometry_derived_boundary`, a
    synchronized-output geometry-boundary scenario, so the two evidence
    boundaries are not conflated.
  - Sparse dirty-row validation now checks observed frame counters and, when
    profiling is enabled, model profile counters. It rejects full repaint and
    extra dirty rows beyond the explicit Batch 1 allowance of one cursor
    carry-over row per measured frame.
  - `render_snapshot_rows_borrowed_from_previous_snapshot` was removed from
    live numeric model profile output and tests. Previous-snapshot borrowing is
    owned by a future lazy-payload/lazy-publication batch, not Batch 1.
  - `session_profile_stats` validation now rejects extra top-level profile keys,
    including stale numeric consumer materialization counters. Schema v21 docs
    now spell out exact shapes for `descriptor_counters`,
    `lazy_snapshot_fallback_reason_counters`, and
    `session_profile_stats.consumer_materialization_counters`.
  - Profile text now emits and tests every available geometry-derived
    consumer materialization key plus `owner_batch=Batch 3`.
  - `benchmarks/surface_stress/validate_surface_stress_descriptor_contract.cmake`
    is part of the intended Batch 1 tracked file set and is referenced by
    `benchmarks/surface_stress/CMakeLists.txt`.
- Direct invocation provenance:
  - The exact CTest-equivalent PowerShell command recorded above for
    `surface_session_selection_snapshot` exited `0`, reported requested
    `48x160`, actual surface grid `56x177`, semantics
    `surface_session_actual_grid_from_qquick_surface_metrics`, and
    `selection_snapshot_spans_observed=1`.
- Commands and results:
  - `git diff --check` passed before rebuild and after the final plan update.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark vnm_terminal_render_snapshot vnm_terminal_profile_text"` passed.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark vnm_terminal_render_snapshot vnm_terminal_profile_text"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_validate|vnm_terminal_surface_stress_descriptor_unavailable_contract)$" --output-on-failure` passed: 4/4.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_profile_validate|vnm_terminal_surface_stress_descriptor_unavailable_contract)$" --output-on-failure` passed: 4/4.
  - `cmake -E env "PATH=C:\Qt\6.10.1\msvc2022_64\bin;$env:PATH" build_codex_renderer_graphics_probe\benchmarks\surface_stress\vnm_terminal_surface_stress_benchmark.exe --frames 5 --warmup-frames 2 --rows 24 --cols 80 --dirty-rows 4 --dirty-row-stride 7 --text-pattern block --graphics-every 0` passed, printed `snapshot_dirty_rows_visible_per_frame=5`, `frame_dirty_rows=25`, and `frame_full_dirty_rows=0`.
  - `cmake -E env "PATH=C:\Qt\6.10.1\msvc2022_64\bin;$env:PATH" build_codex_batch1_profile_on\benchmarks\surface_stress\vnm_terminal_surface_stress_benchmark.exe --frames 5 --warmup-frames 2 --rows 24 --cols 80 --dirty-rows 4 --dirty-row-stride 7 --text-pattern ascii --graphics-every 0 --model-profile-stats` passed, printed `snapshot_dirty_rows_visible_per_frame=5`, `frame_dirty_rows=25`, `frame_full_dirty_rows=0`, and no previous-snapshot borrowed-row counter.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_diagnostics_schema_sync vnm_terminal_diagnostics_text_layout && ctest --test-dir build_codex_renderer_graphics_probe -R ""^(vnm_terminal_diagnostics_schema_sync|vnm_terminal_diagnostics_text_layout)$"" --output-on-failure"` passed: 2/2.

## 8. Batch 2: Private Row-Content View

Purpose: introduce the read abstraction while preserving the existing full
snapshot representation.

Work:

- Add a private row-content view over existing full snapshots.
- Provide row iteration, row cell lookup, row text extraction, row cell count,
  provenance access, dirty-range access, and explicit flat materialization.
- Keep `snapshot.cells` complete and row-major.
- Add row-view equivalence tests against the current flat representation.

Gates:

- Render snapshot tests pass.
- Selection text extraction tests pass through both row view and flat helpers.
- Row-view validation proves parity for text, cell metadata including style and
  hyperlink ids, wide-cell continuations, missing-cell-as-space semantics,
  provenance, dirty ranges, cursor metadata, selection metadata, and IME
  metadata.
- No production caller is forced to use the row view yet.

## 9. Batch 3: Consumer Migration Or Materialization Boundaries

Purpose: remove unsafe production dependence on direct `snapshot.cells` before
lazy publication is possible.

Work:

- Move production consumers to the row-content view where practical.
- Add explicit `materialize_flat_cells(reason)` boundaries where full row-major
  storage remains the correct contract.
- Treat public projection as a detached full-viewport publication boundary.
- Treat transcript/replay diagnostics as row-view full logical extraction unless
  a later reviewed batch intentionally adds a counted transcript materialization
  boundary.
- Make row-view text extraction canonical for selection and copy.

Named paths:

- `src/qsg_terminal_renderer.cpp`: frame-builder content access.
- `include/vnm_terminal/internal/render_snapshot.h`: validation and selection
  helpers.
- `src/terminal_session.cpp`: selection proof, geometry adaptation, synchronized
  output visible snapshot handling, bridge publication.
- `src/terminal_public_projection.cpp`: public projection row copy and hyperlink
  metadata treatment.
- `src/terminal_transcript.cpp`: visible row and selected text diagnostics.
- `tools/transcript_replay/terminal_transcript_replay.cpp`: comparable replay
  diagnostics.
- `src/vnm_terminal_surface.cpp`: surface selection and drag-content validation.

Gates:

- `rg "\.cells\b|->cells\b"` shows only intentional
  materialization boundaries, tests, fixtures, and data construction.
- Selection/copy tests cover clean rows, dirty rows, wide cells, missing cells
  as spaces, trailing trim, synchronized-output visible snapshot copy, and
  public-projection copy.
- Public projection tests prove that projection snapshots remain detached,
  full-viewport publications with unchanged output.
- Geometry-derived snapshot tests prove the explicit counted direct-cell
  adaptation/output boundary with unchanged output. Selection-derived snapshot
  tests prove the row-view/copy contract with unchanged output unless a later
  reviewed batch intentionally adds a counted selection materialization boundary.
- Surface selection and drag-content validation tests prove the row-view or
  materialization migration validates the same content as the flat-cell path.
- Transcript/replay tests run in a transcript-enabled configuration and prove
  snapshot diagnostics, selected text diagnostics, visible-row diagnostics, and
  replay comparison output remain semantically unchanged. Counter checks are
  additional evidence, not the only proof.
- Independent review verifies there is no silent partial-snapshot consumer.

Batch 3 second-amendment record, 2026-06-13:

- Scope: second-round blocker amendment only. No lazy payload representation,
  lazy producer, default enablement, or frame/QSG descriptor contract work is
  added here.
- Inherited block-character mitigation ownership:
  - Batch 3 explicitly owns and integrates the inherited mitigation hunks
    identified by stable patch-id
    `86b130e7d4f22cad4e88986ffcca17d6d853f805`.
  - The integrated file set is
    `include/vnm_terminal/internal/terminal_render_cell_text.h`,
    `src/qsg_terminal_renderer.cpp`, and
    `tests/render_frame/render_frame_tests.cpp`.
- Direct-cell and materialization audit:
  - `src/qsg_terminal_renderer.cpp` consumes render snapshot content through
    `Terminal_render_snapshot_row_content_view`; the direct
    `snapshot->cells`/`snapshot.cells` audit for that file must stay empty.
  - `src/terminal_session.cpp` keeps public projection as a detached full
    publication boundary and keeps geometry-derived snapshots as a counted
    direct-cell adaptation/output boundary. Geometry adaptation reads
    `Terminal_render_snapshot_row_content_view` and writes the final
    `snapshot.cells` directly; it does not materialize an intermediate flat
    public-cell vector or build a rows*columns pointer table.
  - Transcript and replay diagnostics are row-view full logical extraction
    boundaries. They are not counted materialization boundaries unless a later
    reviewed batch intentionally adds that contract.
  - Selection/copy uses the row-view/copy contract. It must not require a full
    materialization boundary unless a later reviewed batch intentionally adds
    one.
  - Remaining direct `snapshot.cells`/`.cells` hits are intentional producers,
    detached publication outputs, validation over the canonical flat producer
    contract, codecs/model row storage, fixture construction, parity baselines,
    and benchmark/test accounting.
- No-orphan sweep:
  - Removed orphan row-view validation hook helpers
    `render_snapshot_cell_style_ids_resolve()` and
    `render_snapshot_cell_hyperlink_ids_resolve()`.
  - Removed now-dead row-view hook tests and renamed the malformed row-view
    lookup test so it no longer claims hook coverage.
- Commands and results:
  - `git diff --check` passed.
  - `rg "render_snapshot_cell_style_ids_resolve|render_snapshot_cell_hyperlink_ids_resolve" include src tools benchmarks tests docs` returned no matches.
  - `rg "snapshot_cells_support_row_content_view_order|snapshot->cells|snapshot\.cells" src/qsg_terminal_renderer.cpp` returned no matches.
  - `rg "\.cells\b|->cells\b" include src tools benchmarks tests` was run. Remaining hits are intentional: model row storage and history-row codecs; render-snapshot producers and detached output builders; `render_snapshot.h` validation, row-view, and parity materialization internals; public-projection row storage; width-result `.cells` fields; fixture/data construction; benchmark/test accounting; and parity baselines.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark"` passed.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark"` passed.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_batch3_transcript --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_transcript vnm_terminal_transcript_replay"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_validate)$" --output-on-failure` passed: 4/4.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_profile_validate)$" --output-on-failure` passed: 4/4.
  - `ctest --test-dir build_codex_batch3_transcript -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_transcript)$" --output-on-failure` passed: 4/4.
  - The profile JSON for `surface_session_geometry_derived_boundary` reported
    `geometry_derived_snapshot_calls=1`, `geometry_derived_snapshot_rows=57`,
    `geometry_derived_snapshot_cells=184`, with
    `resize_boundary_row_changes_observed=1` and
    `geometry_derived_boundary_adapted_rows_observed=57`,
    `geometry_derived_boundary_adapted_cells_observed=184`.

## 10. Batch 4: Lazy Payload Representation, Test-Only

Purpose: prove mixed owned/borrowed row payloads without changing production
publication behavior.

Work:

- Add immutable row payloads that can be owned by the current snapshot or
  borrowed from a previous published immutable snapshot.
- Define the row-payload ownership model. A borrowed row must keep its source
  snapshot row-payload owner alive across source snapshot release and
  `Terminal_render_snapshot` copy/move.
- Store enough row identity to validate content, provenance, style table
  compatibility, hyperlink namespace compatibility, and row-origin generation.
- Remap borrowed row style and hyperlink ids into the receiving snapshot
  metadata namespace. Do not expose row-local metadata to frame building,
  public projection, or validation in this batch.
- Define the retained generation and retained byte policy for borrowed row
  payloads, including eviction behavior for superseded snapshots.
- Add explicit materialization parity tests.
- Add lifetime tests proving a lazy snapshot survives model mutation and model
  destruction, source snapshot release, and snapshot value copy/move.
- Keep production snapshots fully materialized.

Gates:

- Full materialization from mixed owned/borrowed rows exactly matches the full
  producer for equivalent inputs.
- Validation detects unresolved style or hyperlink ids through the row view.
- Validation proves borrowed row ids resolve through the receiving snapshot
  metadata arrays after remap.
- Tests prove no row payload borrows active model rows or temporary builder
  buffers.
- Retention tests cover snapshot backlog, superseded snapshots, and the
  documented generation/byte policy.
- No production lazy publication enabled.

Batch 4 implementation record, 2026-06-13:

- Scope: test-only lazy row payload representation. Production snapshot
  publication remains fully materialized; no session, model producer, frame
  builder, public projection, transcript, replay, or QSG path constructs lazy
  payloads.
- Ownership model:
  - `Terminal_render_snapshot_row_payload_owner` owns immutable row payload
    vectors and records a retained generation plus retained byte estimate.
  - `Terminal_render_snapshot_lazy_row_payload` stores a source-owner
    `shared_ptr` plus row index. Borrowed rows therefore keep the source
    row-payload owner alive across source snapshot release and
    `Terminal_render_snapshot` copy/move.
  - Borrowed rows may carry receiving-namespace remapped cells. Row content
    view, materialization, and validation see only receiving-snapshot
    `style_id` and `hyperlink_id` values.
- Identity and validation:
  - Each immutable row payload records viewport row, visible-line provenance,
    row-origin generation, and source style/hyperlink namespace generation.
  - Lazy snapshots carry no flat `snapshot.cells` backing; validation rejects
    any nonempty flat cell vector when `lazy_row_payloads` is present.
  - Lazy validation rejects row/provenance/row-origin mismatch, unresolved
    receiving style ids, unresolved receiving hyperlink ids, and receiving
    namespace mismatch. Receiving-namespace rows must preserve the source row
    content exactly except for `style_id` and `hyperlink_id` translations, and
    validation rejects hyperlink link/no-link changes such as nonzero source ids
    remapped to zero. Existing flat validation remains the production path when
    `lazy_row_payloads` is empty.
- Retention policy:
  - `Terminal_render_snapshot_row_payload_retention` retains the newest
    configured generation count, replaces superseded owners with the same
    generation, and evicts oldest owners until the retained byte limit is
    satisfied.
  - Retained byte estimates include fallback `QString` heap payloads inside
    `Terminal_render_cell_text`.
  - Evicting an owner from the retention backlog does not invalidate snapshots
    that already borrowed rows from that owner.
- Test evidence added in `tests/render_snapshot/render_snapshot_tests.cpp`:
  - Mixed owned/borrowed materialization parity against the full model
    producer, including rejection of lazy snapshots that retain flat cells.
  - Borrowed-row style and hyperlink remap into the receiving snapshot
    namespace, plus altered-content, nonzero-hyperlink-to-zero, unresolved-id,
    and namespace-mismatch validation failures.
  - Lifetime across model mutation/destruction, source snapshot release, and
    `Terminal_render_snapshot` copy/move.
  - Retained generation, retained byte with long fallback text, superseded-owner,
    and borrowed-after-eviction behavior.
- Commands and results:
  - `git diff --check` passed.
  - `rg -n "lazy_row_payloads\s*=|make_shared<.*Terminal_render_snapshot_lazy_payloads|render_snapshot_row_payload_owner_from_snapshot|borrowed_render_snapshot_lazy_row_payload\(" src tools benchmarks` returned no matches.
  - `rg -n "lazy_row_payloads\s*=|make_shared<.*Terminal_render_snapshot_lazy_payloads|render_snapshot_row_payload_owner_from_snapshot|borrowed_render_snapshot_lazy_row_payload\(" include\vnm_terminal\internal\render_snapshot.h tests\render_snapshot\render_snapshot_tests.cpp` showed only the internal representation helpers and render snapshot tests.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_render_snapshot"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^vnm_terminal_render_snapshot$" --output-on-failure` passed: 1/1.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_embedded_benchmark_validate)$" --output-on-failure` passed: 4/4.

## 11. Batch 5: Eligibility Signal And Disabled Lazy Composer

Purpose: build the lazy publication decision logic behind a disabled or
test-only entry point.

Work:

- Carry `dirty_rows_have_stable_mutation_identity` into the session-level lazy
  composer input.
- Add specific fallback reasons:
  - missing previous content snapshot;
  - grid mismatch;
  - viewport mismatch;
  - active buffer mismatch;
  - public projection;
  - row-origin generation mismatch;
  - style/color/mode incompatibility;
  - hyperlink namespace incompatibility;
  - unstable dirty-row mutation identity;
  - unsupported geometry or detached snapshot path.
- Compare lazy materialization against the current full producer after each
  mutation in targeted tests.
- Keep generic `Terminal_screen_model::render_snapshot()` as a full detached
  builder.

Gates:

- Producer equivalence tests pass for dirty rows, clean rows, viewport changes,
  resize, row-origin changes, alternate buffer, synchronized output release,
  style/color/mode changes, hyperlink changes, and missing prior snapshot.
- Public projection, geometry-derived snapshots, and detached projection paths
  prove their explicit fallback/materialization reasons before any production
  lazy path is enabled.
- Dirty ranges are identical or conservatively wider than the full path.
- Fallback counters identify every ineligible case.
- Production path still uses full materialization.

Batch 5 implementation record, 2026-06-13:

- Scope: disabled/test-only lazy composer eligibility and parity checks only.
  Production snapshot publication remains fully materialized; no default
  session publication path constructs or publishes lazy row payloads.
- Session eligibility model:
  - `dirty_rows_have_stable_mutation_identity` is carried from the current
    `Terminal_screen_model_result` into the session-level test-only composer
    input.
  - `Terminal_lazy_snapshot_fallback_reason` records explicit ineligible
    boundaries for missing previous content snapshots, grid mismatch, viewport
    mismatch, active-buffer mismatch, public projection, row-origin generation
    mismatch, style/color/mode incompatibility, hyperlink namespace
    incompatibility, unstable dirty-row mutation identity, and unsupported
    geometry or detached snapshot paths.
  - Session profile stats, profile text, and embedded benchmark JSON now expose
    Batch 5 numeric reason counters and scalar eligibility/parity counters.
    The Batch 1 unavailable placeholders are replaced by
    `batch_5_lazy_eligibility` semantics under embedded benchmark schema 22.
    Schema 22 remains the final Batch 5 schema version for this unpushed
    schema shape.
- Disabled composer behavior:
  - `Terminal_session::compose_lazy_render_snapshot_for_testing()` evaluates
    eligibility under the session lock, uses Batch 4 immutable row payloads for
    eligible content snapshots, borrows clean rows from the prior content
    snapshot without remap copies when metadata ids are already in the
    receiving namespace, owns dirty rows from the current full snapshot, and
    compares row view materialization against the current full producer output
    with `ROW_VIEW_PARITY_TEST`.
  - Eligibility-passing malformed composer inputs, such as omitted dirty-row
    metadata that would materialize stale clean rows, are rejected without an
    explicit fallback reason and do not return or count an eligible lazy
    snapshot. They count only the test-only materialization mismatch counter.
  - Generic `Terminal_screen_model::render_snapshot()` remains a full detached
    flat-cell builder. The production `Terminal_session::publish_render_snapshot`
    path still constructs `Terminal_render_snapshot snapshot =
    m_screen_model->render_snapshot(request);` and records full snapshot
    publications.
- Test evidence added:
  - Dirty-row parity after synced partial mutations proves lazy materialization
    matches the full producer, dirty ranges remain identical to the full path,
    every dirty row is sourced from the current snapshot payloads, every clean
    row is borrowed from the previous content payloads without a remap copy
    when metadata is compatible, and production snapshots still carry no lazy
    payloads.
  - Synthetic fallback tests prove missing previous content snapshot, grid,
    viewport, active-buffer, public-projection, row-origin, style/color/mode,
    hyperlink namespace, unstable dirty-row identity, geometry-derived, and
    detached-path reasons and their counters.
  - Session-boundary tests cover viewport movement, resize, alternate buffer,
    synchronized output release, mode-state change, and a real hyperlink
    namespace change that remains compatible and materializes successfully.
  - Existing Batch 4 row-payload tests continue to cover detached/lazy row-view
    validation, remap, lifetime, and retention semantics.
- Commands and results:
  - `git diff --check` passed.
  - The removed Batch 5 counter/input/helper symbol grep returned no matches.
  - The stale Batch 5 unavailable-schema label grep returned no matches.
  - `rg -n "lazy_row_payloads\s*=|make_shared<.*Terminal_render_snapshot_lazy_payloads|render_snapshot_row_payload_owner_from_snapshot|borrowed_render_snapshot_lazy_row_payload\(" src tools benchmarks -S` showed lazy payload construction only in the disabled/test-only session composer; the default publication path remains full materialization.
  - `rg -n "Terminal_render_snapshot snapshot = m_screen_model->render_snapshot\(request\)|full_snapshot_publications|compose_lazy_render_snapshot_for_testing|lazy_snapshot\.lazy_row_payloads|publish_render_snapshot" src\terminal_session.cpp -S` confirmed the production publish path still calls the full model producer and increments full-publication counters; lazy payload assignment is outside that path.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark vnm_terminal_backend_session"` passed.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark vnm_terminal_backend_session"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_backend_session|vnm_terminal_embedded_benchmark_validate)$" --output-on-failure` passed: 5/5.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_backend_session|vnm_terminal_embedded_benchmark_profile_validate)$" --output-on-failure` passed: 5/5.

## 12. Batch 6: Narrow Lazy Path, Not Default

Purpose: exercise the safe core lazy content path in an end-to-end
surface/session workflow without making it the default production path.

Work:

- Exercise the lazy composer for eligible `LIVE_CONTENT` / `CONTENT` snapshots
  in tests and benchmarks: same grid, same viewport, stable mutation identity,
  compatible metadata, and previous published content snapshot available.
- Public projection and detached projections remain explicit full-output
  boundaries. Geometry-derived snapshots remain counted direct-cell
  adaptation/output boundaries. Transcript/replay remain row-view full logical
  extraction boundaries unless a later reviewed batch intentionally adds counted
  materialization.
- Count every full fallback and every consumer materialization.
- Keep the default external snapshot contract fully materialized.
- Collect downstream frame/QSG counters even if their optimization lands in the
  next batch, so cost shifting is visible before default enablement.

Gates:

- On pure content sparse-dirty workloads, `K = 0` and:
  - full fallback counters are zero;
  - previous-snapshot borrowed rows equal
    `visible_rows - dirty_rows_visible`;
  - cells scanned by snapshot construction are bounded by
    `dirty_rows_visible * columns`;
  - rows owned/materialized by the producer are bounded by
    `dirty_rows_visible`.
- On overlay workloads, `K` must be declared before the run as the exact set of
  promoted non-content rows, such as old/new cursor rows, selection rows, or IME
  rows. The same formulas apply with `dirty_rows_visible + K`.
- Dirty-row-count and column-count sweeps show fixed proportional slopes rather
  than full-visible-grid behavior.
- ASCII median and p95 performance do not regress beyond measured noise.
- Block-character sparse-dirty performance improves in the publication stage,
  and any downstream frame/QSG cost is reported.
- Correctness tests from Batches 2 through 5 still pass.
- Independent review confirms that default production behavior is still full
  materialization.

Batch 6 implementation record, 2026-06-13:

- Scope: narrow opt-in lazy content exercise only. Default production snapshot
  publication remains fully materialized; `Terminal_session::publish_render_snapshot()`
  still publishes the full `Terminal_screen_model::render_snapshot()` output.
- Composer exercise path:
  - `Terminal_session::compose_lazy_render_snapshot_for_testing()` now reports
    per-call dirty visible rows, previous-snapshot borrowed rows, producer-owned
    rows, producer-materialized rows, producer cells scanned/emitted, and
    consumer materialization counts.
  - Eligible dirty rows are copied into a compact current dirty-row payload
    owner; clean rows borrow from the previous published content snapshot. The
    current producer-owned/materialized/scanned counts are therefore bounded by
    visible dirty rows in the opt-in exercise path.
  - Full fallback counters increment for ineligible composer checks. The
    explicit row-view parity materialization is counted as a consumer
    materialization under `ROW_VIEW_PARITY_TEST`.
- Surface/session and benchmark exercise:
  - `VNM_TerminalSurface_render_bridge::compose_lazy_render_snapshot_for_testing()`
    exposes the composer only through an internal testing/benchmark bridge.
  - `vnm_terminal_surface_host` drives a surface-backed session, renders the
    baseline and mutation through the downstream renderer path, calls the
    opt-in composer, and verifies the production snapshots remain full.
  - Embedded sparse surface/session benchmark scenarios call the opt-in composer
    after the production snapshot/render wait and emit schema 23 lazy exercise
    counters beside existing frame/QSG counters. These sparse content scenarios
    declare `K = 0` through
    `lazy_snapshot_exercise_promoted_non_content_rows`.
- Schema and diagnostics:
  - Embedded benchmark schema 23 adds scenario-level lazy exercise counters and
    session profile counters for lazy fallbacks, borrowed rows, producer-owned
    rows, producer materialization, producer scan/emission, and counted
    row-view parity materialization.
  - `session_profile_stats.consumer_materialization_counters` now has
    `batch_6_materialization_boundaries` semantics and includes both
    geometry-derived direct-output counters and row-view parity test counters.
- Evidence scope:
  - The benchmark validation is a schema/counter smoke gate for the opt-in path
    and downstream frame/QSG visibility. Decision-grade A/B timing, default
    enablement, and full-size performance evidence remain owned by Batch 8.
- Commands and results:
  - `git diff --check` passed.
  - Lazy publication audit passed: `rg` showed `lazy_snapshot.lazy_row_payloads`
    assignment only in the explicit composer, plus manual render snapshot tests;
    `Terminal_session::publish_render_snapshot()` still constructs
    `Terminal_render_snapshot snapshot = m_screen_model->render_snapshot(request)`
    and publishes that full snapshot handle.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_renderer_graphics_probe --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark vnm_terminal_backend_session vnm_terminal_surface_host"` passed.
  - `cmd.exe /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build_codex_batch1_profile_on --target vnm_terminal_render_snapshot vnm_terminal_render_frame vnm_terminal_profile_text vnm_terminal_embedded_benchmark vnm_terminal_backend_session vnm_terminal_surface_host"` passed.
  - `ctest --test-dir build_codex_renderer_graphics_probe -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_backend_session|vnm_terminal_surface_host|vnm_terminal_embedded_benchmark_validate)$" --output-on-failure` passed: 6/6.
  - `ctest --test-dir build_codex_batch1_profile_on -R "^(vnm_terminal_render_snapshot|vnm_terminal_render_frame|vnm_terminal_profile_text|vnm_terminal_backend_session|vnm_terminal_surface_host|vnm_terminal_embedded_benchmark_profile_validate)$" --output-on-failure` passed: 6/6.

## 13. Batch 7: Frame Builder And QSG Descriptor Reuse

Purpose: ensure the publication improvement is not lost by a full downstream
cell walk.

Work:

- Keep row-content-view input to the frame builder as the canonical frame path
  while extending downstream QSG descriptor/reuse from the produced
  `Terminal_render_frame`.
- Any row/layer descriptors must be fields or implementation details of the
  canonical frame path, or strictly derived from `Terminal_render_frame` before
  QSG consumes them. They must not become a second public renderer input.
- Do not let QSG bypass the render-frame contract.
- Add per-row/per-layer descriptors for text, backgrounds, graphics,
  decorations, cursor inverse text, selection rectangles, IME/preedit, visual
  bell, and hyperlink underline state.
- Add row-stable descriptor or upload evidence for rect and graphic instance
  buffers, including dense block-character `graphic_rects`, not only glyph/text
  buffers.
- Define descriptor equality keys for:
  - row content identity;
  - style and color state;
  - reverse-video mode;
  - render options;
  - cell metrics;
  - font epoch;
  - opacity;
  - cursor row and previous cursor row;
  - selection ranges;
  - IME/preedit state;
  - visual bell state;
  - hyperlink underline policy.
- Promote overlay/global invalidation separately from content dirty rows.

Gates:

- Frame tests compare output built from row view and explicit flat
  materialization.
- QSG/atlas tests cover borrowed clean-row reuse, dirty-row partial update,
  unexpected clean-row change fallback, cursor movement, selection changes, IME
  changes, visual bell, font changes, style/color changes, and dense
  block-graphic rect changes.
- Frame `cells_considered` and descriptor build counters scale with dirty rows
  on eligible sparse-dirty workloads.
- QSG prepare time does not hide the snapshot improvement by rebuilding all
  clean-row instance data.
- Rect/graphic instance buffers show row-stable layout or bounded dirty-row
  uploads for dense block-character workloads.

## 14. Batch 8: Default Production Enablement

Purpose: enable the optimized path only after snapshot, frame, QSG, and
end-to-end surface/session evidence all pass.

Work:

- Enable lazy publication by default only for the eligible `LIVE_CONTENT` /
  `CONTENT` case proven in Batches 6 and 7.
- Keep public projection and detached projections on their explicit full-output
  contracts. Keep geometry-derived snapshots on their counted direct-cell
  adaptation/output contract. Keep transcript/replay on row-view full logical
  extraction unless a separate reviewed batch intentionally adds counted
  materialization.
- Keep all fallback and materialization counters active.

Gates:

- End-to-end surface/session benchmark proves improvement or no cost shift
  across `snapshot -> frame -> QSG -> paint/FPS`.
- Snapshot construction, frame building, QSG prepare, memory retention, and
  renderer FPS are all reported in the same comparison.
- Eligible pure content workloads have zero full fallback and zero unexpected
  consumer materialization.
- Public projection, transcript/replay, geometry-derived snapshots, and surface
  selection/drag validation tests pass before default enablement.
- ASCII median and p95 performance do not regress under the Batch 1 decision
  rule.
- Dense block-character sparse-dirty performance improves in the user-visible
  path or the batch does not enable the optimization by default.
- Retained snapshot memory remains within the policy defined in Batch 4.
- Independent review accepts the default enablement evidence.

## 15. Batch 9: Expansion And Cleanup

Purpose: expand the optimized path only after boundaries are explicit, and
remove superseded code as it becomes unused.

Work:

- Consider adding transcript/replay materialization only if a reviewed
  row-payload schema or counter boundary is worth the added contract surface.
- Consider optimizing geometry-derived snapshots only after the base content
  path is stable.
- Keep public projection full unless a separate public-projection contract
  change is planned and reviewed.
- Delete helpers, diagnostics, counters, fixtures, and docs that no longer have
  a current owner.

Gates:

- `rg "\.cells\b|->cells\b"` audit allows only intentional direct
  construction, explicit materialization, fixtures, or tests. The audit must
  include aliases such as `safe_basis.cells` and `public_snapshot.cells`, not
  only variables named `snapshot`.
- `rg "render_snapshot_cells_by_position"` audit allows only intentional
  current materialization boundaries with named owners, or removed call sites.
- `rg "selected_text_from_render_snapshot"` audit verifies the canonical
  selection path.
- No obsolete diagnostics or benchmark-only switches remain without a named
  owner.

## 16. Batch 10: Final Evidence Gate

Purpose: confirm the end-to-end effect and prevent cost shifting.

Correctness gates:

- Render snapshot tests.
- Render frame tests.
- QSG atlas tests.
- Backend session tests.
- Capture/replay conformance tests.
- Transcript targeted tests in a transcript-enabled configuration.
- Selection/copy targeted tests.
- Public projection targeted tests.
- Wide-cell, combining-mark, hyperlink, style, IME, cursor, and dense
  block-character tests.

Performance gates:

- Release profiling-off FPS/timing lane.
- Profiling-on counter/stage lane.
- Matched ASCII and block-character workloads.
- Interleaved runs using the Batch 1 decision rule.
- Snapshot construction, frame building, QSG prepare, memory retention, and
  final renderer FPS all reported.

Acceptance criteria:

- Stable sparse dirty-row updates are proportional to dirty visible rows through
  snapshot publication, frame building, and QSG prepare.
- QSG prepare does not rebuild all clean-row content data on eligible updates.
- ASCII performance does not regress beyond measured noise.
- Dense block-character updates improve at the publication/frame stages.
- Full fallback and materialization counters are zero on eligible workloads.
- Ineligible workloads fall back explicitly with counted reasons and unchanged
  external behavior.
- Retained snapshot memory remains bounded by the documented generation policy.
- No direct flat-cell production consumers remain outside named construction,
  fixture, or materialization boundaries.

## 17. Risks And Review Focus

Primary risks:

- Enabling lazy publication before `snapshot.cells` consumers are safe.
- Treating dirty-row repaint invalidation as the same decision as content
  materialization.
- Borrowing mutable model rows.
- Borrowing rows whose style or hyperlink ids do not resolve in the receiving
  snapshot metadata.
- Moving cost from snapshot construction into frame building or QSG prepare.
- Allowing public projection, transcript/replay, or selection/copy to compare
  partial content outside their current full-output contracts: detached
  full-viewport publication for public projection, row-view full logical
  extraction for transcript/replay, and row-view full-range copy for
  selection/copy. A later materialization boundary must be reviewed and counted.
- Keeping abandoned helper paths after migration.

Reviewers should classify findings as:

- blocker: current sequencing contradiction, broken contract, unsafe enablement,
  or governance deadlock;
- serious risk: issue owned by a named future batch;
- note: optional improvement that should not expand the plan without evidence.

## 18. Current Handoff

Batch 7 now contains the frame-builder and QSG descriptor/reuse work from
Section 13. Row-content-view input remains the canonical frame-builder path,
QSG descriptor and reuse keys are derived from the produced
`Terminal_render_frame`, and no second public renderer input was added. Lazy
publication is still not enabled by default.

Reviewers should review the Batch 7 diff and evidence before Batch 8. The
Batch 7 evidence covers row-view versus flat frame parity, sparse dirty-row
frame descriptor scaling, atlas clean-row reuse and dense block graphic rect
row-stable upload evidence, descriptor/profile/benchmark schema exposure, and
the targeted render-frame, render-snapshot, profile-text, QSG/atlas,
surface-host, backend-session, embedded-benchmark, and surface-stress
validators. If accepted, proceed to Batch 8 default production enablement and
decision-grade end-to-end performance gates.
