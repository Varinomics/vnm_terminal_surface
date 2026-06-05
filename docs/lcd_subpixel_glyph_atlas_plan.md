# LCD/Subpixel Glyph Atlas Plan

## Objective

Deliver text quality comparable to Qt's text renderer while keeping the
canonical QRhi atlas renderer as the terminal text renderer.

The production renderer must draw shaped terminal text through atlas glyph
instances for every glyph it can shape and rasterize through Qt. The atlas is
not an ASCII-only route, and it is not a split renderer where normal non-ASCII
text is handed back to `QSGTextNode`.

The target runtime is windowed hardware rendering, primarily D3D11 through Qt
RHI. Other RHI backends may be kept passing where the repository already tests
them, but they do not define the quality target for this change.

## Non-Negotiable Contract

- `Terminal_render_frame::text_runs` and `cursor_text_runs` remain the renderer
  text input.
- Qt remains the shaping, fallback-font, and glyph-rasterization authority.
  The atlas renderer owns placement, cache ownership, uploads, and drawing.
- All shaped glyphs use one atlas text path in production.
- Printable ASCII, non-ASCII BMP glyphs, CJK, combining clusters after shaping,
  fallback-font glyphs, color glyph images, Braille, and Powerline/Nerd-font
  symbols are handled by the atlas text path. Grid-defined terminal graphics
  (box drawing and block/shade elements) use frame graphic primitives so they
  stay cell-aligned and continuous.
- A broad glyph set is warmed for each font epoch, zoom level, and device pixel
  ratio. Glyphs outside that set are inserted lazily through the same cache.
- `QSGTextNode` is allowed only as a test/reference renderer, not as a production
  text fallback.
- Missing atlas capacity, unsupported glyph image formats, or impossible render
  state are reported explicitly through frame reports/tests. A reported miss is
  not an acceptable downgrade for a glyph that Qt can shape and rasterize.
- LCD/subpixel masks are used when Qt supplies them. Grayscale masks and color
  glyph images remain atlas entries with explicit coverage kinds rather than
  escaping the atlas renderer.
- Companion `vnm_terminal` report/profile consumers are updated in the same
  batch that changes any public atlas report field.

## Producer Contract

Batch 3 makes Qt-shaped glyph records the production glyph producer. Printable
ASCII enters the same shaped-glyph record, cache, rasterization, and
draw-builder path as every other glyph. Public counters expose route-neutral
shaped text runs and shaped glyph records rather than direct-ASCII versus
Qt-layout ownership.

The frame builder routes supported box drawing and block/shade elements into
terminal graphic geometry before they become text runs. That ownership remains
the production path for grid-defined symbols; Braille and other font-defined
symbols are shaped and drawn by the atlas text path.

### Batch 5P Producer Reuse

The Batch 5 performance repair keeps the same quality contract and adds reuse
inside the canonical atlas producer. The renderer may cache prepared text work
by retained line/run identity, but reused work still emits through the same
RGBA/LCD atlas glyph-instance path. Reuse stores shaped records, resolved
presentation, and resolved atlas slot identity; each frame reapplies the current
row, logical row, rectangle, clip, color, opacity, and pass state before
emitting instances.

Producer counters live under `qsg_atlas.producer` and report shaped work built
versus reused, shape-cache hits/misses/prunes, presentation scans, slot
resolutions, and reserved simple-path counters. Legacy `renderer.text_content_*`
counters remain compatibility shims and are not atlas producer reuse evidence.

Cache invalidation is keyed by retained content identity plus shape-affecting
state: guarded text identity, run column/span, cursor-text ownership, font epoch,
font identity, physical pixel size, device pixel ratio, and cell metrics.
Entries not seen in the current visible frame are pruned. If a future dirty
simple-text fast path is needed, it must stay inside this producer, emit normal
atlas glyph instances, and fall back to `QTextLayout` for ambiguous or complex
input.

## Renderer Model

### Complexity Discipline

This work deliberately makes the atlas renderer more capable. It must not become
a pile of one-off glyph special cases. New behavior is added through shared
producers and explicit data kinds:

- shaping goes through shaped glyph records;
- rasterized images are classified into coverage kinds;
- cache keys describe the full glyph identity;
- upload and draw paths consume coverage kinds, not Unicode-family branches;
- failures surface as reported misses with cause and ownership, not silent
  alternate render paths.

