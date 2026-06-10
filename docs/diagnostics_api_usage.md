# Diagnostics And Font Metrics API Usage

This document describes how a host application consumes the public
diagnostics and font/metrics APIs without touching `internal/` headers. The
installed public header set is `vnm_terminal/vnm_terminal_surface.h`,
`vnm_terminal/font_metrics.h`, and the `vnm_terminal/diagnostics/` subtree;
`tests/package_smoke` hard-fails if anything else reaches the install tree
(see "Internal Headers And Privileged First-Party Consumers" in
`public_surface.md`).

## Stability Tiers

- Stable public API: the JSON metric serializers
  (`diagnostics/metrics_json.h`), the surface toggles and frame counters on
  `VNM_TerminalSurface`, the typed scroll-diagnostic enums, and
  `font_metrics.h`. Compiled in every build.
- Profiling-gated API: the profile-text serializers
  (`diagnostics/profile_text.h`) exist only when the surface is built with
  `VNM_TERMINAL_ENABLE_PROFILING=ON`; the declarations are compiled out
  otherwise, so a host must guard its calls with
  `#if VNM_TERMINAL_PROFILING_ENABLED`.
- Debug-only strings: `scroll_noop_cause_name()` and `scroll_action_name()`
  exist for transcript and debug rendering and their spellings preserve the
  historical transcript output. Hosts branch on the `Scroll_noop_cause` and
  `Scroll_action` enum values, never on the strings (`public_surface.md`,
  "Host Operations").

## JSON Metrics

`vnm_terminal/diagnostics/metrics_json.h` declares fill-in-place builders:

- `append_renderer_metrics_json(surface, out)` fills `out` with the renderer
  cumulative-counter metrics.
- `append_atlas_metrics_json(surface, out)` fills `out` with the QSG atlas
  frame-report metrics.

The caller owns the surrounding document and chooses the enclosing keys; the
first-party app nests these under `"renderer"` and `"qsg_atlas"` in its
runtime metrics document. The key sets and their units, scopes, and stability
classes are recorded in `diagnostics_schema.md`; the structural expectations
are pinned by the app's `metrics_json_smoke` test.

Frame counters pair with the serialized metrics for host-level framing (FPS,
frame-evidence):

- `VNM_TerminalSurface::paint_completed_frame_count()`
- `VNM_TerminalSurface::qsg_atlas_render_frame_count()`

## Profile Text

`vnm_terminal/diagnostics/profile_text.h` declares one builder per report
section (dirty-row stats and timeline, model/session profile stats, renderer
stats, cumulative renderer stats, atlas profile, slow-text-layout
diagnostics, surface geometry, render-thread profile). Each call appends
exactly the bytes of one section to a `QTextStream`, with no leading or
trailing blank line; the caller frames the document and writes the
inter-section separators. Section content is profiling data, so the whole
header body is `#if VNM_TERMINAL_PROFILING_ENABLED`.

## Diagnostic Toggles

Two runtime toggles are public methods on `VNM_TerminalSurface`:

- `set_selection_trace_enabled(bool)` writes selection diagnostics to stderr
  (the app exposes it as `--selection-trace`).
- `set_dirty_row_stats_enabled(bool)` enables the dirty-row statistics that
  feed the dirty-row sections of the profile text report.

## Font And Cell Metrics

`vnm_terminal/font_metrics.h` exposes the geometry the surface computes
internally, so a host can size windows in whole-cell multiples:

- `k_default_font_pixel_size` and `default_monospace_font_family()` mirror
  the surface defaults (the bundled framework font when it loads, otherwise
  the platform monospace family).
- `cell_metrics_for_font(family, pixel_size, device_pixel_ratio)` returns the
  per-cell advance and line height in logical pixels, snapped to the device
  pixel grid, matching the surface's internal layout computation.
- `cell_metrics_valid(metrics)` reports whether the result is usable; treat
  invalid metrics as a signal not to resize against them.

Preconditions: font resolution and metrics go through the Qt font database,
so call these after the `QGuiApplication` exists. The device pixel ratio is
an explicit argument; pass the ratio of the window or screen the grid will
render on.

## Worked Example

```cpp
// Runtime metrics document (stable, every build).
QJsonObject metrics;
QJsonObject renderer;
vnm_terminal::diagnostics::append_renderer_metrics_json(*surface, renderer);
metrics.insert("renderer", renderer);
QJsonObject atlas;
vnm_terminal::diagnostics::append_atlas_metrics_json(*surface, atlas);
metrics.insert("qsg_atlas", atlas);
metrics.insert("paint_completed_frames",
    static_cast<qint64>(surface->paint_completed_frame_count()));

// Cell-aligned window sizing.
const QString family = vnm_terminal::default_monospace_font_family();
const vnm_terminal::Cell_metrics cell = vnm_terminal::cell_metrics_for_font(
    family, vnm_terminal::k_default_font_pixel_size, window->devicePixelRatio());
if (vnm_terminal::cell_metrics_valid(cell)) {
    resize_to_grid(cell, /*columns=*/120, /*rows=*/30);
}
```

The first-party `vnm_terminal` app is the reference consumer:
`src/app_metrics.cpp` (JSON framing) and `src/app_profile_text.cpp`
(profile-text framing) contain complete production usage.
