# Repository Guide

This guide maps the repository for build, test, provenance, and maintenance tasks.
It is the quick operational reference after `docs/developer_orientation.md`,
`docs/architecture.md`, and `docs/public_surface.md`.

## Top-Level Layout

- `include/vnm_terminal/vnm_terminal_surface.h` is the public Qt Quick item.
- `include/vnm_terminal/internal` contains source-tree internal contracts shared
  by the session, backend, model, input, renderer, fixture, and generated table
  code. It is not installed API.
- `src` contains the implementation for the surface, ordered terminal session,
  screen model, input encoder, renderer, Unicode width tables, and platform
  backends.
- `resources` contains assets compiled into the library, including the bundled
  monospace font.
- `cmake` contains project CMake helpers. The Qt posture helper validates the
  accepted Qt route and allowed Qt module targets.
- `tools/terminal_canvas_fixture` contains the scripted terminal-canvas fixture
  used by automated tests.
- `tools/unicode_width` contains the Unicode width table generator.
- `tools/conformance` contains local setup helpers for external conformance
  tools and Unicode data.
- `tests` contains contract, model, backend, renderer, surface,
  conformance, randomized, and lifecycle tests.
- `benchmarks/embedded_terminal` contains the embedded terminal benchmark.
- `docs` contains first-read orientation and stable reference material.
- `THIRD_PARTY` and `THIRD_PARTY_NOTICES.md` record dependency and provenance
  information.

The root `CMakeLists.txt` is the best single-file build map. It declares the
library source list, resources, platform backend additions, tests, and
benchmarks.

## Build Options

The project requires CMake 3.21 or newer, C++20, and Qt 6.7 Core, Gui, and
Quick.

The atlas renderer uses Qt private modules for QRhi integration
(`Qt6::GuiPrivate` and `Qt6::QuickPrivate`) under the project Qt posture checks.
Installed binary packages are tied to the Qt major/minor used to build the
surface; source-tree consumers rebuild the surface against their own Qt.

- `BUILD_TESTING` is the standard CTest switch. It controls whether CTest tests
  are enabled at all.
- `VNM_TERMINAL_SURFACE_BUILD_TESTING` is the project-specific test gate. It is
  `ON` when the surface is the top-level project and `OFF` when it is included
  as a subproject. Test executables are configured only when both this option
  and `BUILD_TESTING` are `ON`.
- `VNM_TERMINAL_BUILD_BENCHMARKS` is `OFF` by default. It adds the embedded
  benchmark target and adds benchmark CTest validation tests when both test
  gates are enabled.
- `VNM_TERMINAL_BUILD_REQUIRED_READINESS` is `OFF` by default. It requires both
  test gates to be `ON` and also enables benchmarks and profiling validation.
- `VNM_TERMINAL_ENABLE_PROFILING` is `OFF` by default. Turning it on compiles
  profile-scope instrumentation into the library and enables profile-output
  validation paths.
- `VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY` is `OFF` by default. Turning
  it on compiles the sensitive diagnostic transcript recorder, the
  `vnm_terminal_transcript_replay` tool, and the transcript-specific tests.
- `VNM_TERMINAL_DISTRIBUTION_BUILD` is `OFF` by default. Distribution
  packaging sets it to `ON`; configure fails if transcript capture/replay is
  also enabled.
- `VNM_TERMINAL_QT_LICENSE_ROUTE` is `lgpl_dynamic` by default and also accepts
  `commercial`. The LGPL route requires shared Qt libraries.