If an implementation change needs a special case, the owning batch must either
turn it into a named coverage kind/shared policy or amend the plan with
independent review. Ad hoc branches for ASCII, CJK, color emoji, box drawing,
or a specific font are not acceptable production architecture.

### Shaped Glyph Records

The renderer creates durable shaped glyph records before rasterization. A record
contains:

- fallback face id;
- glyph index;
- physical pixel size;
- device pixel ratio;
- font epoch;
- glyph position from Qt shaping;
- source string index/range from `QGlyphRun::stringIndexes()`;
- owning terminal row, cell/span, and text run;
- coverage kind after rasterization;
- LCD orientation/order;
- subpixel phase when the snapped grid proves a non-zero phase exists.

For a correctly snapped monospace terminal grid, all normal cell origins should
land on a stable physical-pixel phase. The implementation still records the
phase in the key so fractional regressions are observable instead of silently
collapsing different masks into one entry.

### Coverage Kinds

The atlas stores explicit glyph tile kinds:

- LCD RGB/BGR coverage masks tinted by the terminal foreground color;
- grayscale coverage masks stored in the same atlas format and tinted by the
  terminal foreground color;
- color glyph images composited from atlas RGBA without foreground tinting,
  while preserving opacity and cursor/selection ordering.

Coverage kind is not inferred from `QImage::Format` alone. The classifier uses
the requested presentation, Qt glyph image semantics, image format, alpha/color
content, and sample probes. An ambiguous image is a reported blocker until the
classifier or plan is amended with independent review.

### Texture Format And Upload

The glyph atlas moves from `R8` to an RGBA-capable format. Texture uploads,
cache page storage, stats, tests, and shader resources move with it in the same
batch that changes the format.

Qt image bytes are normalized deliberately before QRhi upload. The implementation
must not rely on QRhi to convert source formats or on platform byte order to make
`Format_RGB32`, `Format_ARGB32_Premultiplied`, or `Format_RGBA8888` equivalent.
Tests verify channel order, alpha, premultiplication, stride, and GPU readback.

Production page budget may exceed one only when the GPU shader/upload path
addresses pages explicitly. Multi-page cache ownership, texture-array upload,
and shader page selection are part of the same production-visible change.

### LCD Compositing

The D3D11 target uses a real LCD compositing model. The preferred model is
dual-source blending with a destination factor derived from the sampled LCD
coverage. Batch 1 probes the required QRhi/D3D11 capability and shader-package
support. If that capability is unavailable, the plan must be amended in Batch 1
with a reviewed replacement compositing model before production LCD rendering
work opens.

LCD gates cover default backgrounds, colored backgrounds, selection, cursor
inverse text, and opacity/faint text. A gate that only detects colored edge
pixels is not sufficient.

### Sampling And Placement

The text sampler is nearest-neighbor for snapped 1:1 glyph quads. Linear
sampling is not acceptable for terminal text because it smears already
rasterized glyph coverage when the quad is even slightly off-grid.

Glyph placement is computed in physical pixels first, then converted back to
logical coordinates for the QRhi draw. The code records enough diagnostics to
prove that:

- cell metrics are snapped to device pixels;
- glyph origins are snapped to physical pixels;
- atlas bitmap dimensions match the submitted glyph quad dimensions;
- atlas UVs sample texel centers with no gutter bleed;
- cursor inverse text and clipped runs use the same snapping policy as main
  text.

Any batch that changes placement must rerun the dense-grid spacing test and the
manual visual check before it closes.

## Warm Set And Lazy Insertion

Each font epoch builds an atlas warm set for the active font, zoom level, and
device pixel ratio. The warm set is shaped through Qt so fallback fonts and
glyph indexes are discovered the same way production text discovers them.

The seed set includes scalar ranges and explicit shaped cluster probes:

- printable ASCII and other common control-adjacent visible symbols;
- Latin-1 Supplement and Latin Extended-A ranges commonly used in prompts;
- Greek and Cyrillic ranges;
- arrows, mathematical operators, miscellaneous technical symbols, geometric
  shapes, dingbats used in CLIs, and currency symbols;
- box drawing, block elements, sextants, quadrants, Braille, and shade blocks;
- Powerline and common Nerd-font private-use symbols;
- a bounded representative CJK seed for startup coverage;
- combining-mark probes such as `e` plus combining acute;
- emoji presentation selector, keycap, skin-tone, and ZWJ cluster probes;
- common color glyph probes to validate color-image atlas handling.

