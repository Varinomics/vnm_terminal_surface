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
| Shaping (code points -> positioned glyph ids) | Qt | `QTextLayout`, `QRawFont::glyphIndexesForString` |
| Glyph rasterization + hinting + AA | Qt | `QRawFont::alphaMapForGlyph` (returns the glyph image; color for color fonts) |
| Metrics | Qt | `QRawFont`, `QFontMetricsF`, existing `Qt_grid_metrics_provider` |
| Glyph cache + atlas packing + format conversion | **this renderer** | QRhi texture(s) |
| Per-cell instancing + GPU composition | **this renderer** | QRhi pipeline + buffers |
| Atlas lifecycle (DPR/size invalidation) | **this renderer** | — |

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
- **`QRawFont` is thread-local** and cannot be moved across threads. Rasterization uses
  a render-thread-local `QRawFont` constructed from the captured font, inside
  `prepare()`/`render()`. (Alternative: rasterize on the GUI thread and hand finished,
  converted `QImage` tiles across via the captured frame; the render-thread-local route
  is preferred to keep rasterization off the GUI thread.)
- All QRhi resource create/upload/destroy happen only in render-thread callbacks
  (`QSGRenderNode::prepare`/`render`/`releaseResources`). Atlas textures and buffers
  live on the render thread's QRhi.
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
  **order its passes by blending (painter's order), not depth**, apply the supplied
  projection/model-view matrices, and implement scissor plus **stencil** clipping
  itself to respect non-rectangular clips from ancestor `clip: true` items.
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
(`cmake/vnm_terminal_qt_posture.cmake`) currently allows only
`Qt6::Core`/`Qt6::Gui`/`Qt6::Quick` as direct targets and those plus
`Qt6::QuickPrivate` as link targets, and `FATAL_ERROR`s on anything else. So the plan's
earlier framing that QRhi is "already in the allowlist / adds no module" is wrong:
adopting QRhi adds `Qt6::GuiPrivate`.

Required, in the same batch as adoption:

- Amend `cmake/vnm_terminal_qt_posture.cmake` to permit `Qt6::GuiPrivate` as a link
  target, scoped to the atlas-renderer translation units.
- Document the no-BC-guarantee consequence in the policy posture section: builds using
  the atlas renderer are tied to the exact Qt minor version, exactly like the existing
  `Qt6::QuickPrivate` dependency. The LGPL/commercial posture is unchanged (still no new
  public module, still within Gui).

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
  half-rebuilt atlas — with old textures released on the render thread via
  `releaseResources`.

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
   reference does by placing them in the selection layer). Selection spans are already
   suppressed at snapshot build when `visible_line_provenance` is invalid
   (`suppress_selection_spans_without_valid_line_provenance`), so the renderer emits
   whatever spans the snapshot carries and cannot paint selection on stale/scrolled
   rows. Colors from captured `Terminal_render_options`.
3. **Graphic pass.** Box-drawing (U+2500–U+257F) and arcs come through
   `frame.graphic_rects`/`graphic_arcs` as instanced rects/arcs. **Hard block-element
   graphics (U+2580–U+259F: blocks, quadrants, shades) do not:** regardless of the
   `packed_text_sidecars_enabled` setting (off in production), they are routed through
   the packed graphic sidecar
   (`frame.packed_rows`/`packed_graphic_spans`/`packed_graphic_codepoints`), not
   `frame.graphic_rects` — except a hard-block cell *under the cursor or IME preedit*,
   which the cells pass does emit into `frame.graphic_rects` for the cursor pass to
   invert. The atlas renderer must reproduce the packed hard blocks by decoding the
   packed graphic spans (as the reference does via `packed_hard_graphic_rects`) or by
   re-deriving them from snapshot cells via `append_terminal_graphic_codepoint_rects`.
   Treating `frame.graphic_rects` as the complete graphic set silently drops every block
   element — the dense-block content that motivates this work. Both routes use the
   existing geometry path, not the atlas.
