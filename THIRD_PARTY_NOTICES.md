# Third-Party Notices

`vnm_terminal_surface` is distributed under the project license in `LICENSE`.

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

## Ubuntu Mono derivative Bront Embedded Font

`resources/fonts/UbuntuMonoDerivativeBront-Regular.ttf` retains the Bront glyph
outlines and metrics. It is licensed under the Ubuntu Font Licence 1.0.

Copyright notice retained from the font:

- Copyright 2011 Canonical Ltd. Licensed under the Ubuntu Font Licence 1.0.

Upstream contributor:

- Chris Wendt (`chrismwendt`), the author of the pinned upstream commit. The
  pinned upstream files contain no separate contributor copyright statement.

Varinomics changed only the name table so this non-substantially changed Ubuntu
Mono derivative follows the UFL 1.0 section 2(c) form `Ubuntu Mono derivative
Bront`. The immutable input and output hashes, exact metadata diff, fontTools
version, deterministic recipe, PostScript spelling, and table-preservation
invariant are recorded in
`THIRD_PARTY/ubuntu_mono_derivative_bront_font.toml`.

Source and license:

- https://github.com/chrismwendt/bront/tree/aef23d9a11416655a8351230edb3c2377061c077
- https://ubuntu.com/legal/font-licence

## Unicode 16.0 Data Files

The terminal width policy is pinned to Unicode 16.0.0 data files published by
the Unicode Consortium. These files are not vendored in the repository.
Generated table artifacts record the exact input URLs and hashes used.

Unicode data files are governed by the Unicode License v3. The source index is:

- https://www.unicode.org/Public/16.0.0/