Full Unicode prewarming is not practical. Completeness comes from lazy insertion:
every shaped glyph outside the warmed set is inserted into the same atlas cache
before the frame is reported complete. Lazy insertion is not a separate renderer
path.

The warm set is a source-controlled table or generator with tests. Memory,
startup cost, lazy-insert frame impact, and page pressure are measured for the
target font/DPR/zoom combinations. If the seed set exceeds the accepted budget,
the plan is amended by changing the seed set, not by narrowing production
rendering to ASCII.

## Hardware Evidence And Artifacts

Every visual/performance artifact records:

- Qt version;
- font family, style, pixel size, zoom level, and fallback faces observed;
- device pixel ratio;
- window size and rendered image size;
- `QSG_RHI_BACKEND`;
- Qt graphics API reported by the window;
- adapter name/LUID when QRhi exposes it;
- whether the scene graph reports a software renderer;
- build type and profiling state;
- atlas page size, page count, memory use, miss counts, and snapped-origin
  failures.

Performance gates use Release, no profiling, windowed hardware rendering. The
orchestrator must notify the user before performance tests and run them only
after the user confirms the machine is otherwise idle.

## Batches

### Batch 1 - Quality Harness, Capability Probe, And Baseline

Purpose:

- establish the target visual reference and Qt glyph image capabilities on the
  Windows D3D11 hardware path;
- archive a comparable performance and memory baseline before production-cost
  changes;
- add durable tests/diagnostics without changing production text output.

Primary files:

