# GPU Glyph-Atlas Renderer Plan

Status: **proposal, gated.** This plan describes an alternate GPU renderer backend
for `VNM_TerminalSurface`. It is not an adopted decision. Proceeding requires the
[Decision Gates](#decision-gates) to pass first: a rendering bound diagnosis that
confirms the terminal's own rendering is the bottleneck, a scoped amendment to the
[Qt rendering policy](qt_rendering_policy.md), and a scoped amendment to the Qt
dependency posture (QRhi is private API).

This document is a companion to [architecture](architecture.md),
[Qt rendering policy](qt_rendering_policy.md), and
[terminal text representation plan](terminal_text_representation_plan.md). The
text-representation work remains a valid CPU-side optimization; this plan addresses
a different, larger problem in a different layer.

## Single canonical renderer

The atlas renderer **replaces** `QSGTextNode` as the sole renderer; there is no
permanent fallback and no Rule 1 exception. The product targets accelerated (QRhi)
rendering only and does not support Qt Quick's software (`QPainter`) scene-graph
adaptation, so there is no environment where `QSGTextNode` would be needed and the
atlas renderer would not run — under the RHI backend both render through the same
`QRhi`, so if QRhi is unavailable the window cannot render at all. `QSGTextNode` is
kept only as a default-off experimental path *during* bring-up (Stages 0–4) for revert
safety; the cutover batch makes the atlas renderer canonical and deletes the
`QSGTextNode` consumer in the same commit, per change-governance Rule 1.

## Motivation

Observed, cross-implementation: the same terminal-canvas content (the CMDG scene
suite) renders an order of magnitude faster in a glyph-atlas/GPU terminal (Windows
Terminal's `AtlasEngine`) than in `VNM_TerminalSurface`. Because the byte stream and
cell content are identical across terminals, the content is not intrinsically
expensive; the renderer architecture is the limiter.

Profiling corroborates that the limited layer is not the one the text-representation
work targets. `append_cells` (model-to-snapshot copy) measured at ~7.9% of total
frame time and ~46% of *profiled GUI-thread* time, so the entire instrumented
GUI-thread cost is only ~17% of wall time; the CMDG profiler reports separate thread
trees, and the remaining ~80% of wall time is render-thread plus GPU plus present —
the Qt Quick scene-graph rendering of text and geometry. (These figures predate the
single-BMP inline cell-text work now on master, which further shrank the CPU
append/copy cost; the render-thread/GPU share is therefore, if anything, even more
dominant. The bound-confirmation gate re-measures them.)

Animated, full-grid, per-cell-varying-color content is close to the worst case for
the Qt Quick scene graph: per-run `QTextLayout` work, per-cell color/material changes
that defeat batching into many draw calls, and per-frame node management that
clean-row caching cannot save once every cell changes every frame. A glyph-atlas
renderer pays roughly one instanced draw for the whole grid, with per-cell instance
data sampled against a cached atlas. That is an architectural difference, not a
tuning difference.

**Goal:** close the rendering gap for animated full-grid content while preserving the
public `VNM_TerminalSurface` contract, the parser/model/session pipeline, and exact
visual parity with the current renderer.

**Non-goal:** writing a font rasterizer or shaper. Qt continues to perform font
loading, fallback selection, shaping, and glyph rasterization. This plan owns only
glyph caching and GPU composition.

## Behavioral parity principle

The atlas renderer must reproduce the **current** `QSGTextNode` renderer's behavior
exactly, including its present limitations — not an idealized renderer. Where the
current renderer ignores an attribute or declines a case, the atlas renderer does the
same. Concretely, the current renderer (per `src/qsg_terminal_renderer.cpp`):

- does **not** synthesize separate BOLD/ITALIC faces — one base `render_font` per run;
  `render_style_attributes` consults only UNDERLINE/STRIKE/INVERSE/FAINT/INVISIBLE;
- resolves FAINT by halving foreground alpha and INVISIBLE by `foreground = background`
  on the CPU before emission;
- inverts the block cursor by re-drawing the owning glyph (or, for a box/block cell,
  the graphic geometry) in the cell's *background* color clipped to the cursor rect —
  and for a graphic cell it *carves the graphic shape out of* the cursor fill rather
  than overpainting a solid fill;
- composites the semi-transparent selection fill and IME-preedit background **below**
  the glyph/graphic layers, not on top;
- gates hyperlink underline on the host option `underline_hyperlinks` (off by default)
  and visual bell on `visual_bell_enabled` plus `metadata.visual_bell_active`;
- sources cursor/selection/preedit/visual-bell colors and the effective cursor
  shape/blink from host-configured `Terminal_render_options`, not from the snapshot.

These behaviors are inputs to the design below and to the parity oracle. Any change to
them is a separate, post-parity behavior change, out of scope here.

## What stays, what changes

This is a renderer *backend swap*, not a new library. The runtime shape in
[architecture](architecture.md) is unchanged from the session boundary down to the
immutable render snapshot:

```text
backend bytes -> session -> parser -> screen model -> render snapshot   [UNCHANGED]
render snapshot -> render frame -> renderer                             [REPLACED BACKEND]
```

- **Unchanged:** `VNM_TerminalSurface` public API and base class (`QQuickItem`),
  event/IME/focus handling, backend selection, `Terminal_session`, parser, screen
  model, `Terminal_render_snapshot`, and `Terminal_render_frame` as the renderer input
  contract.
- **Replaced:** the snapshot/frame-to-`QSGNode` consumer (`qsg_terminal_renderer.cpp`)
  gains a GPU-atlas consumer driven from `updatePaintNode`. During bring-up the atlas
  consumer is behind a default-off flag; at cutover it replaces the `QSGNode` consumer,
  which is deleted in the same batch (Rule 1).
- **Reused:** `Qt_grid_metrics_provider` for cell metrics; `cell_stable_shaping.h` for
  cell-to-cluster/glyph orchestration and ownership invariants; the snapshot's style
  table, color state, cursor, selection spans + line provenance, dirty-row ranges,
  viewport, and modes.
- **Snapshot subset consumed:** identical to the current renderer — cells, styles,
  `color_state`, cursor (with `options` shape/blink overrides taking precedence over
  `snapshot.cursor`), IME preedit, selection spans with `visible_line_provenance`,
  dirty ranges, viewport, `modes.reverse_video`, `metadata.visual_bell_active`. All
  other snapshot fields (`metadata.mouse_reporting_mode_changed`, the rest of `modes`,
  hyperlink `uri`/`identity_key`, `public_scroll_diagnostics`) are intentionally not
  renderer inputs, exactly as today.

## Division of labor

Mirrors Windows Terminal's split (DirectWrite does fonts; `AtlasEngine` does atlas +
GPU). The Qt equivalent:

| Concern | Owner | Qt API |
| --- | --- | --- |
| Font load / selection / fallback | Qt | `QFont`, `QRawFont`, `QFontDatabase` |
| Shaping (code points -> fallback faces, positioned glyph ids) | Qt | `QTextLayout`, `QGlyphRun`, `QRawFont` |
| Glyph rasterization + hinting + AA | Qt | `QRawFont::alphaMapForGlyph` (returns the glyph image; color for color fonts) |
| Metrics | Qt | `QRawFont`, `QFontMetricsF`, existing `Qt_grid_metrics_provider` |
| Glyph cache + atlas packing + format conversion | **this renderer** | QRhi texture(s) |
| Per-cell instancing + GPU composition | **this renderer** | QRhi pipeline + buffers |
| Atlas lifecycle (DPR/size invalidation) | **this renderer** | — |

The compact cell text is a Unicode payload, not a glyph identity. Glyph ids,
glyph positions, fallback face identity, and `QRawFont` ownership come from Qt
shaping (`QTextLayout`/`QGlyphRun`) for every non-trusted-fast path, including
single-BMP CJK/symbol cells. A Unicode scalar is never used as an atlas cache
key. The only layout bypass is the trusted printable-ASCII route, and only after
the same support, fixed-pitch, clipping, and no-fallback checks used by the
current ASCII replacement path.

`QRawFont::alphaMapForGlyph` returns a `QImage` in `Format_Indexed8` for grayscale AA
(or `Format_RGB32` for subpixel), and the color-font image for color glyphs — it is
**not** a raw single-channel coverage buffer. The renderer must `convertToFormat` to
an 8-bit single-channel format (`Format_Grayscale8`/`Format_Alpha8`) and honor
`bytesPerLine` stride before uploading to an R8 `QRhiTexture`; color tiles upload as
RGBA. No direct HarfBuzz, FreeType, or ICU dependency is introduced.

## Threading and state capture

This is the most load-bearing part of the design, because the project's threaded
render loop changes what is safe.

Today, the entire frame build runs inside `updatePaintNode`, which reads
GUI-thread-owned mutable state **by direct reference** (`m_private->render_snapshot`,
`m_private->ime_preedit`, `m_private->cursor_blink_visible`, `m_private->cell_metrics`,
`m_private->render_font`, `m_private->render_device_pixel_ratio`, plus the item's
`boundingRect()`). That is safe only because, under the threaded render loop,
`updatePaintNode` runs on the render thread **with the GUI thread blocked** at sync.

A `QSGRenderNode` is different: its `prepare()`/`render()` run on the render thread
**after** the sync point, with the GUI thread running free. Dereferencing
`m_private->...` from `render()` would be a data race. Therefore:

- The render node must **capture immutable copies of every input at sync time**
  (inside `updatePaintNode`, GUI thread blocked): the `shared_ptr<const Terminal_render_snapshot>`
  handle, a copy of `Ime_preedit_state`, `cursor_blink_visible`, `cell_metrics`,
  `logical_size` (the item bounding rect — needed for the full-surface bell quad and
  viewport math, and not reachable from `RenderState`), device-pixel-ratio, the active
  font, and the resolved `Terminal_render_options` (cursor/selection/preedit/bell
  colors, cursor shape/blink overrides, `underline_hyperlinks`, `visual_bell_enabled`).
  `render()` reads only this captured state, never `m_private`.
- **Font invalidation is delivered through captured state, not a cross-thread signal.**
  `refresh_grid_metrics` is GUI-thread-only; the atlas cannot subscribe to it from the
  render thread. Each captured frame carries a font/size/DPR epoch; when the epoch
  changes, the render thread rebuilds the atlas before the next present.
- **`QRawFont`/`QGlyphRun` objects are thread-local** and cannot be moved across threads.
  Atlas text shaping happens on the render thread from the captured `QFont` and text
  payloads. The renderer uses the `QRawFont` from each render-thread `QGlyphRun` for
  glyph ids, positions, fallback face identity, and rasterization; the captured `QFont`
  is only the input to render-thread shaping. If shaping/rasterization is moved to the
  GUI thread instead, the captured payload must be finished converted glyph-image tiles
  plus placement metadata, not live `QRawFont`/`QGlyphRun` handles.
- All QRhi resource create/destroy happens only in render-thread callbacks. All
  copy/upload/update batches for atlas pages, instance buffers, uniform buffers, and
  epoch rebuilds are recorded in `QSGRenderNode::prepare()` before the render pass.
  `render()` only binds prepared pipelines/resources, applies viewport/scissor/stencil
  state, and issues draws. A glyph miss discovered too late for `prepare()` renders a
  replacement or previous prepared tile; it never uploads inside `render()`.
  `prepare()` should enumerate and populate all glyphs needed by the captured frame
  before instance data is finalized. If a late miss remains possible, `render()` may only
  mark render-node-local state and use a captured thread-safe queued notifier whose sole
  operation is to post a GUI-thread update request; the notifier must not expose
  GUI-owned mutable state to the render thread. `render()` must not call
  `QQuickItem::update()`, `QQuickWindow::update()`, dereference any GUI-thread `QObject`,
  or touch `VNM_TerminalSurface`/`m_private`.
- Routine font/size/DPR invalidation retires and rebuilds atlas resources on the render
  thread without relying on Qt to call `releaseResources()`. `releaseResources()` and
  the node destructor both reset all QRhi resources.
- Supported render loops: `basic` and `threaded`. The capture discipline above is
  required for `threaded` and harmless for `basic`.

## Integration point

Two ways to drive QRhi inside Qt Quick:

- **`QSGRenderNode`** — a scene-graph node issuing QRhi commands inline in the window's
  render pass. QRhi is obtained from `window()->rhi()`; the command buffer from
  `QSGRenderNode::commandBuffer()` and the target from `renderTarget()` (both since Qt
  6.6). `VNM_TerminalSurface` stays a `QQuickItem` and keeps `updatePaintNode`,
  returning a render node. The node must honor the `RenderState` it is given: only
  `ViewportState`/`ScissorState` are applied by the scene graph
  (`DepthState`/`StencilState`/`ColorState`/`BlendState` have no effect), so the
  renderer must disable depth writes, emit geometry at Z=0 in scene coordinates,
  **order its passes by blending (painter's order), not depth**, apply
  `projectionMatrix() * matrix()`, apply `inheritedOpacity()` to every pass, and
  implement scissor plus **stencil** clipping itself to respect non-rectangular clips
  from ancestor `clip: true` items. The implementation reports changed render state via
  `changedStates()` and chooses `flags()` conservatively: `NoExternalRendering`, plus
  `BoundedRectRendering`/`DepthAwareRendering` only when the implementation satisfies
  those contracts.
- **`QQuickRhiItem`** — renders into its own offscreen texture, composited as a
  textured quad. It was introduced **as Technology Preview in Qt 6.7** (the project's
  floor) and became a **public API in Qt 6.8**, inheriting only QRhi's no-BC guarantee;
  it changes the surface's base class and adds an extra offscreen composite per frame.

**Recommendation:** `QSGRenderNode`. It preserves the `QQuickItem` facade and
`updatePaintNode` structure (with the state-capture discipline above), avoids the extra
offscreen composite, and its QRhi entry points are stable since 6.6. Confirm exact
since-versions in Stage 0.

## Qt dependency posture amendment (required)

QRhi is **private Qt API** (`Qt::GuiPrivate`, `rhi/`-prefixed headers) with no
source/binary-compatibility guarantee across Qt versions. The project's posture gate
(`cmake/vnm_terminal_qt_posture.cmake`) allows `Qt6::Core`/`Qt6::Gui`/`Qt6::Quick`
as direct targets and permits private Qt modules only as link targets:
`Qt6::GuiPrivate`, `$<LINK_ONLY:Qt6::GuiPrivate>`, `Qt6::QuickPrivate`, and
`$<LINK_ONLY:Qt6::QuickPrivate>`. Anything else still fails posture validation.

The source and installed-package CMake posture must stay paired: root `find_package`
and `cmake/vnm_terminal_surfaceConfig.cmake.in` include `GuiPrivate`; posture tests
cover allowed and forbidden direct/link-only private targets. If compile-scope
isolation matters, atlas sources are built through a dedicated object library that
alone receives `GuiPrivate` include usage; the final static target still exports the
private link dependency.

The no-BC-guarantee consequence is product posture: installed/binary builds using
private Qt APIs are tied to the Qt major/minor used to build `vnm_terminal_surface`,
exactly like the existing `Qt6::QuickPrivate` dependency. The LGPL/commercial posture
is unchanged (still no new public module, still within Gui). Source `add_subdirectory`
consumption remains valid because the surface is rebuilt against the consumer's Qt.

## Shader pipeline

Atlas shaders are Vulkan-style GLSL sources under `resources/shaders/`, built with
`qt6_add_shaders` from a build-time `Qt6::ShaderTools` component, embedded in the Qt
resource system, and loaded with `QShader::fromSerialized`. No runtime
`QShaderBaker`/ShaderTools dependency is introduced. Generated shader packages are
built with the same Qt minor used for the product, or explicitly pinned to the supported
floor if prebuilt shader blobs are checked in.

`Qt6::ShaderTools` is a build-time-only source-tree requirement for generating embedded
`.qsb` resources. It is added to the root source build `find_package` only when
`VNM_TERMINAL_ENABLE_SHADER_GENERATION` is enabled, is not linked to
`vnm_terminal_surface`, is not added to `vnm_terminal_allowed_qt_link_targets`, and is
not added to installed
`vnm_terminal_surfaceConfig.cmake.in` `find_dependency()` because installed/binary
packages consume already-embedded shader resources.

## The glyph atlas

- **Cache key:** `(glyph index, font face id, physical pixel size, subpixel-position
  bucket)`. Physical pixel size is logical size × device-pixel-ratio. Monospace cell
  snapping makes subpixel buckets few; start with a single horizontal bucket and
  grayscale AA, and fold the resulting sub-pixel positional delta into the parity
  tolerance (see [Testing](#testing-and-benchmarks)). FAINT/INVISIBLE are color, not
  glyph, effects and are not in the key; BOLD/ITALIC are not in the key because the
  reference does not synthesize them.
- **Tile kinds:** an R8 coverage atlas for monochrome glyphs and an RGBA atlas for
  color glyphs. **Tile-kind selection is driven by the cluster's
  `Terminal_shaped_presentation_mode`** (EMOJI vs TEXT/DEFAULT) and the existing
  VS15/VS16 width policy — not guessed from the glyph. `REPLACEMENT` (U+FFFD) clusters
  route as ordinary mono glyphs.
- **Packing:** shelf/skyline bin-packing into one or a few `QRhiTexture` pages.
- **Population:** lazy. On first sighting, Qt rasterizes the glyph once; the result is
  converted (`Format_Indexed8` → single-channel, or RGBA for color) and uploaded; later
  frames sample the cached tile. Steady-state animated content reaches a stable working
  set.
- **Eviction:** none initially (terminal glyph sets are small and bounded). If a measured
  workload overflows the page budget, add LRU page eviction (and, as a hard cap, draw the
  replacement glyph rather than failing). Overflow is handled within the atlas, not by
  switching renderers.
- **Invalidation:** rebuild on font, font-size, or device-pixel-ratio change, delivered
  via the captured-frame epoch. A mid-session DPR change (window dragged between a 1.0
  and 2.0 screen) must invalidate and re-rasterize before the next present — no
  half-rebuilt atlas — with old textures retired on the render thread. The node stores
  the current QRhi pointer plus render-pass descriptor identity, target pixel size, sample
  count, color/depth-stencil state, and shader package version; if any changes
  (including host `layer.enabled` changing the render target), pipelines and
  target-dependent resources are rebuilt before drawing.

## The instanced renderer

Per frame, from the captured snapshot/frame. The renderer indexes cells via the
positional index (`render_snapshot_cells_by_position`), not by assuming `snapshot.cells`
is row-major (the snapshot does not guarantee production order).

Passes are emitted in the **reference's exact back-to-front paint order** (from the
scene-node child order: background → selection → graphic → text → decoration → cursor →
cursor-graphic → cursor-text → overlay). Because the `QSGRenderNode` ignores blend
state and the selection/preedit fills are semi-transparent, this order is load-bearing
and is enforced by painter's-order emission, not depth.

1. **Background pass.** (a) One full-`logical_size` quad in the default background color
   (it matches the surface even when not an exact cell multiple). (b) One
   background instance **per owning cell, spanning `display_width` columns**, emitted
   only when its resolved background differs from the grid default (reverse-video
   resolution matches the current renderer). (c) **Continuation cells emit nothing** —
   the owning cell's span covers their columns. (This matches the reference, which
   `continue`-skips `wide_continuation` cells and sizes the owning background rect to
   `display_width`.)
2. **Selection + preedit-background pass.** The semi-transparent selection highlight and
   the IME-preedit background, emitted **below** the graphic and glyph passes (this is
   why order matters — they are alpha 190 / 120 and must composite under glyphs, as the
   reference does by placing them in the selection layer). Selection fill instances are
   emitted first; IME-preedit background instances are emitted after selection within the
   same below-text layer, and batching must not reorder those translucent primitives.
   Selection spans are already suppressed at snapshot build when
   `visible_line_provenance` is invalid (`suppress_selection_spans_without_valid_line_provenance`),
   so the renderer emits whatever spans the snapshot carries and cannot paint selection
   on stale/scrolled rows. Colors from captured `Terminal_render_options`.
3. **Graphic pass.** Box-drawing (U+2500–U+257F) and arcs come through
   `frame.graphic_rects`/`graphic_arcs` as instanced rects/arcs. **Hard block-element
   graphics means the current `terminal_hard_block_graphic_is_supported` set only:**
   implemented U+2580–U+2590 and U+2596–U+259F forms. Unsupported shade/one-eighth
   variants remain text/atlas glyphs exactly as today. Supported hard blocks, regardless
   of the `packed_text_sidecars_enabled` setting (off in production), are routed through
   the packed graphic sidecar
   (`frame.packed_rows`/`packed_graphic_spans`/`packed_graphic_codepoints`), not
   `frame.graphic_rects` — except a hard-block cell *under the cursor or IME preedit*,
   where supported hard blocks also populate cursor/normal graphic primitives needed for
   inversion/preedit parity. The atlas renderer must reproduce the packed hard blocks by decoding the
   packed graphic spans (as the reference does via `packed_hard_graphic_rects`) or by
   re-deriving them from snapshot cells via `append_terminal_graphic_codepoint_rects`.
   Treating `frame.graphic_rects` as the complete graphic set silently drops every block
   element — the dense-block content that motivates this work. Both routes use the
   existing geometry path, not the atlas.
4. **Glyph (text) pass.** One instanced draw family over occupied owning text clusters.
   Per-instance data: cell rect/origin, atlas page + UV, **fully-resolved RGBA
   foreground** (post FAINT alpha-halving / INVISIBLE `fg=bg` / INVERSE — resolved on the
   CPU as today), and flags. Compact cell text supplies only the Unicode payload; Qt
   shaping supplies glyph ids, positions, and fallback face identity. Ownership is one
   text cluster per owning cell, but rendering emits one or more glyph instances from the
   shaped `QGlyphRun` for that cluster. All glyphs in the cluster share the owning
   cell/cluster span and positions supplied by Qt; continuation cells emit no ownership.
   Cursor inverse redraws every glyph instance in the intersecting cluster, clipped to
   the cursor rect. The fragment shader blends the coverage (or color) tile with the
   foreground using the same straight/premultiplied-alpha convention as the current
   `coverage_color` path, so FAINT (alpha < 255) renders correctly. **Clipping:** glyphs are clipped
   *vertically* to the cell/row height across the full row width (a per-row vertical
   clip, matching the reference `row_text_clip_rect`); ordinary glyphs are **not**
   horizontally clipped per cell — horizontal overhang into adjacent cells is allowed,
   as in the reference (including natural-wide single-cell clusters, which the reference
   leaves unclipped — it does not apply the `cell_stable_shaping.h` cluster clip here). A
   per-instance horizontal clip is applied only to the cursor-inverse instance.
   IME-preedit *text* is part of this pass, drawn in the option colors
   (`default_foreground` / `preedit_background`), not per-cell style.
5. **Decoration pass.** Underline, strike, and hyperlink underline as instanced
   lines/rects, in the cell's **per-instance resolved foreground** (not a uniform).
   Hyperlink underline only when `underline_hyperlinks` is set. The IME-preedit caret is
   part of this pass, in the option `default_foreground` color (not per-cell style). SGR
   BLINK is matched to the reference (no attribute-blink animation).
6. **Cursor pass.** Effective cursor shape and blink come from the captured `options`
   overrides taking precedence over `snapshot.cursor`, gated by `cursor_blink_visible`.
   For a BLOCK cursor:
   - over a **glyph** cell: fill the cell with the cursor color, then re-draw the owning
     glyph instance within the cursor rect in the inverted color (cell background as
     foreground) — the AA glyph blends over the solid fill.
   - over a **box/block-graphic** cell: the fill is the cursor rect **minus the graphic
     rects** (`cursor_rects_excluding_graphics` subtracts `cursor_graphic_rects` only,
     not arcs), and the inverted graphic is drawn into the carved gap. For arc glyphs
     (U+256D–U+2570) the fill is not carved and the inverted arc overpaints it, matching
     the reference.
   - over an **empty** cell: fill only.
   - over a **wide** cell: the block cursor rect is a single cell and the inverted glyph
     is clipped to it, so only the cursor-column slice inverts; the rest of the wide
     glyph renders normally (matching the reference's single-cell-clipped inversion).
   BAR/UNDERLINE cursors are instanced rects.
7. **Overlay pass (top).** Only the visual bell: a single full-`logical_size` blended
   quad gated on `visual_bell_enabled` plus `metadata.visual_bell_active`.

Buffers: a dynamic instance buffer plus a uniform buffer for grid metrics, viewport, and
option colors. Because QRhi double-buffers `Dynamic` buffers across in-flight frames, the
dirty-row partial-update optimization keeps a CPU-side mirror and re-uploads dirty
regions against the correct rotating buffer each frame, rather than assuming a single
persistent buffer whose unchanged regions survive — or uses a non-rotating buffer type.
Dirty rows drive text/content row uploads only. Selection, preedit, cursor, decorations
affected by `underline_hyperlinks`, option-color uniforms, visual bell, viewport
identity, and scroll/projection basis are invalidated from their own captured inputs or
force their covered rows/layers dirty. See
[Viewport](#viewport-scrollback-and-snapshot-basis) for when partial updates must fall
back to full re-upload.

## Viewport, scrollback, and snapshot basis

The snapshot carries a viewport/scroll model the renderer must honor:

- **Viewport offset / scrollback / alternate buffer.** Instances are keyed to viewport
  row indices (the renderer is already viewport-row based via `cell_rect`).
  `viewport.offset_from_tail`, `scrollback_rows`, and `active_buffer`
  (PRIMARY/ALTERNATE) select which logical rows are visible; the instance build uses the
  snapshot's visible rows, not raw model rows.
- **Snapshot basis/purpose.** `PUBLIC_PROJECTION`/`SCROLL` snapshots are validated to a
  **full-grid dirty range**. The partial dirty-row update path must detect
  `basis == PUBLIC_PROJECTION` or `purpose == SCROLL` (or any snapshot whose dirty range
  coalesces to the full grid) and fall back to a full instance-buffer re-upload. Treating
  a scroll/projection frame as a partial update would leave stale rows.
- **Row reuse identity.** Viewport row is only the presentation slot. Reuse identity
  includes active buffer and logical row, and text rows also include retained line id and
  content generation when provenance is valid.
- **Selection provenance.** Selection spans are suppressed at snapshot build when
  `visible_line_provenance` is invalid; the renderer emits the spans the snapshot
  carries, so selection never paints on stale/scrolled rows.

## Renderer selection during bring-up

The atlas renderer is gated behind a build-time option (default off) plus a runtime
property, so it can be exercised and parity-tested alongside the default `QSGTextNode`
renderer during Stages 0–4. This selection is **transitional**: at cutover the atlas
renderer becomes unconditional and the option, the runtime property, and the
`QSGTextNode` consumer are all removed in the same batch (Rule 1).

There is no automatic-fallback-on-error path. The product requires a QRhi (RHI)
backend; `QSGTextNode` and the atlas renderer both run on the same `QRhi` under that
backend, so there is no "QRhi failed but QSGTextNode works" state to guard — if QRhi is
unavailable the window cannot render at all. QRhi backend availability is a deployment
requirement, validated once at the Stage-0 spike. Atlas-budget overflow is handled by
eviction, not by switching renderers; glyph-rasterization failures draw the replacement
glyph, as any renderer does.

The Qt Quick software scene-graph adaptation (`QSGRendererInterface::Software`, including
the current `vnm_terminal --software-renderer` diagnostics) is not an atlas path because
`QQuickWindow::rhi()` is null there. During Stages 0–4 existing software-scene-graph
tests remain only for the transitional `QSGTextNode` path, or are duplicated onto a named
accelerated QRhi backend. Stage 5 removes or redefines `vnm_terminal --software-renderer`
and any CMDG/test options that still imply the software adaptation.

## Text-correctness surface

Each case states the **initial scope** and the **behavior to match** (the current
renderer's, per the [parity principle](#behavioral-parity-principle)).

| Case | Initial scope | Behavior to match |
| --- | --- | --- |
| Printable ASCII, single BMP scalar (CJK, Latin-1, symbols) | yes | shaped glyph, mono tile |
| Box-drawing / supported hard-block graphics | yes | existing geometry route (instanced rects/arcs), not atlas; unsupported block/shade variants remain text glyphs |
| Combining marks / single-cell clusters | yes | shaped cluster via `cell_stable_shaping.h`, one owning cluster that may emit multiple glyph instances |
| Wide (2-cell) glyphs | yes | one owning cluster spanning two columns; continuation cells emit nothing (owning span covers them) |
| BOLD / ITALIC | yes | **match reference: not separately synthesized** (single base font) |
| FAINT / INVISIBLE | yes | resolved RGBA foreground (alpha-halved / fg=bg) carried per instance |
| INVERSE / reverse video | yes | resolved fg/bg swap on CPU as today |
| SGR BLINK | yes | match reference (no attribute blink animation) |
| Block-cursor inverse text | yes | glyph cell: solid fill + inverted glyph; graphic cell: fill carved around graphic + inverted graphic |
| Selection / IME-preedit background | yes | semi-transparent, composited **below** glyphs |
| Hyperlink underline | yes | option-gated on `underline_hyperlinks` (off by default) |
| Color emoji | **bring-up defer only** | before cutover, emoji-presenting clusters must either pass the parity gate on the supported Qt version/backend or be explicitly documented as matching the current renderer's observed fallback/replacement behavior |
| Font fallback | yes | Qt selects the fallback face; cache keyed by face id |
| Subpixel/LCD AA | out (initial) | grayscale AA only; revisit if parity requires |
| Complex scripts (Arabic/Indic cross-cell shaping) | scope decision required | match current per-cell behavior exactly; do not "improve" it here |
| Ligatures | out | terminals disable; match current behavior |

## Pre-Stage-0 policy and posture acceptance gate

The pre-Stage-0 posture slice applies the narrow policy and package prerequisites that
make QRhi/atlas exploration executable. This is an acceptance gate, not implementation
authorization: Stage 0 remains blocked until the bound-confirmation gate passes and the
maintainer accepts the amended policy/posture.

1. **[Qt rendering policy](qt_rendering_policy.md).** The policy permits a GPU
   glyph-atlas backend to cache glyphs rasterized by Qt's font engine
   (`QRawFont::alphaMapForGlyph`) into atlas textures and composite cells through
   instanced QRhi draws. Direct HarfBuzz, FreeType, and ICU dependencies remain
   prohibited; shaping, rasterization, and fallback stay with Qt's font engine. At
   cutover, this backend replaces the `QSGTextNode` text route, and the replaced route
   is removed in the same batch. `QGlyphRun` remains validation/probing-only around the
   Qt text route and does not move text ownership away from `frame.text_runs`.

2. **Qt dependency posture.** See
   [above](#qt-dependency-posture-amendment-required): source/package CMake permits
   `Qt6::GuiPrivate` as a private link dependency, records the Qt private-API
   major/minor for installed packages, rejects installed consumers with a different Qt
   minor, and keeps `Qt6::ShaderTools` source-tree build-time-only when shader
   generation is enabled.

## Staged rollout

Per the [change governance](https://github.com/Varinomics/varinomics-standards) shared
multi-batch rules: small, individually gated batches; the `QSGTextNode` renderer stays
selectable (behind a default-off flag) through bring-up for revert safety, and is
deleted at cutover in the same batch that makes the atlas renderer canonical (Rule 1).

- **Stage 0 — spike.** Confirm `QSGRenderNode` QRhi entry points and the posture
  amendment on the supported QRhi backends; stand up a trivial instanced quad behind the
  surface. Exit evidence: backend matrix; non-null `window()->rhi()`,
  `commandBuffer()`, and `renderTarget()` proof; pixel tests for
  `projectionMatrix() * matrix()` under translated/scaled ancestors; inherited opacity
  applied to every pass; no-clip, scissor clip using `state->scissorRect()` coordinates,
  and stencil clip using compare `EQUAL`, ops `KEEP`, ref `state->stencilValue()`, masks
  `0xff`; depth writes disabled; `changedStates()` reports `ViewportState |
  ScissorState` when those states are changed; normal hosting, clipped hosting, and
  `layer.enabled` ancestor hosting.
- **Stage 1 — atlas + capture infrastructure.** Glyph cache, bin-packing, R8 coverage
  atlas, `Format_Indexed8` conversion, render-thread-local `QRawFont` rasterization, and
  the sync-time state capture. Exit evidence: cache-key tests including physical pixel
  size and fallback face id; packing/stride/format-conversion tests; epoch invalidation
  tests; `Captured_atlas_frame` value built only during `updatePaintNode`; source-posture
  check and render smoke proving `prepare()`/`render()` read only the captured value and
  render-thread QRhi/QSGRenderNode accessors, never `VNM_TerminalSurface`, `m_private`,
  `boundingRect()`, GUI-owned options, direct GUI update scheduling, or GUI-object
  dereference. Run capture smoke under both
  `QSG_RENDER_LOOP=threaded` and `QSG_RENDER_LOOP=basic`, including a test that mutates
  GUI-thread surface state after sync and verifies the rendered frame uses the captured
  snapshot/options/font epoch. No cutover.
- **Stage 2 — background + selection/preedit-bg + glyph + decoration + cursor passes.**
  In the reference paint order. Covers ASCII/BMP glyphs, FAINT/INVISIBLE/INVERSE
  resolution, below-glyph selection/preedit fills, block-cursor inversion (glyph and
  graphic carve-out), and decoration colors. Behind the selection flag, default off.
  Exit evidence: named parity corpus results with exact masked zero-diff regions for
  fills/decorations and per-backend AA budgets for glyphs.
- **Stage 3 — graphics, scroll/viewport, selection provenance, fallback faces.** Box/block
  geometry route, viewport/scrollback/alternate, PUBLIC_PROJECTION/SCROLL full-repaint
  fallback, provenance-gated selection, font fallback. Exit evidence: parity corpus
  covering supported/unsupported hard blocks, box arcs, scrollback/alternate viewport,
  selection provenance, fallback fonts, emoji policy, and row-reuse identities.
- **Stage 4 — partial updates + perf.** Dirty-row instance updates against the rotating
  buffer, background coalescing, draw-call minimization. Exit evidence:
  rotating-buffer tests, non-dirty selection/cursor/preedit/options/visual-bell
  invalidation tests, PUBLIC_PROJECTION/SCROLL full-reupload tests, atlas memory/page
  budget counters, and CMDG performance gate.
- **Stage 5 — cutover.** If the gates pass, make the atlas renderer unconditional and
  **delete the `QSGTextNode` consumer, the build-time option, and the runtime property in
  the same batch** (Rule 1), removing any orphaned dead code (verified via `git grep`).
  Update the companion `vnm_terminal` app and its profile-text counters in the same
  cross-repo batch (see [Cross-repo](#cross-repo-coordination)). Exit evidence: cross-repo
  deletion grep, build, tests, parity gate, and CMDG gate.

Each stage is independently revertable because the QSG renderer remains the default and
the atlas renderer stays behind a default-off flag until the Stage 5 cutover.

## Cross-repo coordination

Cutover (Stage 5) is a coordinated cross-repo batch with the `vnm_terminal` app. Minimum
successor file list:

- framework: `src/qsg_terminal_renderer.cpp`,
  `include/vnm_terminal/internal/qsg_terminal_renderer.h`,
  `include/vnm_terminal/internal/qsg_terminal_render_frame.h`,
  `include/vnm_terminal/internal/vnm_terminal_surface_render_bridge.h`,
  `include/vnm_terminal/internal/hierarchical_profiler.h`,
  `include/vnm_terminal/internal/metrics_contract.h`, `src/vnm_terminal_surface.cpp`,
  `src/metrics_contract.cpp`, `tests/qsg_render/qsg_render_tests.cpp`,
  `tests/qsg_text_node/qsg_text_node_tests.cpp`, `tests/render_frame/render_frame_tests.cpp`,
  `tests/surface_host/surface_host_tests.cpp`, `tests/qt_render_smoke/render_smoke_tests.cpp`,
  `tests/qt_metrics/qt_metrics_tests.cpp`, `tests/package_smoke/CMakeLists.txt`, and
  `tests/CMakeLists.txt` if the public `QSGTextNode` probe is retired,
  `benchmarks/embedded_terminal/CMakeLists.txt`,
  `benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp`,
  `benchmarks/surface_stress/terminal_surface_stress_benchmark.cpp`,
  `cmake/vnm_terminal_qt_posture.cmake`, `cmake/vnm_terminal_surfaceConfig.cmake.in`,
  `docs/qt_rendering_policy.md`, `docs/architecture.md`, and
  `docs/repository_guide.md`;
- app: `src/main.cpp`, `benchmarks/cmdg_nelostie/CMakeLists.txt`,
  `benchmarks/cmdg_nelostie/run_cmdg_nelostie.cmake`,
  `benchmarks/cmdg_nelostie/README.md`, `tests/CMakeLists.txt`,
  `tests/expect_metrics_json.cmake`, and `docs/debugging_knowledge.md`.

Concrete successor touch-points: the app's profile-text counter parsing (the
compact/fallback counter names introduced for the text-representation work, plus any new
atlas-renderer counters), removal or redefinition of `--software-renderer`, removal of
any transitional renderer-selection the app exposed during bring-up, and package config
minor-version enforcement. Exit requires `git grep` in both repos for transitional
selector/property names, `QSGTextNode` consumer assumptions, and stale counter names.

## Testing and benchmarks

A bit-exact cross-renderer oracle is **not** achievable: the reference rasterizes via
`QSGTextNode` at `QtRendering` (Qt font-engine grayscale), while the atlas rasterizes via
`QRawFont::alphaMapForGlyph` and composites in a custom shader, and results differ across
QRhi backends (Vulkan/Metal/D3D/GL) in hinting, gamma/blend space, and snapping. The
existing tests are also not an exact oracle — they sample colors with tolerance 4–12, and
the only exact compares are same-renderer self-consistency. So the parity gate is layered
and must be built as new work:

- **Exact, masked** parity on non-antialiased elements: backgrounds, selection fill,
  block-cursor fill, solid decorations — masked-region byte-diffs.
- **Perceptual/structural** parity on glyph and antialiased-geometry regions: a per-pixel
  max-delta plus a budget on the count of deviating pixels (or SSIM), with **separate
  budgets per backend**. The deterministic CI parity gate runs on one explicitly
  supported accelerated QRhi backend fixed by Stage 0 (for example D3D11 on Windows CI),
  selected before the first `QQuickWindow` via `QSG_RHI_BACKEND` or
  `QQuickWindow::setGraphicsApi`, with Qt minor version, font, DPR, window size, graphics
  API, and backend-specific tolerances recorded in artifacts. Do not use
  `QSGRendererInterface::Software` for atlas parity; backend-specific software adapters
  such as `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` are allowed only if Stage 0 proves they
  still produce a non-null `QQuickWindow::rhi()`. The single-subpixel-bucket positional
  delta (≤ ~1px) is folded into this budget.
- **Glyph-correctness corpora:** ASCII, BMP/CJK, box/block graphics, combining marks, wide
  cells, presentation-selector pairs, font fallback — from the existing unicode-width,
  shaping-contract, and render tests.
- **Existing suites** (`render_snapshot`, `render_frame`, `qsg_render`,
  `shaping_contract`, `screen_*`, conformance) stay green for the unchanged snapshot
  contract and the selectable QSG path.
- **Performance gate (CMDG).** Measure the CMDG suite, hardware, windowed, against the
  `QSGTextNode` baseline using `vnm_terminal` `benchmarks/cmdg_nelostie` Release runs
  with profiling off for timing, `OFFSCREEN=OFF`, `SOFTWARE_RENDERER=OFF`, fixed
  window/font/frame limit/scenes/repeats, artifact tags for baseline/candidate, and
  warmed or interleaved ordering with CPU-frequency counters when available. Profile-text
  runs are separate. Compare `scene_frames_per_second`, `draw_frames_per_second`,
  terminal `paint_frames_per_second`, renderer counters, GPU timing, and atlas
  memory/page metrics. Pass requires at least 25% median improvement on the motivating
  CMDG scenes, no default CMDG scene regression over 5%, no ordinary shell/render smoke
  regression, and no atlas overflow outside the declared budget.

Exact bit parity is an explicit non-goal; tolerances are documented per element class, not
as one global number.

## Decision Gates

This plan does not authorize implementation. Gates preceding Stage 0:

1. **Bound confirmation.** A render-thread/GPU diagnosis confirming the terminal's own
   rendering — not host content, input backlog, vsync idling, or producer starvation — is
   the bottleneck on the target scenes. It records scene list, window/font/frame limit,
   Qt version, QRhi backend, render loop, hardware, build flags, vsync setting, p50/p95
   frame time, `QSG_RENDER_TIMING`, terminal/CMDG metrics JSON, profile text,
   resolution/grid-scaling results, and GPU timer-query results or an explicit
   unavailable note. The gate passes only when the same CMDG scenes/window/font as the
   performance gate reach `frame_limit`, terminal metrics show no backend errors/timeouts,
   vsync is disabled or proven non-capping, model/snapshot append/copy remains below 20%
   of frame wall time, and render-thread + GPU/present work accounts for at least 70% of
   frame wall time or is the dominant scaling component in the resolution/grid-control
   run.
2. **Policy + posture amendments.** The maintainer accepts the
   [pre-Stage-0 policy/posture acceptance gate](#pre-stage-0-policy-and-posture-acceptance-gate).

Start Stage 1 only if the Stage 0 spike exit evidence passes. Start Stage 5 only if the
CMDG performance gate and the layered parity gate both pass.

## Risks

- **Maintenance burden.** A glyph cache + instanced renderer + atlas lifecycle is a real,
  ongoing component that replaces `QSGTextNode`'s free correctness — after cutover the
  product owns this rendering path with no fallback.
- **Private-API exposure.** `Qt6::GuiPrivate`/QRhi has no BC guarantee; builds are tied to
  the exact Qt minor version (like the existing `QuickPrivate` use). A Qt upgrade may
  require renderer changes.
- **Threading correctness.** The state-capture discipline is mandatory; a missed capture
  is a render-thread data race.
- **Parity oracle complexity.** Cross-renderer/cross-backend AA differences make the
  oracle a non-trivial new test component; a too-loose budget hides regressions, a
  too-tight one never passes.
- **Color emoji and fallback edge cases.** RGBA-vs-coverage routing by presentation mode,
  color-font support only from Qt 6.9, and per-face caching are the fiddliest correctness
  areas.
- **Cross-platform QRhi backends and HiDPI.** Backend resource/shader differences and
  mid-session DPR changes must stay exact.
- **Cross-repo coordination.** The `vnm_terminal` app and its counters change with the
  cutover batch.

## Open questions

- `QSGRenderNode` vs `QQuickRhiItem` for production (Stage-0 spike resolves;
  recommendation is `QSGRenderNode`).
- Grayscale-only AA acceptable, or is subpixel/LCD AA required for parity?
- Complex-script scope: match current per-cell behavior exactly (default) vs improve (out
  of scope for the perf goal).
- Atlas page budget and whether any measured workload needs eviction.
- Color-emoji minimum Qt version (defer on 6.7, or raise that feature to 6.9).
