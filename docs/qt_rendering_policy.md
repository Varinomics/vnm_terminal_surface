# Qt Rendering Policy

`vnm_terminal` renders through a C++ `QQuickItem` and Qt Scene Graph nodes. QML
may host `VNM_TerminalSurface`, but QML is not the terminal rendering or input
implementation layer.

## Qt Dependency Posture

The direct Qt module allowlist is:

- `Qt6::Core`
- `Qt6::Gui`
- `Qt6::Quick`

Private Qt module use is limited to link-scope posture entries:

- `Qt6::GuiPrivate`
- `Qt6::QuickPrivate`

These private modules are not direct product modules. They expose private Qt
APIs with no source or binary compatibility guarantee across Qt releases.
Installed/binary `vnm_terminal_surface` packages are tied to the Qt major/minor
used to build them, and the generated package config rejects consumers that
resolve a different Qt minor. Source-tree consumers that use `add_subdirectory`
rebuild the surface against their selected Qt and are not an installed-package
minor-mismatch case.

`Qt6::ShaderTools` is a source-tree build tool only when
`VNM_TERMINAL_ENABLE_SHADER_GENERATION` enables shader package generation. It is
not linked to `vnm_terminal_surface`, not part of the Qt posture link allowlist,
and not an installed package dependency.

The project uses Qt through either a commercial Qt license held by the
distributor or the LGPL-compatible dynamic-linking route recorded in
`THIRD_PARTY_NOTICES.md` and `THIRD_PARTY/`. The default
`VNM_TERMINAL_QT_LICENSE_ROUTE` is `lgpl_dynamic`; that route requires shared
Qt libraries. No GPL-only Qt module is allowed in the product dependency graph.

Qt source is not vendored in this repository. Under the LGPL route, product
packaging keeps Qt libraries separately replaceable and ships the applicable Qt
notices.

## Renderer Shape

The production renderer consumes immutable render snapshots and updates reusable
Qt Scene Graph nodes through one renderer contract. It does not allocate one
child `QQuickItem` per cell, and it does not render terminal rows or frames by
painting text into `QImage` with `QPainter`.

Current production text input is `Terminal_render_frame::text_runs` plus cursor
inverse-text input from `Terminal_render_frame::cursor_text_runs`. Those runs go
through public `QTextLayout` and `QSGTextNode` APIs, including
`QQuickWindow::createTextNode()` and `QSGTextNode::addTextLayout()`.
`frame.text_runs` remains the canonical terminal text input; cursor text runs
are overlay input derived from it.

Geometry routes may use public QSG geometry and material APIs. Private Qt text
and scene graph APIs are not part of the renderer contract. QRhi rendering may
use `Qt6::GuiPrivate` and Qt `rhi/` private headers only under the private Qt
module posture above.

`QImage` and `QPainter` are acceptable for test framebuffer readback and narrow
diagnostics. They are not the production terminal-row renderer and are not a
`QImage`-to-texture text route.

A GPU glyph-atlas renderer backend may cache glyphs rasterized by Qt's font
engine (`QRawFont::alphaMapForGlyph`) into atlas textures and composite cells
through instanced QRhi draws. Direct HarfBuzz, FreeType, and ICU dependencies
remain prohibited; shaping, rasterization, and fallback stay with Qt's font
engine. At cutover, this backend replaces the `QSGTextNode` text route, and the
replaced route is removed in the same batch. `QGlyphRun` may be used only for
validation or probing around the Qt text route; it does not move text ownership
away from `frame.text_runs`.

Packed row, text, and graphic sidecars are owned by `Terminal_render_frame`.
They are auxiliary classification, diagnostics, accounting, cache-key, row
identity, or graphics-route inputs. Packed text sidecars do not own terminal
text and do not retire or replace `frame.text_runs`.

`Terminal_scene_node` may keep reusable row caches keyed by active buffer,
logical row, and exact content or layer descriptors. Clean-row reuse is internal
and must respect dirty rows, viewport identity, geometry, style/color state, and
descriptor equality before preserving Qt nodes.

## Cell Ownership

Terminal cell identity belongs to the parser/model layer. Qt shaping, fallback,
and glyph advances are normalized into model-owned cell rectangles. Renderer
overlays such as selection, cursor, underline, strike, hyperlink underline,
preedit, reverse video, and visual bell use snapshot cell spans rather than
glyph bounds.