- `tests/qsg_atlas/atlas_tests.cpp`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `src/qsg_atlas_renderer.cpp`;
- `resources/shaders/atlas_glyph.frag`;
- `resources/shaders/atlas_glyph.vert`;
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`;
- `C:\plms\varinomics\vnm_terminal\tests\expect_metrics_json.cmake`;
- `C:\plms\varinomics\vnm_terminal\benchmarks\cmdg_nelostie\run_canonical_atlas_cmdg_gate.ps1`.

Work:

- add a hardware visual fixture that renders `Process:`, dense ASCII, box
  drawing, Braille, CJK, combining text, color glyph samples, and cursor/selection
  variants;
- render the same fixture through a Qt text reference path used only by tests;
- probe `QRawFont::alphaMapForGlyph(..., QRawFont::SubPixelAntialiasing)` for
  every sample family and record returned image formats, sizes, coverage-kind
  candidates, and then-current production miss/color-image diagnostic counters;
- probe D3D11/QRhi dual-source blend and shader-package support;
- add frame/report fields for coverage-kind counts, glyph misses, page pressure,
  snapped-origin failures, sampler mode, and capability-probe output;
- update companion `vnm_terminal` report/profile/JSON consumers in the same
  batch if any public report field changes;
- archive baseline D3D11 artifacts for FPS, frame time, atlas memory, glyph
  misses, page pressure, frame completion, startup latency, visible first-frame
  completion, and current cold glyph insertion/frame impact equivalents.

Acceptance gates:

- hardware D3D11 visual fixture produces non-empty reference and atlas images
  with artifact metadata;
- capability probe output confirms which sample families produce LCD, grayscale,
  color, ambiguous, or missed images on this workstation;
- color glyph color-image behavior is recorded as diagnostic state, not
  accepted as the future compositing contract;
- dense-grid spacing test remains passing;
- baseline performance, memory, startup, first-frame, and cold-insert artifacts
  are archived before production-cost batches open;
- production text route is unchanged in this batch.

Gate commands:

- build `vnm_terminal_qsg_atlas` in Release with profiling disabled;
- run the D3D11 atlas smoke, dense-grid, pixel-parity, layout-parity, and report
  CTests selected by the batch's CTest names;
- build `vnm_terminal` Release/no-profiling if companion report fields changed;
- run CMDG baseline only after user idle confirmation.

### Batch 2 - RGBA Tile Model Preparation

Purpose:

- prepare coverage-kind-aware RGBA tile ownership while keeping the production
  atlas page format, upload path, and then-current single-channel report fields
  unchanged.

Primary files:

- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `src/qsg_atlas_renderer.cpp`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- `docs/gpu_atlas_renderer_plan.md`;
- `docs/qt_rendering_policy.md`;
- companion `vnm_terminal` report/profile/test files if report fields change.

Work:

- introduce an RGBA-capable glyph tile model carrying kind, pixel size, stride,
  normalized byte layout, and diagnostic source format;
- update `Glyph_atlas_cache_key` with coverage kind, LCD order, and subpixel
  phase in preparation for the production cache migration;
- add packer/cache accounting helpers for RGBA bytes, page use, misses, and
  failed inserts;
- keep production `max_pages == 1` and the then-current single-channel
  upload/report path unchanged until GPU page addressing and RGBA upload land
  together.

Acceptance gates:

- unit tests cover LCD, grayscale, color-image, and ambiguous tile conversion;
- cache-key tests prove glyphs that differ by face, size, phase, or coverage
  kind do not alias;
- production rendering remains on the existing single-channel page format and
  report fields;
- no companion `vnm_terminal` report migration is required unless this batch adds
  public diagnostic fields.

### Batch 3 - Unified Shaped Glyph Producer

Purpose:

- make Qt-shaped glyph records the only production glyph producer.

Primary files:

- `src/qsg_atlas_renderer.cpp`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- `docs/gpu_atlas_renderer_plan.md`;
- `docs/qt_rendering_policy.md`;
- companion `vnm_terminal` report/profile/test files for counter changes.

Work:

- request `QTextLayout::RetrieveGlyphIndexes`, `RetrieveGlyphPositions`, and
  `RetrieveStringIndexes` for every text run;
- build shaped glyph records with source string range, owning terminal cell/span,
  fallback face id, glyph id, and position;
- delete or rework `Atlas_ascii_glyph_cache`, `append_direct_ascii_text_run`,
  `atlas_direct_ascii_run_candidate`, and direct-ASCII public counters so
  printable ASCII enters the same producer as every other glyph;
- replace `direct_ascii_*` and `qt_layout_*` public split counters with
  canonical shaped-glyph counters;
- update docs and tests that describe direct printable-ASCII as a separate
  production producer in the same batch;
- add tests for printable ASCII, `e` plus combining acute, CJK, VS16 emoji, and
  ZWJ emoji records;
- update dense-grid tests that currently assert direct-ASCII instances.

Acceptance gates:

- no production text glyph reaches rasterization without a shaped glyph record;
- tests prove string-index/cluster ownership for combining, CJK, VS16, and ZWJ
  samples;
- no production `QSGTextNode` fallback is introduced;
- no public report field names expose direct-ASCII versus Qt-layout ownership;
- companion `vnm_terminal` report/profile/metrics tests pass.

### Batch 4 - Coverage Classification And RGBA Packing Preparation

Purpose:

- prepare Qt glyph-image classification and RGBA packing without changing the
  production atlas page format.

Primary files:

- `src/qsg_atlas_renderer.cpp`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- `docs/gpu_atlas_renderer_plan.md`;
- companion `vnm_terminal` report/profile/test files if private diagnostics are
  surfaced publicly.

Work:

- request subpixel antialiasing for classifier/probe rasterization;
- classify glyph images into LCD mask, grayscale mask, color image, or ambiguous
  using the coverage-kind decision, not `QImage::Format` alone;
- manually normalize source images into the atlas RGBA byte layout;
- replicate grayscale masks into the RGBA atlas representation;
- convert color glyph images into the atlas color-image kind without foreground
  tinting;
- preserve source-format diagnostics for ambiguous or unsupported images;
- add CPU tests for channel order, alpha, premultiplication, stride, and
  representative source formats;
- keep production glyph rendering on the existing page format until Batch 5
  switches upload, shader resources, reports, and companion consumers together.

Acceptance gates:

- LCD text glyphs and color emoji/VS16 glyphs are accepted without tint/color
  confusion in the classifier tests;
- unsupported or ambiguous formats fail classifier checks with source format and
  glyph identity;
- production rendering remains valid on the existing page format;
- no production `QSGTextNode` fallback is introduced.

### Batch 5 - Production RGBA Atlas, Upload, LCD Compositing, And Multi-Page Rendering

Purpose:

- draw all coverage kinds from the RGBA atlas with correct blending and page
  addressing.

Primary files:

- `resources/shaders/atlas_glyph.frag`;
- `resources/shaders/atlas_glyph.vert`;
- `resources/shaders/atlas_glyph.frag.qsb`;
- `resources/shaders/atlas_glyph.vert.qsb`;
- `resources/shaders/atlas_dual_source_probe.frag`;
- `resources/shaders/atlas_dual_source_probe.frag.qsb`;
- `resources/vnm_terminal_surface.qrc` if shader aliases change;
- `src/qsg_atlas_renderer.cpp`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- `docs/gpu_atlas_renderer_plan.md`;
- `docs/qt_rendering_policy.md`;
- companion `vnm_terminal` CMDG/report validators if report fields change.

Work:

- switch the production glyph atlas page format from R8 to RGBA;
- migrate texture uploads, cache page storage, stats, tests, shader resources,
  report fields, and companion consumers in the same batch;
- use the Batch 4 coverage classifier and RGBA packing path for production glyph
  rasterization;
- delete color-alpha demotion behavior and counters that describe demotion as
  acceptable;
- update docs and tests that describe R8 upload or color-alpha demotion in the
  same batch;
- replace the single-channel glyph shader with a coverage-kind-aware shader;
- implement the D3D11 LCD compositing model chosen from Batch 1 evidence;
- apply replicated coverage for grayscale masks;
- composite color glyph images without foreground tint;
- add page index/kind data to `atlas_glyph_instance_t` and the glyph buffer key;
- implement page-addressed upload and draw logic in `upload_coverage_texture`,
  shader resources, and `draw_glyph_pass`;
- enable production multi-page budget only after page addressing works;
- replace linear glyph sampling with nearest sampling for snapped text;
- regenerate checked-in `.qsb` shader packages with:
  `C:\Qt\6.10.1\msvc2022_64\bin\qsb.exe --qt6 -o resources\shaders\atlas_glyph.vert.qsb resources\shaders\atlas_glyph.vert`;
- regenerate checked-in `.qsb` shader packages with:
  `C:\Qt\6.10.1\msvc2022_64\bin\qsb.exe --qt6 -o resources\shaders\atlas_glyph.frag.qsb resources\shaders\atlas_glyph.frag`;
- regenerate checked-in `.qsb` shader packages with:
  `C:\Qt\6.10.1\msvc2022_64\bin\qsb.exe --qt6 -o resources\shaders\atlas_dual_source_probe.frag.qsb resources\shaders\atlas_dual_source_probe.frag`.

Acceptance gates:

- no `R8`-specific public report name remains in the canonical atlas API or
  companion app consumers;
- `rg "\bR8\b|r8_|color_glyph_alpha_demotions|demot"` finds only references that
  explicitly describe removed behavior, historical plan context, or test probes
  for current batch validation;
- sample glyph families from Batch 1 produce atlas entries with zero unexplained
  misses;
- any nonzero miss includes glyph identity, source string range, Qt source
  format/capability, capacity or render-state cause, and an owner-reviewed
  impossibility proof or plan amendment;
- color glyph samples no longer increment a demotion counter;
- GPU readback verifies channel order, alpha, premultiplication, stride, and
  representative source formats after upload;
- shader packages are regenerated from source and embedded resources load;
- hardware D3D11 tests prove non-empty text output for each coverage kind;
- visual fixture validates LCD compositing over default background, colored
  backgrounds, selection, cursor inverse text, and faint/opacity cases;
- dense-grid spacing remains stable;
- frame report confirms nearest glyph sampling and page-addressed rendering;
- `vnm_terminal` builds and CMDG/report validators pass if report fields changed.

### Batch 6 - Physical-Pixel Placement And Grid Graphics Quality

Purpose:

- remove the remaining blur source while preserving grid-aligned primitives for
  box drawing and block/shade elements.

Primary files:

- `src/qsg_atlas_renderer.cpp`;
- `src/qsg_terminal_renderer.cpp`;
- `include/vnm_terminal/internal/terminal_graphic_geometry.h`;
- `include/vnm_terminal/internal/qsg_terminal_render_frame.h`;
- `include/vnm_terminal/internal/qsg_terminal_renderer.h`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- `tests/render_frame/render_frame_tests.cpp`;
- `docs/gpu_atlas_renderer_plan.md`;
- `docs/terminal_text_representation_plan.md`;
- `docs/qt_rendering_policy.md`.

Work:

- compute glyph bitmap rectangles in physical pixels;
- snap baseline origins, glyph origins, and quad rectangles at the physical
  pixel boundary;
- convert snapped physical rectangles back to logical draw rectangles;
- validate texel-center UVs and atlas gutter handling;
- apply the same policy to all shaped text runs, clipped runs, and cursor inverse
  text;
- keep box drawing and block/shade elements on `graphic_rects`/`graphic_arcs`
  so continuous surfaces and rounded corners remain grid-aligned;
- keep Braille and font-defined text symbols in `text_runs` and shaped atlas
  glyphs.

Acceptance gates:

- dense-grid X spacing test passes on D3D11;
- visual fixture shows no periodic gaps or row/column drift;
- `Process:` fixture is manually approved against the Qt text reference and
  stored with artifact metadata;
- snapped-origin failure counters remain zero on the demo and fixture set;
- box drawing and block/shade elements report graphic rect/arc primitives;
  Braille and ordinary text report shaped atlas glyph instances.
- `rg "GRAPHIC_GEOMETRY|packed_hard_block|graphic_arc_raster|block_rects"`
  finds only geometry routes that still own non-text graphics or tests/docs that
  accurately describe those remaining routes.

### Batch 7 - Warm Set And Lazy Insert Completion

Purpose:

- populate the atlas before normal text appears, while preserving complete
  coverage for glyphs outside the seed set.

Primary files:

- source-controlled warm-set table/generator files introduced by the batch;
- `src/qsg_atlas_renderer.cpp`;
- `include/vnm_terminal/internal/qsg_atlas_renderer.h`;
- `tests/qsg_atlas/atlas_tests.cpp`;
- companion `vnm_terminal` report/profile/test files for warm/lazy stats.

Work:

- add the source-controlled warm-set table/generator with scalar ranges and
  shaped cluster probes;
- shape the seed set through Qt for the active font epoch;
- prewarm atlas entries for discovered glyphs/fallback faces;
- insert cache misses lazily through the same rasterization path;
- add budget, startup-duration, first-frame, lazy-insert latency, and page-pressure
  diagnostics for warm and lazy inserts;
- expose warm/lazy stats in `vnm_terminal` where existing atlas reports are
  surfaced.

Acceptance gates:

- warm-set tests prove the named Unicode families and cluster probes are
  represented as shaped atlas entries;
- high-cardinality lazy-insert stress renders outside-seed CJK and color/emoji
  glyphs through the atlas;
- lazy insertion records insert count, latency, page pressure, incomplete frames,
  and zero unexplained misses;
- no production text run falls back to Qt text nodes;
- prewarm duration, startup latency, atlas memory, page pressure, lazy-insert
  frame impact, and visible first-frame completion are reported on D3D11 windowed
  hardware;
- `vnm_terminal` builds against the updated report API.

### Batch 8 - End-To-End Quality, Documentation, And Performance Gate

Purpose:

- produce the evidence needed for the owner to decide whether the atlas renderer
  is production-ready with the new quality contract.

Primary files:

- `docs/gpu_atlas_renderer_plan.md`;
- `docs/qt_rendering_policy.md`;
- `docs/architecture.md`;
- `docs/repository_guide.md`;
- `docs/terminal_text_representation_plan.md`;
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`;
- `C:\plms\varinomics\vnm_terminal\benchmarks\cmdg_nelostie\run_canonical_atlas_cmdg_gate.ps1`.