4. **Glyph (text) pass.** One instanced draw over occupied owning cells. Per-instance
   data: cell rect/origin, atlas page + UV, **fully-resolved RGBA foreground** (post
   FAINT alpha-halving / INVISIBLE `fg=bg` / INVERSE — resolved on the CPU as today),
   and flags. For the common single-BMP cell the glyph identity comes directly from the
   compact cell text's `single_bmp_code_unit()` (master's `terminal_render_cell_text.h`);
   only multi-codepoint clusters need shaping via `cell_stable_shaping.h`. The fragment
   shader blends the coverage (or color) tile with the
   foreground using the same straight/premultiplied-alpha convention as the current
   `coverage_color` path, so FAINT (alpha < 255) renders correctly. Wide/combining
   clusters follow `cell_stable_shaping.h` ownership: **one glyph instance per owning
   cell**; continuation cells emit no glyph. **Clipping:** glyphs are clipped
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
See [Viewport](#viewport-scrollback-and-snapshot-basis) for when partial updates must
fall back to full re-upload.

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

## Text-correctness surface

Each case states the **initial scope** and the **behavior to match** (the current
renderer's, per the [parity principle](#behavioral-parity-principle)).

| Case | Initial scope | Behavior to match |
| --- | --- | --- |
| Printable ASCII, single BMP scalar (CJK, Latin-1, symbols) | yes | shaped glyph, mono tile |
| Box-drawing / block graphics | yes | existing geometry route (instanced rects/arcs), not atlas |
| Combining marks / single-cell clusters | yes | shaped cluster via `cell_stable_shaping.h`, one owning tile |
| Wide (2-cell) glyphs | yes | one owning instance spanning two columns; continuation cells emit nothing (owning span covers them) |
| BOLD / ITALIC | yes | **match reference: not separately synthesized** (single base font) |
| FAINT / INVISIBLE | yes | resolved RGBA foreground (alpha-halved / fg=bg) carried per instance |
| INVERSE / reverse video | yes | resolved fg/bg swap on CPU as today |
| SGR BLINK | yes | match reference (no attribute blink animation) |
| Block-cursor inverse text | yes | glyph cell: solid fill + inverted glyph; graphic cell: fill carved around graphic + inverted graphic |
| Selection / IME-preedit background | yes | semi-transparent, composited **below** glyphs |
| Hyperlink underline | yes | option-gated on `underline_hyperlinks` (off by default) |
| Color emoji | **out of initial scope on the Qt 6.7 floor** | cross-platform color fonts unified only in Qt 6.9; gate this feature to a 6.9 minimum or defer |
| Font fallback | yes | Qt selects the fallback face; cache keyed by face id |
| Subpixel/LCD AA | out (initial) | grayscale AA only; revisit if parity requires |
| Complex scripts (Arabic/Indic cross-cell shaping) | scope decision required | match current per-cell behavior exactly; do not "improve" it here |
| Ligatures | out | terminals disable; match current behavior |

## Policy and posture amendments required

Two scoped amendments land in the same batch as adoption; neither is a silent weakening.

1. **[Qt rendering policy](qt_rendering_policy.md)** currently forbids "glyph atlas,
   glyph-tile, QImage-to-texture, QSGTexture, or parallel simple-text renderer route" and
   limits `QGlyphRun` to validation. Proposed amendment:

   > A GPU glyph-atlas renderer backend MAY cache glyphs rasterized by Qt's font engine
   > (`QRawFont::alphaMapForGlyph`) into atlas textures and composite cells via instanced
   > QRhi draws. Direct HarfBuzz, FreeType, and ICU dependencies remain prohibited;
   > shaping, rasterization, and fallback stay with Qt's font engine. The `QSGTextNode`
   > text route it replaces is removed at cutover.

2. **Qt dependency posture** — see
   [above](#qt-dependency-posture-amendment-required): permit `Qt6::GuiPrivate`, document
   the version-tied (no-BC) consequence.

## Staged rollout

Per the [change governance](https://github.com/Varinomics/varinomics-standards) shared
multi-batch rules: small, individually gated batches; the `QSGTextNode` renderer stays
selectable (behind a default-off flag) through bring-up for revert safety, and is
deleted at cutover in the same batch that makes the atlas renderer canonical (Rule 1).

- **Stage 0 — spike.** Confirm `QSGRenderNode` QRhi entry points and the posture
  amendment on the supported QRhi backends; stand up a trivial instanced quad behind the
  surface honoring scissor/stencil clip, depth-writes-disabled, and the supplied
  matrices; confirm `window()->rhi()` is available on every target deployment. Exit: a
  colored, correctly-clipped grid renders through QRhi behind `VNM_TerminalSurface`.
- **Stage 1 — atlas + capture infrastructure.** Glyph cache, bin-packing, R8 coverage
  atlas, `Format_Indexed8` conversion, render-thread-local `QRawFont` rasterization, and
  the sync-time state capture. Unit-tested in isolation (cache keys incl. physical pixel
  size, packing, epoch invalidation). No cutover.
- **Stage 2 — background + selection/preedit-bg + glyph + decoration + cursor passes.**
  In the reference paint order. Covers ASCII/BMP glyphs, FAINT/INVISIBLE/INVERSE
  resolution, below-glyph selection/preedit fills, block-cursor inversion (glyph and
  graphic carve-out), and decoration colors. Behind the selection flag, default off.
  Parity-tested.
- **Stage 3 — graphics, scroll/viewport, selection provenance, fallback faces.** Box/block
  geometry route, viewport/scrollback/alternate, PUBLIC_PROJECTION/SCROLL full-repaint
  fallback, provenance-gated selection, font fallback. Parity-tested.
- **Stage 4 — partial updates + perf.** Dirty-row instance updates against the rotating
  buffer, background coalescing, draw-call minimization. CMDG benchmark.
- **Stage 5 — cutover.** If the gates pass, make the atlas renderer unconditional and
  **delete the `QSGTextNode` consumer, the build-time option, and the runtime property in
  the same batch** (Rule 1), removing any orphaned dead code (verified via `git grep`).
  Update the companion `vnm_terminal` app and its profile-text counters in the same
  cross-repo batch (see [Cross-repo](#cross-repo-coordination)).

Each stage is independently revertable because the QSG renderer remains the default and
the atlas renderer stays behind a default-off flag until the Stage 5 cutover.

## Cross-repo coordination

Cutover (Stage 5) is a coordinated cross-repo batch with the `vnm_terminal` app. Concrete
successor touch-points: the app's profile-text counter parsing (the compact/fallback
counter names introduced for the text-representation work, plus any new atlas-renderer
counters), and removal of any transitional renderer-selection the app exposed during
bring-up. The exact symbol/file
list in `vnm_terminal` is enumerated when Stage 5 opens, the named successor batch; per
change-governance Rule 8 (defer-trap, satisfied because Stage 5 is named with its file
list), later-batch detail is not pulled forward into a current blocker.

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
  budgets per backend**, pinned to a **software QRhi backend**
  (`QSG_RHI_BACKEND`/`setGraphicsApi`) for the deterministic CI gate. The
  single-subpixel-bucket positional delta (≤ ~1px) is folded into this budget.
- **Glyph-correctness corpora:** ASCII, BMP/CJK, box/block graphics, combining marks, wide
  cells, presentation-selector pairs, font fallback — from the existing unicode-width,
  shaping-contract, and render tests.
- **Existing suites** (`render_snapshot`, `render_frame`, `qsg_render`,
  `shaping_contract`, `screen_*`, conformance) stay green for the unchanged snapshot
  contract and the selectable QSG path.
- **Performance gate (CMDG).** Measure the CMDG suite, hardware, windowed, against the
  `QSGTextNode` baseline; report per-scene frame time and CPU/GPU bound. Adoption requires
  a material, measured win on the motivating scenes with parity preserved.

Exact bit parity is an explicit non-goal; tolerances are documented per element class, not
as one global number.

## Decision Gates

This plan does not authorize implementation. Gates preceding Stage 0:

1. **Bound confirmation.** A render-thread/GPU diagnosis (`QSG_RENDER_TIMING`,
   resolution-scaling, vsync check, GPU timer queries) confirming the terminal's own
   rendering — not host content, not vsync idling — is the bottleneck on the target
   scenes.
2. **Policy + posture amendments.** The maintainer accepts the
   [rendering-policy and Qt-posture amendments](#policy-and-posture-amendments-required).

Continue past Stage 1 only if the spike shows the integration is sound. Continue to Stage
5 only if the CMDG performance gate and the layered parity gate both pass.

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
