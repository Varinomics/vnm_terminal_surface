# MSDF Text Renderer Handover - 2026-06-06

## Purpose

This document is a durable handover checkpoint for the MSDF text renderer work on
branch `codex/gpu-atlas-renderer` in:

- `C:\plms\varinomics\vnm_terminal_surface`
- `C:\plms\varinomics\vnm_terminal`

It was written because the user may run out of credits while the current text
quality investigation is mid-flight. The current checkpoint is intentionally a
WIP/revert boundary, not a completed quality fix.

Do not interpret the accompanying WIP commit as an accepted final state. The user
explicitly observed that the current renderer still has visible glyph alignment
problems.

## Prompt For The Next Agent

You are continuing the MSDF text renderer quality investigation in
`C:\plms\varinomics\vnm_terminal_surface`, with the app repository at
`C:\plms\varinomics\vnm_terminal`. Start by reading:

- `C:\plms\varinomics\vnm_terminal\AGENTS.md`
- `C:\plms\varinomics\vnm_terminal_surface\AGENTS.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_change_governance.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md` before
  producing or consolidating reviews.

The current code is a WIP checkpoint. It adds an opt-in MSDF terminal text
renderer and diagnostic gates, but the visible quality problem is not solved.
The user reports that `build_msdf` still shows vertically misaligned glyphs and
new left/right spacing regressions. In particular, the `l` appears one pixel too
high even though a very dim pixel touches the expected baseline, and spacing
around `i/l` and `d/f` looks too large because dim side pixels are being counted
as real geometry.

Do not continue by merely tightening the old weak-bbox tests. The current tests
are insufficient because they treat very dim MSDF fringe pixels as ink. Add or
use visible/weighted metrics, reproduce the exact perceptual failure, then make
a causal renderer change. The most likely current cause is per-glyph rounding of
the padded MSDF support rectangle, compounded by shader-side frame-origin
rounding.

Act as orchestrator if the work becomes broad. Use focused implementation or
review agents for bounded subtasks, then consolidate reviews and iterate until
review is green before claiming completion.

## Current User-Visible Problem

The literal string `build_msdf` still does not render acceptably in the MSDF
experiment terminal:

- glyphs are crisp, but not vertically aligned;
- `l` appears visually one pixel higher than neighboring letters;
- a weak/dim baseline pixel can make the old bbox metric pass while the visible
  stem still looks too high;
- horizontal spacing appears regressed, especially around `i/l` and `d/f`;
- dim MSDF fringe pixels appear to distort measured glyph gutters.

The user also noted that the earlier pre-edge-snapping version, although it had
larger vertical variation, seemed more fixable by translation only and had
better left/right geometry.

This means the current WIP direction should be treated with suspicion. The
current tests passing is not enough evidence.

## Repository State At Handover

Expected branch in both repositories:

```text
codex/gpu-atlas-renderer
```

At the time this report was written:

- `vnm_terminal` was clean.
- `vnm_terminal_surface` had the MSDF WIP changes listed below.

The intended WIP checkpoint commit is local to `vnm_terminal_surface` unless it
has been pushed later. Use `git log -1 --stat` in that repository to identify
the exact commit containing this report.

## Files In The WIP Checkpoint

The dirty/checkpointed surface files were:

```text
M CMakeLists.txt
A cmake/vnm_terminal_msdf_text_dependency.cmake
M include/vnm_terminal/internal/qsg_atlas_renderer.h
A resources/shaders/atlas_msdf_text.frag
A resources/shaders/atlas_msdf_text.frag.qsb
A resources/shaders/atlas_msdf_text.vert
A resources/shaders/atlas_msdf_text.vert.qsb
M resources/vnm_terminal_surface.qrc
M src/qsg_atlas_renderer.cpp
M tests/CMakeLists.txt
M tests/qsg_atlas/atlas_tests.cpp
A docs/msdf_text_renderer_handover_20260606.md
```

The app repository was used to build and launch the experiment app, but did not
have source changes at handover.

## What The WIP Implements

