# Change To Test Matrix

This document maps a changed subsystem to the test targets that exercise it. Use
it to choose what to run locally before pushing. The target names are the
`ctest` test names registered in the surface `tests/CMakeLists.txt` and the
application `tests/CMakeLists.txt`.

Tests run offscreen: set `QT_QPA_PLATFORM=offscreen`. `ctest` runs without the
Visual Studio developer shell; configuring and building native MSVC targets does
require it.

Run a single target with:

```text
ctest --output-on-failure -R ^vnm_terminal_backend_session$
```

Targets listed as surface targets live in this repository. Targets listed as
application targets live in the `vnm_terminal` application repository and run
against an app build that consumes this surface.

## Matrix

| If you change | Run these targets | Repository |
| --- | --- | --- |
| Backend lifecycle and signals (start, write, resize, pause, interrupt, terminate, exit) | `vnm_terminal_backend_session`, `vnm_terminal_posix_pty_backend`, `vnm_terminal_windows_conpty_backend`, `vnm_terminal_behavior_smoke` | surface |
| Native backend shared I/O helpers or output delivery limit coupling | `vnm_terminal_backend_session`, `vnm_terminal_posix_pty_backend`, `vnm_terminal_windows_conpty_backend` | surface |
| Render snapshot and frame building | `vnm_terminal_render_snapshot`, `vnm_terminal_render_frame`, `vnm_terminal_qsg_atlas`, `vnm_terminal_render_cell_text` | surface |
| Surface / host API (scroll, selection, paste, focus, clipboard) | `vnm_terminal_surface_host` | surface |
| Screen model, parser, and escape-sequence behavior | `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_screen_sgr`, `vnm_terminal_terminal_modes`, `vnm_terminal_viewport`, `vnm_terminal_sequence_matrix`, `vnm_terminal_parser_ir`, `vnm_terminal_parser_randomized` | surface |
| CLI / application behavior | `vnm_terminal_smoke`, `vnm_terminal_help_*`, `vnm_terminal_rejects_*` | application |
| Metrics and diagnostics output | `vnm_terminal_qt_metrics`, `vnm_terminal_qt_metrics_scaled`, `vnm_terminal_diagnostics_text_layout`, `vnm_terminal_diagnostics_schema_sync` | surface |
| Transcript capture/replay | `vnm_terminal_transcript` | surface |

## Notes On Specific Targets

- The POSIX and Windows backend targets are platform-gated. The surface registers
  `vnm_terminal_posix_pty_backend` only on Linux and macOS and
  `vnm_terminal_windows_conpty_backend` only on Windows. Run the one that matches
  your platform when you touch backend code; run `vnm_terminal_backend_session`
  on every platform because it exercises the session ingress and the backend
  contract through a test backend.
- `vnm_terminal_behavior_smoke` runs the native surface against the scripted
  `vnm_terminal_canvas_fixture` child process. It also registers the
  `vnm_terminal_surface_behavior_smokes` and
  `vnm_terminal_native_surface_behavior_smokes` variants from the same
  executable. The native variant is registered on Windows, Linux, and macOS.
- The parser subsystem is covered by `vnm_terminal_parser_ir` (IR contract) and
  `vnm_terminal_parser_randomized` (randomized corpus). `vnm_terminal_input_encoder`
  covers key, mouse, paste, and focus encoding and is the companion to
  `vnm_terminal_surface_host` for input changes.
- `vnm_terminal_sequence_matrix` checks the supported/ignored/rejected sequence
  tables in `docs/terminal_sequence_matrix.md` and
  `docs/terminal_conformance_oracles.md`. Update those docs when you change
  sequence handling.
- `vnm_terminal_transcript` is registered only when the build enables transcript
  capture/replay (`-DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`). The
  application's transcript-facing CLI tests (capture, rejection when disabled)
  are likewise gated on that option.
- The application `vnm_terminal_help_*` and `vnm_terminal_rejects_*` families are
  groups of individually named tests (for example
  `vnm_terminal_rejects_bad_window_size`,
  `vnm_terminal_help_mentions_text_renderer`). Run the family with a `ctest -R`
  prefix match, for example `ctest -R vnm_terminal_rejects`.

## Validation Reality

These facts describe how the change is actually validated in CI and what is left
for local runs.

- CI runs on Windows, Linux, and macOS.
- Linux and macOS run the full `ctest` suite, including the POSIX backend tests.
- Windows CI runs `vnm_terminal_windows_conpty_backend` in the normal test suite
  and in a focused AddressSanitizer job.
- Windows CI still excludes `vnm_terminal_compat_smoke`. Run it locally on
  Windows for the full native-surface fixture compatibility smoke.
- Benchmarks are not built in CI. Build with
  `-DVNM_TERMINAL_BUILD_BENCHMARKS=ON` locally to verify benchmark edits.
- The application CI checks out this surface at `ref: master`. For a cross-repo
  change, push the surface before the application so the application build sees
  the surface change.
- All tests run offscreen with `QT_QPA_PLATFORM=offscreen`.
- `ctest` works without the Visual Studio developer shell; the developer shell is
  needed only to configure and build native MSVC targets.
