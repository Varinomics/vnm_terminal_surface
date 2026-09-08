#pragma once

#include <QFont>
#include <QString>

namespace vnm_terminal::internal {

constexpr int k_vnm_terminal_default_font_pixel_size = 13;

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
QFont vnm_terminal_font(QString family, qreal pixel_size);

// Answers a family name a host stored in an earlier release with the family the
// shipped monospace face registers under now, and returns every other family
// unchanged. The face itself never changed - the same Bront outlines under
// successive family names - so a stored name from an earlier release means "the
// shipped face", and taking it literally would silently move the terminal onto
// whatever the host substitutes for a family that no longer exists.
QString vnm_terminal_migrated_font_family(QString family);

}