The WIP adds an opt-in MSDF/MTSDF text renderer path for printable ASCII text
runs in the terminal atlas renderer.

Important pieces:

- build option/dependency wiring for `vnm_msdf_text`;
- MSDF renderer resources and pipeline state in `src/qsg_atlas_renderer.cpp`;
- MSDF vertex and fragment shaders in `resources/shaders/atlas_msdf_text.*`;
- generated Qt shader packages (`.qsb`);
- resource registration in `resources/vnm_terminal_surface.qrc`;
- test and probe coverage in `tests/qsg_atlas/atlas_tests.cpp`;
- LCD capability probe artifacts for single `W`, repeated `W`, translated
  repeated `W`, and an ASCII panel containing `build_msdf`.

The experiment is controlled by the build option:

```text
VNM_TERMINAL_BUILD_MSDF_TEXT_RENDERER_EXPERIMENT=ON
```

The MSDF renderer should remain opt-in until the quality issue is resolved and
reviewed.

## Current Geometry Policy In The WIP

At handover, `append_msdf_text_instance` in
`src/qsg_atlas_renderer.cpp` snaps the terminal baseline origin to integer
physical pixels, then rounds the top-left of each glyph support rectangle:

```cpp
const int physical_origin_x = atlas_snapped_physical_int(...);
const int physical_origin_y = atlas_snapped_physical_int(...);

const qreal physical_left = std::lround(
    physical_origin_x + glyph.plane_left);
const qreal physical_top = std::lround(
    physical_origin_y + glyph.plane_bottom);

const qreal physical_width =
    glyph.plane_right - glyph.plane_left;
const qreal physical_height =
    glyph.plane_top - glyph.plane_bottom;
```

The shader currently rounds the projected frame origin but preserves the
unrounded frame size:

```glsl
vec2 frame_origin = round(fragment_frame_rect.xy);
vec2 frame_size = max(vec2(1.0), fragment_frame_rect.zw);
vec2 glyph_pixel = gl_FragCoord.xy - frame_origin;
```

This policy passed the current tests, but the user still sees visible failure.
It is therefore probably the wrong policy or at least not sufficient.

## Why The Current Policy Is Suspect

The MSDF `glyph.plane_*` rectangle is not the visible ink box. It is a padded
distance-field support rectangle. In `C:\plms\bsd_licensed\vnm_msdf_text`, the
plane bounds include the MSDF pixel range/fringe:

```cpp
glyph.plane_left =
    bounds.l * draw_scale - options.atlas_px_range * screen_to_atlas_ratio;
glyph.plane_right =
    bounds.r * draw_scale + options.atlas_px_range * screen_to_atlas_ratio;
glyph.plane_top =
    -bounds.b * draw_scale + options.atlas_px_range * screen_to_atlas_ratio;
glyph.plane_bottom =
    -bounds.t * draw_scale - options.atlas_px_range * screen_to_atlas_ratio;
```

Rounding `origin + glyph.plane_left` and `origin + glyph.plane_bottom` per glyph
therefore translates each glyph according to the fractional part of its padded
support rectangle, not according to the terminal cell or the visible glyph ink.

That matches the observed failure:

- different glyphs can get different tiny y translations;
- left/right perceived spacing can change because side fringe affects the
  rounded support origin;
- a glyph can have a very dim pixel on the expected baseline while the visible
  stem is still one pixel high.

The shader then rounds the projected support-frame origin again, which ties the
MSDF sampling phase to that per-glyph support rectangle.

## Important Agent Findings Already Received

Two late review/investigation agents reported the following.

### Renderer Geometry Finding

The current fix is likely snapping the wrong thing:

- `qsg_atlas_renderer.cpp` snaps the cell/baseline origin, then rounds
  `origin + glyph.plane_left` and `origin + glyph.plane_bottom` per glyph.
- `glyph.plane_*` values include distance-field padding/fringe.
- every glyph can receive a distinct extra x/y translation based on the
  fractional support box;
- shader frame-origin rounding compounds the issue.

Recommended direction from that agent:

- back out per-glyph support-edge snapping;
- snap only the terminal cell/baseline origin;
- add `glyph.plane_left/bottom/right/top` unchanged;
- remove shader frame-origin rounding, or replace it with a single snap delta
  derived from the cell/baseline origin, not the glyph support rectangle;
- avoid per-glyph correction tables or blur/sharpness tweaks until geometry is
  causally understood.

### Test Metric Finding

The current tests are too weak because they define ink using a low pixel-delta
threshold and then build bbox/first-ink metrics from that binary mask.

Observed weaknesses:

- very dim MSDF fringe pixels count as ink;
- weak bboxes can say a glyph touches the baseline even when the visible stem is
  one pixel high;
- weak side pixels can make horizontal gutters appear correct in the metric
  while the visible glyph spacing looks wrong;
- the ASCII-panel raw MSDF reference catches GPU-vs-CPU disagreement but cannot
  catch geometry mistakes shared by both MSDF paths;
- the quality oracle should include a Qt text-node reference for visible
  baselines, stem extents, and gutters.

Recommended metric additions:

- normalized pixel energy:

  ```text
  pixel_delta(pixel, background) / pixel_delta(foreground, background)
  ```

- strong-pixel threshold around 0.45-0.55 foreground energy;
- row and column energy sums;
- visible baseline row;
- visible top row;
- visible left/right columns;
- energy-weighted centroid;
- visible gutters for adjacent pairs such as `i/l`, `d/f`, `u/i`, and `l/d`;
- targeted `build_msdf` gates that compare visible metrics to a Qt text
  reference, or at least diagnose outliers.

Keep weak-mask metrics only as diagnostics for "nothing disappeared"; do not use
them as the primary quality gate for alignment.

## Experiments To Run Next

Run these as small, reversible experiments. Do not let the passing current tests
convince you the visual issue is fixed.

### 1. Dump Support Rounding Deltas For `build_msdf`

For each glyph in `build_msdf`, dump:

- codepoint and glyph index;
- `glyph.plane_left`;
- `glyph.plane_right`;
- `glyph.plane_top`;
- `glyph.plane_bottom`;
- `round(origin_x + plane_left) - (origin_x + plane_left)`;
- `round(origin_y + plane_bottom) - (origin_y + plane_bottom)`;
- visible baseline/top/left/right metrics once those exist.

Check whether `l`, `_`, `f`, `i`, and the two `d` glyphs correlate with the
reported visual jumps and gutters.

### 2. A/B Geometry Policies

Compare at least these variants:

1. Current WIP:
   - cell/baseline origin snap;
   - per-glyph support top-left rounding;
   - shader frame-origin rounding.

2. Origin snap only:
   - snap terminal cell/baseline origin;
   - add `glyph.plane_*` unchanged;
   - no shader frame-origin rounding.

3. Single snap delta:
   - compute one screen-space snap delta for the terminal cell/baseline origin;
   - apply the same delta to all four glyph vertices;
   - sample from unrounded glyph-local coordinates or equivalent stable
     frame coordinates.

The expected best causal direction is variant 2 or 3, not per-glyph support
edge rounding.

### 3. Add Visible Metrics Before Tightening Gates

Add a visible/weighted metrics structure beside the existing weak metrics in
`tests/qsg_atlas/atlas_tests.cpp`.

Suggested fields:

```text
has_visible_ink
visible_left_x
visible_right_x
visible_top_y
visible_baseline_y
visible_width
visible_height
weighted_center_x
weighted_center_y
row_energy[]
column_energy[]
strong_pixel_count
energy_sum
```

Suggested thresholding:

- `weak` threshold remains useful for disappearance/fringe diagnostics;
- `strong_pixel` should be around 0.45-0.55 foreground-equivalent energy;
- row/column support can be based on either at least one strong pixel or energy
  at least one foreground-equivalent pixel, whichever proves more stable.

Do not choose thresholds solely to pass the current artifact. First print the
metrics for MSDF, raw atlas, and Qt text references and inspect whether they
explain the screenshot.

### 4. Add A Qt Quality Reference For The ASCII Panel

