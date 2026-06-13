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
    are the current block-character mitigation baseline under test; Batch 0
    does not change them.
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
      rejection. Owner: prior-session block-character mitigation.
    - `qsg_terminal_renderer.cpp`, `classify_terminal_simple_content_cell()`:
      adds current lines 1084-1093, routing inline single-BMP terminal graphics
      to the `NON_ASCII_TEXT` path after the strict printable-ASCII bypass and
      before inline-BMP width/shaping validation. Owner: prior-session
      block-character mitigation.
    - `qsg_terminal_renderer.cpp`, `build_terminal_render_frame()`:
      adds `Open_full_block_graphic_run` state at current lines 1751-1759 and
      replaces the old immediate terminal-graphic branch at current lines
      `src/qsg_terminal_renderer.cpp:1889-1953` with full-block U+2588
      graphic-rect coalescing before text-run construction. Owner:
      prior-session block-character mitigation.
    - `render_frame_tests.cpp`: adds
      `test_full_block_graphic_rects_coalesce_contiguous_cells()` at current
      lines 267-308 and wires it into `main()` at current line 2255. Owner:
      prior-session block-character mitigation test coverage.
  - Later workers must preserve the prior-session mitigation hunks above unless
    a reviewed batch intentionally owns and changes them. This is especially
    important for `src/qsg_terminal_renderer.cpp` and
    `tests/render_frame/render_frame_tests.cpp`, which later frame/QSG batches
    are likely to touch.
  - Existing untracked artifact baseline:
    - `dirty_row_lazy_snapshot_plan.md` is this approved control artifact, but
      remains untracked until the selected-path durability commit happens.
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
  - Batch 0 durability is not active yet because this file is still untracked.
    The precise closure condition is a selected-path commit of only
    `dirty_row_lazy_snapshot_plan.md` after clean independent review and the
    lightweight checks. That commit must not stage the three dirty mitigation
    source/test files or `vnm_terminal_review_roadmap.md`.
  - If review requires another material amendment, repeat the review and checks
    before the selected-path durability commit. Until then, the plan is a local
    untracked artifact and cannot by itself satisfy the repository-control-plane
    requirement from change governance.
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
    overlay, resize fallback, viewport-change fallback, alternate-buffer
    fallback, style/color/mode fallback, hyperlink fallback, public-projection
    boundary, and transcript/replay boundary when transcript evidence is
    claimed.
  - The first end-to-end decision-grade sparse-dirty embedded runs must use
    Direct3D 11 RHI, the default/bundled monospace font at 13 px, grid
    `235x873`, and window size `6984x3760`, extending the benchmark's current
    grid limit if needed. If that grid cannot run on the machine, the batch
    must amend this plan under review instead of silently substituting a smaller
    matrix.
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
  - rows borrowed from previous published snapshots;
  - rows borrowed from model row accessors inside the full builder;
  - rows materialized by consumers;
  - explicit materialization reason by caller;
  - fallback reason by eligibility check;
  - snapshots constructed but superseded before render;
  - snapshots consumed by the bridge/renderer;
  - frame input cells considered;
  - frame row descriptors built/reused;
  - QSG layer descriptors built/reused;
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
- For `vnm_terminal_embedded_benchmark`, the required first QSG lane is:
  Windows Direct3D 11 RHI, threaded render loop, atlas renderer, default
  bundled monospace font at 13 px, `--grid 235x873`,
  `--window-size 6984x3760`, and the new sparse-dirty/fallback scenarios named
  in Batch 0. Existing full-dirty or snapshot-bridge scenarios may supplement
  this lane, but cannot replace it.
- Define the benchmark decision rule before later batches use it:
  - at least two warmup runs per variant;
  - at least ten measured runs per variant;
  - interleaved order, alternating which variant runs first;
  - no cherry-pick removal of outliers, only documented process failures;
  - median and p95 reported;
  - regression if the median worsens by more than the larger of 5 percent or
    twice the measured baseline median absolute deviation.

