#pragma once

#include <QFont>
#include <QString>

namespace vnm_terminal::internal {

constexpr int k_vnm_terminal_default_font_pixel_size = 13;
constexpr qreal k_vnm_terminal_default_logical_dpi = 96.0;

// The largest pixel size the terminal will build a font at. fontSize is a
// public property, so a host can hand it any finite number, and converting a
// floating value whose truncated form does not fit an int is undefined - the
// bound has to exist before the conversion rather than after it. The value is a
// deliberate product ceiling rather than int's range: a cell taller than this
// exceeds any display the terminal is used on, and the glyph atlas would still
// be asked to rasterize it.
constexpr int k_vnm_terminal_max_font_pixel_size = 1024;

QString vnm_terminal_default_monospace_font_family();
bool vnm_terminal_default_monospace_font_loaded();
qreal normalized_logical_dpi(qreal logical_dpi);
QFont vnm_terminal_font(
    QString family,
    qreal   pixel_size,
    qreal   logical_dpi = k_vnm_terminal_default_logical_dpi);
QFont vnm_terminal_font_for_logical_dpi(QFont font, qreal logical_dpi);

}