The probe already has Qt text reference machinery for other fixtures. Add an
ASCII-panel Qt text-node reference for the `build_msdf` quality checks.

The raw MSDF reference is still valuable for GPU-vs-CPU parity, but it is not an
independent oracle for glyph quality because it shares MSDF geometry mistakes.

### 5. Gate Targeted Visible Conditions

Candidate gates after metrics are validated:

- non-descender letters in `build_msdf` should have visible baseline rows
  aligned, unless Qt reference shows the same spread;
- the repeated `d` glyphs should match visible baseline, visible left/right, and
  weighted centroid exactly or nearly exactly;
- `i/l`, `d/f`, `u/i`, and `l/d` visible gutters should not be inflated relative
  to the Qt reference beyond a deliberately chosen tolerance;
- same-origin repeated `W` stability should remain exact or near-exact;
- translated repeated `W` should still reject ink-mask and geometry drift.

## Known Current Test Behavior

The current WIP passed these gates before this report was written:

```powershell
git -C C:\plms\varinomics\vnm_terminal_surface diff --check

cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_batch2_experiment_ninja --target vnm_terminal_qsg_atlas'

ctest --test-dir build_batch2_experiment_ninja -R "^vnm_terminal_qsg_atlas_lcd_capability_probe_d3d11$" --output-on-failure

ctest --test-dir build_batch2_experiment_ninja -R "^vnm_terminal_qsg_atlas$" --output-on-failure

cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_batch2_slice_a_ninja --target vnm_terminal_qsg_atlas && ctest --test-dir build_batch2_slice_a_ninja -R "^vnm_terminal_qsg_atlas$" --output-on-failure'
```

Those passes are not evidence that the visible problem is solved.

Important metric values from the current successful probe included:

```text
build_msdf:
  compared = 10
  max_top_delta = 1
  max_bottom_delta = 1
  max_center_y_delta = 0.5
  signed_center_y_delta_range = 0.5
  signed_top_delta_range = 1
  signed_bottom_delta_range = 1

translated repeated-W origin match:
  diff_pixels = 411
  max_delta = 1
  ink_mask_diff_pixels = 0
  max_ink_pixels_delta = 0
  max_relative_bbox_y_delta = 0
  max_relative_bbox_height_delta = 0

translated repeated-W stability:
  diff_pixels = 3
  max_delta = 1
  ink_mask_diff_pixels = 0
```

The `max_delta <= 1` translated repeated-W relaxation was independently
reviewed as acceptable only because ink masks and relative geometry remain
exact. Do not relax it further without adding a cap on the number of drifting
pixels.

## Probe Artifacts

Current probe artifacts were in:

```text
C:\plms\varinomics\vnm_terminal_surface\build_batch2_experiment_ninja\tests\lcd_atlas_probe_d3d11
```

Useful files:

```text
lcd_capability_probe_metadata.json
lcd_capability_probe_ascii_panel_atlas.png
lcd_capability_probe_ascii_panel_raw_atlas_reference.png
lcd_capability_probe_qt_text_reference.png
lcd_capability_probe_repeated_w_atlas.png
lcd_capability_probe_single_w_atlas.png
lcd_capability_probe_single_w_msdf.png
lcd_capability_probe_single_w_qt_text_reference.png
lcd_capability_probe_single_w_raw_atlas_reference.png
lcd_capability_probe_translated_repeated_w_atlas.png
```

Do not rely only on these existing artifacts after code changes; regenerate
them.

## Building And Launching The Experiment App