Gates:

- Release profiling-off runs for FPS/timing.
- Profiling-on runs for counters and stage timings.
- Repeated interleaved A/B runs with the decision rule above.
- Diagnostics/profile schema and JSON key validation updated for every
  diagnostic, profile, or counter key added, renamed, removed, or behaviorally
  changed, including per-consumed-update normalization where applicable.
- No production optimization enabled by this batch.

## 8. Batch 2: Private Row-Content View

Purpose: introduce the read abstraction while preserving the existing full
snapshot representation.

Work:

- Add a private row-content view over existing full snapshots.
- Provide row iteration, row cell lookup, row text extraction, row cell count,
  provenance access, dirty-range access, style/hyperlink validation hooks, and
  explicit flat materialization.
- Keep `snapshot.cells` complete and row-major.
- Add row-view equivalence tests against the current flat representation.

Gates:

- Render snapshot tests pass.
- Selection text extraction tests pass through both row view and flat helpers.
- Row-view validation proves parity for text, styles, hyperlinks, wide-cell
  continuations, missing-cell-as-space semantics, provenance, dirty ranges,
  cursor metadata, selection metadata, and IME metadata.
- No production caller is forced to use the row view yet.

## 9. Batch 3: Consumer Migration Or Materialization Boundaries

Purpose: remove unsafe production dependence on direct `snapshot.cells` before
lazy publication is possible.

Work:

- Move production consumers to the row-content view where practical.
- Add explicit `materialize_flat_cells(reason)` boundaries where full row-major
  storage remains the correct contract.
- Treat public projection as a detached full-viewport publication boundary.
- Treat transcript/replay as either a counted full logical materialization
  boundary or a reviewed schema change. The first implementation should prefer
  counted materialization.
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
- Geometry-derived and selection-derived snapshot tests prove those paths cross
  explicit full materialization boundaries with unchanged output.
- Surface selection and drag-content validation tests prove the row-view or
  materialization migration validates the same content as the flat-cell path.
- Transcript/replay tests run in a transcript-enabled configuration and prove
  snapshot diagnostics, selected text diagnostics, visible-row diagnostics, and
  replay comparison output remain semantically unchanged. Counter checks are
  additional evidence, not the only proof.
- Independent review verifies there is no silent partial-snapshot consumer.

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

## 12. Batch 6: Narrow Lazy Path, Not Default

Purpose: exercise the safe core lazy content path in an end-to-end
surface/session workflow without making it the default production path.

Work:

- Exercise the lazy composer for eligible `LIVE_CONTENT` / `CONTENT` snapshots
  in tests and benchmarks: same grid, same viewport, stable mutation identity,
  compatible metadata, and previous published content snapshot available.
- Public projection, geometry-derived snapshots, transcript/replay boundaries,
  and detached projections remain explicit materialization boundaries unless
  separately reviewed.
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

## 13. Batch 7: Frame Builder And QSG Descriptor Reuse

Purpose: ensure the publication improvement is not lost by a full downstream
cell walk.

Work:

- Make the frame builder consume the row-content view and produce the canonical
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
- Keep public projection, geometry-derived snapshots, transcript/replay
  boundaries, and detached projections on their explicit full materialization
  contracts unless a separate reviewed batch changes them.
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

- Consider removing materialization from transcript/replay only if a reviewed
  row-payload schema is worth the added contract surface.
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
  partial content without an explicit materialization boundary.
- Keeping abandoned helper paths after migration.

Reviewers should classify findings as:

- blocker: current sequencing contradiction, broken contract, unsafe enablement,
  or governance deadlock;
- serious risk: issue owned by a named future batch;
- note: optional improvement that should not expand the plan without evidence.

## 18. Immediate Next Step

Start with Batch 0 and Batch 1 only. Do not implement lazy publication until
the consumer migration, materialization boundary work, and downstream
frame/QSG proof path have passed independent review. Do not enable lazy
publication by default before Batch 8 gates pass.
