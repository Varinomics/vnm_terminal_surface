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

`Qt6::ShaderTools` is source-tree tooling only when
`VNM_TERMINAL_ENABLE_SHADER_GENERATION` is enabled. The current build embeds
checked-in `.qsb` shader packages from `resources/shaders/`; the option only
requires the ShaderTools component for source-tree shader tooling and does not
wire shader package regeneration into the target build. ShaderTools is not
linked to `vnm_terminal_surface`, not part of the Qt posture link allowlist, and
not an installed package dependency.

Dual-source atlas fragment shader packages must carry OpenGL GLSL 330 targets
and patched GLSL 150 replacements from the corresponding `.glsl150.frag`
source files. Qt Shader Baker's generated GLSL 150 output drops the explicit
fragment-output location while keeping `index = 1`, which some Mesa/OpenGL
drivers reject. Regenerate those packages with explicit `--glsl
"100 es,120,150,330"` targets, then replace `glsl,150` from the checked-in
`.glsl150.frag` file before committing the `.qsb`.

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
inverse-text input from `Terminal_render_frame::cursor_text_runs`. The canonical
atlas renderer rasterizes glyph coverage through Qt's font engine, caches it in
atlas textures, and composites cells through QRhi-backed scene graph render
nodes. `frame.text_runs` remains the canonical terminal text input; cursor text
runs are overlay input derived from it.

Geometry routes may use public QSG geometry and material APIs. The canonical
atlas renderer may use QRhi through `Qt6::GuiPrivate` and Qt `rhi/` private
headers only under the private Qt module posture above; that private-API use is
part of the renderer contract and is guarded by the package minor-version
checks. Private Qt text APIs remain outside the terminal text ownership
contract.

`QImage` and `QPainter` are acceptable for test framebuffer readback and narrow
diagnostics. They are not the production terminal-row renderer and are not a
`QImage`-to-texture text route.

The GPU glyph-atlas renderer caches glyphs rasterized by Qt's font engine
(`QRawFont::alphaMapForGlyph`) into atlas textures and composites cells through
instanced QRhi draws. Direct HarfBuzz, FreeType, and ICU dependencies remain
prohibited; shaping, rasterization, and fallback stay with Qt's font engine.
`QGlyphRun` may be used only for validation around Qt font behavior; it does not
move text ownership away from `frame.text_runs`.

Packed row and text sidecars are owned by `Terminal_render_frame`. They are
auxiliary classification, diagnostics, accounting, cache-key, and row identity
inputs. Packed text sidecars do not own terminal text and do not retire or
replace `frame.text_runs`.

The atlas render node may keep reusable QRhi resources and row/layer upload
state keyed by active buffer, logical row, and exact content or layer
descriptors. Clean-row reuse is internal and must respect dirty rows, viewport
identity, geometry, style/color state, and descriptor equality before preserving
renderer-local GPU state.

## Cell Ownership

Terminal cell identity belongs to the parser/model layer. Qt shaping, fallback,
and glyph advances are normalized into model-owned cell rectangles. Renderer
overlays such as selection, cursor, underline, strike, hyperlink underline,
preedit, reverse video, and visual bell use snapshot cell spans rather than
glyph bounds.