Work:

- run visual fixtures and atlas tests on D3D11 windowed hardware rendering;
- run the assembly/demo scenario used for manual inspection;
- run CMDG/end-to-end benchmarks after user idle confirmation;
- compare FPS, frame time, atlas memory, glyph misses, page pressure, warm/lazy
  timing, and frame completion against the Batch 1 baseline;
- update or retire any remaining docs that still conflict with the implemented
  atlas text quality contract;
- document the canonical atlas text quality contract.

Acceptance gates:

- manual visual check accepts `Process:` and dense grid rendering;
- no glyph miss is hidden as successful lower-quality rendering;
- no unexplained FPS, latency, memory, or startup regression remains;
- docs match the implemented contract;
- `rg "\bR8\b|r8_|direct_ascii|qt_layout|demot|QSGTextNode"` finds only references
  that are still accurate under the final contract;
- final production choice remains an owner decision based on the Batch 8 evidence;
- independent review is green before push.

## Review Requirements

Every batch needs independent review before it closes. Reviewers must classify
findings as blockers, owning-batch risks, or notes. A finding that suggests an
ASCII-only production slice, a normal `QSGTextNode` fallback, or unscheduled
future work conflicts with this plan unless it names a concrete predecessor
blocker and the exact successor batch that lands the work.

The orchestrator must push back on review that narrows the objective to a
smaller renderer without proving the full atlas contract impossible.

## Repositories

Primary implementation:

- `C:\plms\varinomics\vnm_terminal_surface`

Required companion updates:

- `C:\plms\varinomics\vnm_terminal`

The companion repo is updated in the same batch as any public report/API change
that would otherwise break the terminal app, tests, or benchmarks.