Useful configure shapes:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake -S . -B build -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
cmake -S . -B build -DVNM_TERMINAL_BUILD_REQUIRED_READINESS=ON -DBUILD_TESTING=ON
cmake -S . -B build-profile -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON -DBUILD_TESTING=ON
cmake -S . -B build-transcript -DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON -DBUILD_TESTING=ON
```

## Supported Platforms

The native product targets are:

| Platform | Native backend scope | Backend |
| --- | --- | --- |
| Windows | x64 | ConPTY |
| Linux | x86_64 | POSIX PTY |
| macOS | Darwin builds | POSIX PTY |

Other platforms have no native backend support claim. A platform without a
native backend reports an unavailable-backend error for process launch.

## Important Targets

The root CMake project builds:

- `vnm_terminal_surface`, the static library, with alias
  `vnm_terminal_surface::vnm_terminal_surface`;
- `vnm_terminal_canvas_fixture`, the scripted child process used by backend,
  surface, and conformance tests;
- `vnm_terminal_transcript_replay`, only when
  `VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`;
- `vnm_terminal_embedded_benchmark`, when benchmarks are enabled;
- test executables named after their CTest entries when both test gates are on;
- optional conformance targets created by CMake cache variables under
  `tests/CMakeLists.txt`;
- optional `vnm_terminal_parser_libfuzzer`, when
  `VNM_TERMINAL_BUILD_LIBFUZZER=ON` is configured with Clang.

## Local Windows Build

Use an x64 MSVC Developer Command Prompt or another shell where the Visual
Studio C++ environment has already been initialized:

```bat
cmake --build build --config Release
```

Build only the surface library:

```bat
cmake --build build --target vnm_terminal_surface --config Release
```

## Test Families

CTest names are the stable way to find a test. The main families are:

- `vnm_terminal_parser_*`, `vnm_terminal_sequence_matrix`, and
  `vnm_terminal_unicode_*`: parser IR, authored parser seeds, sequence
  coverage, and Unicode width behavior.
- `vnm_terminal_screen_*`, `vnm_terminal_terminal_modes`, and
  `vnm_terminal_viewport`: screen model behavior, SGR, model operations,
  alternate screen, private modes, and viewport control.
- `vnm_terminal_backend_session`, `vnm_terminal_windows_conpty_backend`, and
  `vnm_terminal_posix_pty_backend`: session and platform backend behavior.
- `vnm_terminal_qt_*`, `vnm_terminal_render_*`,
  `vnm_terminal_qsg_*`, and `vnm_terminal_shaping_contract`: metrics, render
  snapshots, render frames, atlas QSG rendering, and shaping checks.
- `vnm_terminal_surface_host`, `vnm_terminal_input_encoder`, and
  `vnm_terminal_behavior_smoke`: public surface host behavior, input encoding,
  and behavior-smoke launches through the canvas fixture.
- `vnm_terminal_compat_smoke`: compatibility smoke behavior when the target
  exists.
- `vnm_terminal_resource_lifecycle`, `vnm_terminal_profiler`,
  `vnm_terminal_qt_posture`, and `vnm_terminal_canvas_fixture_contract`:
  lifecycle, profiling, Qt posture, and fixture protocol tests.
- `vnm_terminal_transcript`: sensitive diagnostic transcript capture/replay
  coverage, only when `VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`.
- `vnm_terminal_*_conformance`, `vnm_terminal_parser_fuzz_smoke`, and
  `vnm_terminal_libvterm_differential`: optional conformance and differential
  tests controlled by CMake cache variables.
- `vnm_terminal_embedded_benchmark_*`: readiness benchmark validation tests when
  benchmarks are enabled.

Run the configured suite:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Focused examples:

```powershell
ctest --test-dir build -C Release -R "^vnm_terminal_(parser|sequence_matrix|unicode)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(screen|terminal_modes|viewport)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(backend_session|windows_conpty_backend|posix_pty_backend)$" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(qt|render|qsg|shaping)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(surface_host|input_encoder|behavior_smoke)$" --output-on-failure
```

Default capture replay and parser fuzz smoke are added when
`tests/conformance/captures` exists:

```powershell
ctest --test-dir build -C Release -R "^(vnm_terminal_capture_replay_conformance|vnm_terminal_parser_fuzz_smoke)$" --output-on-failure
```

Single-config generators do not use `-C Release`.

## Conformance Controls

The conformance controls live in `tests/CMakeLists.txt` and are documented in
`tests/conformance/README.md`.

- `VNM_TERMINAL_CAPTURE_REPLAY_DIR` adds
  `vnm_terminal_capture_replay_conformance`.
- `VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR` adds
  `vnm_terminal_parser_fuzz_smoke`.
- `VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR` adds
  `vnm_terminal_graphics_capture_replay_conformance`.
- `VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR` adds
  `vnm_terminal_unicode_data_conformance`.
- `VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE=ON` adds the aspirational
  Unicode conformance CTest entry.
- `VNM_TERMINAL_LIBVTERM_INCLUDE_DIR`,
  `VNM_TERMINAL_LIBVTERM_LIBRARY`, and
  `VNM_TERMINAL_LIBVTERM_CORPUS_DIR` add the linked libvterm differential
  target.
- `VNM_TERMINAL_LIBVTERM_DIFF_COMMAND` and
  `VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND` add external CTest runners.

Optional strong-copyleft runners are not part of the checked-in test tree. If a
local ignored `more_tests/CMakeLists.txt` exists, the top-level build includes
it after `tests/` so normal and local-only tests can run from one CTest tree.

Semicolon-separated command variables are CMake lists. Put the executable first
and each argument in its own element.

## Benchmarks

Configure without profiling for user-visible timing and structural validation:

```powershell
cmake -S . -B build -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
cmake --build build --target vnm_terminal_embedded_benchmark --config Release
ctest --test-dir build -C Release -R "^vnm_terminal_embedded_benchmark_validate$" --output-on-failure
```

Configure a separate profiling build only for attribution and scope timing:

```powershell
cmake -S . -B build-profile -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON -DBUILD_TESTING=ON
cmake --build build-profile --target vnm_terminal_embedded_benchmark --config Release
ctest --test-dir build-profile -C Release -R "^vnm_terminal_embedded_benchmark_profile_validate$" --output-on-failure
```

The benchmark supports `--list-scenarios`, repeated `--scenario <name>`,
`--iterations`, `--warmup`, `--grid`, `--window-size`, sparse
surface-session sweep controls `--dirty-rows` and `--dirty-row-stride`, JSON
output, hierarchical profile output, `--validate-json`,
`--lazy-snapshot-evidence-mode`, and `--require-requested-grid`.
Profile flags such as `--profile`, `--profile-json`, and `--profile-text`
require a `VNM_TERMINAL_ENABLE_PROFILING=ON` build.

Benchmark JSON uses `schema_version` 24. Profile JSON uses
`profile_schema_version` 2, `time_unit` `ns`, and
`thread_semantics` `separate_thread_trees`, with separate GUI and render thread
trees. Schema 24 includes sparse dirty-row sweep metadata
`sparse_dirty_row_sweep_applicable`, `configured_sparse_dirty_rows`, and
`configured_sparse_dirty_row_stride`; per-scenario requested and actual grid
metadata through `requested_rows`, `requested_columns`, `rows`, `columns`,
`actual_grid_matches_request`, and `grid_semantics`; root and per-scenario
`lazy_snapshot_evidence_mode`; root `requested_grid_required`; and the exact
Batch 7 descriptor counter object. Sparse dirty-row validation requires the
observed frame and profile dirty-row counts to cover the requested dirty rows,
rejects full repaint, and allows at most one cursor carry-over row per measured
frame.

Schema 24 also exposes lazy snapshot evidence counters for eligible sparse
content session scenarios. The default evidence mode is
`row_view_parity_test`, which keeps the Batch 6 row-view materialization parity
check. `publication_candidate_no_materialization` exercises the same lazy
candidate composer without publishing it and without the row-view parity
materialization; it is evidence plumbing only and does not enable lazy
publication by default. The scenario-level keys are
`lazy_snapshot_exercise_applicable`,
`lazy_snapshot_exercise_promoted_non_content_rows`,
`lazy_snapshot_exercise_attempts`,
`lazy_snapshot_exercise_eligible_attempts`,
`lazy_snapshot_exercise_full_fallbacks`,
`lazy_snapshot_exercise_materialization_mismatches`,
`lazy_snapshot_exercise_dirty_rows_visible`,
`lazy_snapshot_exercise_previous_snapshot_borrowed_rows`,
`lazy_snapshot_exercise_producer_owned_rows`,
`lazy_snapshot_exercise_producer_materialized_rows`,
`lazy_snapshot_exercise_producer_cells_scanned`,
`lazy_snapshot_exercise_producer_cells_emitted`,
`lazy_snapshot_exercise_consumer_materialization_calls`,
`lazy_snapshot_exercise_consumer_materialization_rows`, and
`lazy_snapshot_exercise_consumer_materialization_cells`. Sparse content
validation declares K=0 through
`lazy_snapshot_exercise_promoted_non_content_rows`, requires zero full
fallbacks and zero materialization mismatches, and bounds producer owned rows,
producer materialized rows, and producer cells scanned by the observed dirty
rows times columns. In `row_view_parity_test` mode, consumer materialization
calls, rows, and cells must match the full visible grid parity check. In
`publication_candidate_no_materialization` mode, those consumer materialization
counters must remain zero.

The schema 24 `descriptor_counters` object has exactly
`available=true`,
`schema_semantics="batch_7_frame_qsg_descriptor_reuse"`,
numeric `frame_row_descriptors`, numeric `frame_layer_descriptors`, and
numeric `qsg_layer_descriptors`. `qsg_layer_descriptors` is zero until QSG
layer descriptor-key work is wired back in.

The schema 24 `lazy_snapshot_fallback_reason_counters` object has exactly
`available=true`,
`schema_semantics="batch_5_lazy_eligibility"`, and a `reasons`
object whose keys are `missing_previous_content_snapshot`, `grid_mismatch`,
`viewport_mismatch`, `active_buffer_mismatch`, `public_projection`,
`row_origin_generation_mismatch`, `style_color_mode_incompatibility`,
`hyperlink_namespace_incompatibility`,
`unstable_dirty_row_mutation_identity`, and
`unsupported_geometry_or_detached_snapshot_path`; every reason value is a
nonnegative integer counter.

The schema 24 `session_profile_stats` object also exposes the scalar lazy
snapshot counters `lazy_snapshot_eligibility_checks`,
`lazy_snapshot_eligible_checks`, `lazy_snapshot_full_fallbacks`,
`lazy_snapshot_dirty_rows_visible`,
`lazy_snapshot_previous_snapshot_borrowed_rows`,
`lazy_snapshot_producer_owned_rows`,
`lazy_snapshot_producer_materialized_rows`,
`lazy_snapshot_producer_cells_scanned`,
`lazy_snapshot_producer_cells_emitted`, and
`lazy_snapshot_materialization_mismatches_for_testing`. Test-only
materialization mismatches are counted only by that mismatch counter; they do
not increment explicit fallback reason counters.

The schema 24 `session_profile_stats.consumer_materialization_counters` object
has exactly `available=true`,
`schema_semantics="batch_6_materialization_boundaries"`,
`owner_batch="Batch 6"`, and numeric counters
`geometry_derived_snapshot_calls`, `geometry_derived_snapshot_rows`, and
`geometry_derived_snapshot_cells`, plus `row_view_parity_test_calls`,
`row_view_parity_test_rows`, and `row_view_parity_test_cells`. Geometry-derived
counters are produced by geometry-derived snapshot direct-output boundaries.
Row-view parity counters are produced by the opt-in Batch 6 lazy snapshot
exercise materialization boundary. Stale top-level materialization numeric
counters are rejected by exact key validation.

The `surface_session_selection_snapshot` scenario is a session snapshot contract
and validates `selection_snapshot_spans_observed`, not renderer overlay
counters. The `surface_session_resize_smoke_boundary`,
`surface_session_viewport_change_smoke_boundary`,
`surface_session_alternate_buffer_smoke_boundary`,
`surface_session_style_color_mode_smoke_boundary`, and
`surface_session_hyperlink_smoke_boundary` scenarios are Batch 1 smoke
boundaries, not lazy fallback decision runs. Schema 24 includes text coalescing
counters:
`text_coalescing_candidate_groups`, `text_coalescing_enabled_groups`,
`text_resource_runs_before_coalescing`, and
`text_resource_runs_after_coalescing`. Validation enforces
`text_coalescing_enabled_groups <= text_coalescing_candidate_groups` and
`text_resource_runs_after_coalescing <= text_resource_runs_before_coalescing`.
The public-projection boundary scenario is validated in profiling builds by
requiring nonzero `public_projection_scroll_requests` and
`public_projection_scroll_publications`. Geometry-derived direct-output
counting is validated separately by the
`surface_session_geometry_derived_boundary` scenario, which requires a
height-changing resize boundary, `geometry_derived_snapshot_calls` matching the
observed geometry boundary count, `geometry_derived_snapshot_rows` matching the
adapted output rows, and `geometry_derived_snapshot_cells` matching the adapted
output cells observed at the boundary.
On Windows, the supported readback benchmark invocation is the CTest wrapper or
an equivalent `cmake -E env ... cmd.exe /d /c call <benchmark>.exe` command
from the benchmark working directory that supplies the Qt runtime path, the
`windows` QPA platform, D3D11 RHI, and scale-factor environment. Launching the
benchmark executable directly from PowerShell without that wrapper/environment
is not a supported benchmark lane.
Readback readiness validation also requires visible pixels and atlas render
signals (`atlas_frame_observed`, `atlas_render_observed`,
`atlas_instances_observed`, `atlas_budget_valid`, and
`atlas_failures_zero`). Profiling validation waits for the atlas render-thread
sequence and requires `Qsg_atlas_render_node::prepare`,
`Qsg_atlas_render_node::prepare_atlas_instances`, and
`Qsg_atlas_render_node::render` scopes before accepting a scenario profile.

`vnm_terminal_embedded_benchmark_validate` is structural validation under the
configured Qt runtime path, not a performance comparison.
`vnm_terminal_embedded_benchmark_publication_candidate_validate` is the
profiling-off structural lane for
`publication_candidate_no_materialization`; validation requires zero consumer
materialization counters for the sparse surface-session evidence scenarios.
`vnm_terminal_embedded_benchmark_require_requested_grid_rejects_mismatch`
expects a deliberate requested-grid mismatch to fail and validates the emitted
schema fields and diagnostic.
`vnm_terminal_embedded_benchmark_profile_validate` covers the Batch 1 sparse,
selection, public projection, resize, viewport, alternate-buffer,
style/color/mode, and hyperlink smoke-boundary scenarios for schema, profile,
route-count, and scope-timing validation. It is not an interleaved A/B
performance decision run. Use no-profile Release benchmark runs for
user-visible timing. Use profiling-enabled Release runs only for attribution,
route counts, and scope timing.
`vnm_terminal_embedded_benchmark_profile_publication_candidate_validate` is the
profiling-on companion for the candidate no-materialization evidence lane.

Benchmark comparisons should record the build directory, profiling state,
renderer backend, scenario list, grid, window size, warmup count, iteration
count, command line, output JSON, and any profile artifacts.

Batch 10 lazy-publication decision evidence must use the sparse surface-session
scenarios with `--require-requested-grid` so a window-manager clamp cannot
silently substitute another grid. The full-size matrix remains
`--grid 235x873 --window-size 6984x3760 --dirty-rows 8 --dirty-row-stride 7`
for both `surface_session_sparse_ascii_output` and
`surface_session_sparse_block_graphics_output`. If the machine cannot create
that requested grid, the run must fail validation and move to a reviewed
machine requirement or matrix amendment instead of being treated as substitute
evidence. The local `48x160` CTest lanes are structural validation, not
decision-grade performance evidence.

Use the default `row_view_parity_test` mode when the run is proving row-view
equivalence. Use `--lazy-snapshot-evidence-mode` with
`publication_candidate_no_materialization` for the Batch 9/10 candidate
evidence lane; it measures the normal full-output production publication path
and then exercises the candidate lazy composer without publishing it or
materializing the row view for parity.

## CMake Consumers

Sibling source checkouts can include the surface directly:

```cmake
set(VNM_TERMINAL_SURFACE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("../vnm_terminal_surface" "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface")
target_link_libraries(my_terminal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface)
```

Installed consumers can use the exported package:

```cmake
find_package(vnm_terminal_surface CONFIG REQUIRED)
target_link_libraries(my_terminal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface)
```

The installed package exports the public host header
`include/vnm_terminal/vnm_terminal_surface.h`. Source-tree headers under
`include/vnm_terminal/internal` are implementation and test contracts, not
installed consumer API.

## Generated And Provenance Artifacts

- `tools/unicode_width/generate_unicode_width_tables.py` is network-free. With
  `--source-dir`, it consumes Unicode 16.0.0 files and emits deterministic
  generated artifacts. Without a source directory, it emits a representative
  authored table for self-tests and local smoke checks.
- The generated table artifacts are
  `include/vnm_terminal/internal/unicode_width_tables.h` and
  `src/unicode_width_tables.cpp`. The source exposes
  `unicode_width_tables_manifest_json()` with generator command, generator
  version, Unicode version, input URLs, and input hashes.
- `tools/conformance/fetch_unicode_data.ps1` and
  `tools/conformance/fetch_unicode_data.sh` create local Unicode conformance
  data directories with manifests. Unicode source data is not vendored.
- Authored terminal fixtures use provenance headers. The relevant checked-in
  file kinds are `*.vnm_fixture`, `*.vnm_capture`, and `*.vnm_seed`.
- The conformance fixture readers validate checked-in fixture provenance.
  `tests/conformance/captures` stores authored capture replay fixtures, and
  `tests/parser_randomized` stores authored parser seeds.
- `tests/conformance/README.md` defines the fixture, capture, and seed formats.
  Use it before adding or changing fixture data.

## Reference And Licensing Boundaries

The project is BSD-licensed. Dependency and reference boundaries are part of the
build contract:

- Qt Core, Gui, and Quick are allowed through either the commercial route or the
  LGPL-compatible dynamic-linking route. GPL-only Qt modules are not allowed in
  the product dependency graph.
- The renderer contract is the documented QSG atlas path, including its
  QRhi/private-Qt integration under the project Qt posture checks. Per-cell
  `QQuickItem` trees and production row/frame `QImage` text rendering remain
  outside that contract.
- `THIRD_PARTY/*.toml` records reviewed dependency metadata. Qt and Unicode data
  are not vendored dependency source trees.
- `THIRD_PARTY_NOTICES.md` records project notices for Qt and Unicode data.
- `docs/terminal_conformance_oracles.md` records which sources may establish
  terminal behavior.
- `docs/terminal_reference_inventory.md` records external terminal references,
  import decisions, and forbidden uses.
- The parser is owned in-tree. External terminal projects and parser suites may
  inform behavior questions only within the oracle and provenance rules.
- Unicode width behavior is defined by generated Unicode 16.0.0 tables and
  [Unicode width policy](unicode_width_policy.md), not by Qt shaping metrics or
  copied `wcwidth` tables.
- Strong-copyleft tools may be installed and run locally for behavior questions
  when the oracle policy allows it. Do not check in their source, test names,
  transcripts, captured output, byte streams, fixture bytes, or goldens.
- Checked-in fixtures must be independently authored or pass the documented
  provenance gate. Candidate reference projects do not authorize material import
  by themselves.

## Reading Pointers

- Build shape: `CMakeLists.txt`, `tests/CMakeLists.txt`,
  `benchmarks/CMakeLists.txt`.
- Runtime architecture: `docs/architecture.md`.
- Public component API: `docs/public_surface.md` and
  `include/vnm_terminal/vnm_terminal_surface.h`.
- Terminal behavior matrix: `docs/terminal_sequence_matrix.md`.
- Reference and licensing policy: `docs/terminal_conformance_oracles.md`,
  `docs/terminal_reference_inventory.md`, `THIRD_PARTY_NOTICES.md`, and
  `THIRD_PARTY/`.
- Fixture format: `tests/conformance/README.md`.
- Unicode width policy: `docs/unicode_width_policy.md` and
  `tools/unicode_width/generate_unicode_width_tables.py`.
- Qt rendering policy: `docs/qt_rendering_policy.md`.
