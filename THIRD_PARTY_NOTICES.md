# Third-Party Notices

`vnm_terminal_surface` is distributed under the project license in `LICENSE`.

The native process custody library and owner helper in `cpp/process_custody`
are Copyright © 2026 Varinomics Ltd. and use the same GPL-3.0 license.
Standalone custody packages include that license and the copyright notice.

## Qt 6

Qt 6 Core, Gui, and Quick are required. The project uses Qt through either a
commercial Qt license held by the distributor or an LGPLv3-compatible
dynamic-linking posture. No GPL-only Qt module is allowed in the product
dependency graph.

The per-module records in `THIRD_PARTY/` name the Qt module, upstream project,
license expression, CMake target, source path, and reviewed license posture.

Qt upstream notices and license texts are supplied by the installed Qt package
and the Qt Company distribution materials:

- https://www.qt.io/licensing/
- https://doc.qt.io/qt-6/licenses-used-in-qt.html

## Windows ConPTY

Windows sessions use Microsoft's redistributable ConPTY host from the
`Microsoft.Windows.Console.ConPTY` NuGet package, licensed under MIT.
The upstream project is https://github.com/microsoft/terminal and its license
is preserved in `THIRD_PARTY/conpty/LICENSE` and deployed with the runtime.

## Embedded Fonts

The default terminal face is Ubuntu Sans Mono derivative vnm.

The default monospace face is supplied by
[`vnm_fonts`](https://github.com/Varinomics/vnm_fonts), which ships the file
byte-verbatim as its author published it and marks the family name in memory as
it enters the font database. That repository carries the provenance manifest,
the upstream licence text, and the notice for every face it ships; this project
redistributes none of them itself.

## Unicode 16.0 Data Files

The terminal width policy is pinned to Unicode 16.0.0 data files published by
the Unicode Consortium. These files are not vendored in the repository.
Generated table artifacts record the exact input URLs and hashes used.

Unicode data files are governed by the Unicode License v3. The source index is:

- https://www.unicode.org/Public/16.0.0/
