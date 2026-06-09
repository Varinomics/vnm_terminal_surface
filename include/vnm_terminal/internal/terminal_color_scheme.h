#pragma once

#include "vnm_terminal/internal/terminal_style.h"

#include <QString>
#include <QStringView>
#include <QtGlobal>

#include <array>
#include <vector>

namespace vnm_terminal::internal {

// A named terminal color scheme in the style of Windows Terminal: the 16 ANSI
// colors (palette slots 0..15 -- eight normal followed by eight bright) plus
// the default foreground, background, cursor, and selection colors. Every value
// is a 0xAARRGGBB quantity, matching Terminal_color_state palette entries.
struct Terminal_color_scheme
{
    QString                  name;
    std::array<quint32, 16>  ansi_palette_rgba = {};
    quint32                  foreground_rgba   = k_terminal_default_foreground_rgba;
    quint32                  background_rgba   = k_terminal_default_background_rgba;
    quint32                  cursor_rgba       = k_terminal_default_cursor_rgba;
    quint32                  selection_rgba    = 0xffffffffU;
};

// The bundled color schemes (the Windows Terminal built-in set) in presentation
// order. The first entry is the default scheme.
const std::vector<Terminal_color_scheme>& builtin_color_schemes();

// The bundled scheme applied when none is selected (Windows Terminal Campbell).
const Terminal_color_scheme& default_color_scheme();

// Looks up a bundled scheme by name (case-insensitive). Returns nullptr when no
// bundled scheme matches the requested name.
const Terminal_color_scheme* find_color_scheme(QStringView name);

// Builds the 256-entry runtime color state for a scheme: palette slots 0..15
// from the scheme's ANSI colors, slots 16..255 from the standard xterm 6x6x6
// color cube and grayscale ramp, and the default foreground/background/cursor
// from the scheme.
Terminal_color_state make_terminal_color_state(const Terminal_color_scheme& scheme);

} // namespace vnm_terminal::internal