Build the terminal app from `C:\plms\varinomics\vnm_terminal`:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_msdf_experiment_ninja --target vnm_terminal'
```

If link fails with:

```text
LINK : fatal error LNK1104: cannot open file 'vnm_terminal.exe'
```

then an experiment instance is probably still running. Identify only the process
from:

```text
C:\plms\varinomics\vnm_terminal\build_msdf_experiment_ninja\vnm_terminal.exe
```

Do not close the installed `dist\...` terminals unless explicitly asked. The
user has other terminals running.

Launch and foreground a fresh experiment terminal:

```powershell
$exe = 'C:\plms\varinomics\vnm_terminal\build_msdf_experiment_ninja\vnm_terminal.exe'
$env:PATH = 'C:\Qt\6.10.1\msvc2022_64\bin;' + $env:PATH
$process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class VnmTerminalForeground {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
'@
for ($i = 0; $i -lt 30; ++$i) {
    Start-Sleep -Milliseconds 200
    $process.Refresh()
    if ($process.MainWindowHandle -ne 0) { break }
}
if ($process.MainWindowHandle -ne 0) {
    [VnmTerminalForeground]::ShowWindow($process.MainWindowHandle, 9) | Out-Null
    [VnmTerminalForeground]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
    "launched pid=$($process.Id) hwnd=$($process.MainWindowHandle)"
}
else {
    "launched pid=$($process.Id) hwnd=0"
}
```

Last launched experiment PID before the handover report was:

```text
40084
```

Verify the executable path before closing any process.

## Regenerating Shader Packages

When editing MSDF shaders, regenerate the Qt shader packages:

```powershell
C:\Qt\6.10.1\msvc2022_64\bin\qsb.exe --qt6 -o resources\shaders\atlas_msdf_text.vert.qsb resources\shaders\atlas_msdf_text.vert
C:\Qt\6.10.1\msvc2022_64\bin\qsb.exe --qt6 -o resources\shaders\atlas_msdf_text.frag.qsb resources\shaders\atlas_msdf_text.frag
```

Commit `.frag/.vert` and `.qsb` together.

## Current Review Status

Earlier review agents marked the current WIP green, but that was before the
user supplied the latest screenshot and clarified that the metric was blind to
visible dim-pixel failures. Treat those green reviews as obsolete for the
current quality question.

Late agents identified the current likely issue:

- renderer review: per-glyph support-rectangle rounding is probably the wrong
  geometry policy;
- test review: weak binary ink masks are not a valid primary quality metric for
  MSDF visible alignment.

Before claiming this batch fixed, ask independent review agents to review both:

- the renderer geometry/sampling policy;
- the probe metric contract and new gates.

Round-trip until those reviews are green.

## Suggested Next Patch Order

1. Add diagnostic visible metrics and JSON/artifact output for `build_msdf`,
   without changing renderer behavior.
2. Use those metrics to reproduce the user's visual complaint objectively.
3. Run a small geometry-policy A/B patch:
   - first remove per-glyph support-edge rounding;
   - test with no shader frame-origin rounding;
   - if translation stability regresses, test a single cell-origin snap delta
     applied uniformly to glyph geometry/sampling.
4. Compare MSDF vs Qt text reference on visible baselines, stems, and gutters.
5. Keep the smallest causal renderer fix.
6. Regenerate `.qsb`.
7. Run focused tests and launch the terminal for user inspection.
8. Ask review agents to review.
9. Only after user accepts the visual result, clean up obsolete diagnostic code
   or make useful metrics part of the permanent probe contract.

## What Not To Do

- Do not blur the glyphs to hide pixel steps.
- Do not add hand-authored per-glyph correction tables.
- Do not tune thresholds only until the current test passes.
- Do not treat weak alpha/fringe pixels as the baseline or side bearing.
- Do not relax repeated-W stability further without a stronger drift-count gate.
- Do not close unrelated `dist` terminal processes.
- Do not commit scratch review notes except this explicitly requested durable
  handover/checkpoint report.

## Revert Guidance

The WIP checkpoint is deliberately commit-shaped so it can be reverted cleanly.
If this direction proves wrong or blocks progress, revert the checkpoint commit
that contains this file from `vnm_terminal_surface`.

Because this is opt-in experiment code, reverting the WIP checkpoint should not
affect the default non-MSDF renderer path. Still run the default atlas test after
any revert:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build_batch2_slice_a_ninja --target vnm_terminal_qsg_atlas && ctest --test-dir build_batch2_slice_a_ninja -R "^vnm_terminal_qsg_atlas$" --output-on-failure'
```

