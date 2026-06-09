#pragma once

#include <QByteArray>

#include <optional>

class QFont;

namespace vnm_terminal::internal {

// Returns the complete in-memory font file (a full sfnt blob) for `font`,
// suitable for FreeType / msdfgen. The bundled monospace family is served from
// its Qt resource; any other installed family is resolved to its on-disk font
// file. Returns std::nullopt when the bytes cannot be obtained, in which case
// the caller must fall back to the glyph renderer rather than claim MSDF.
std::optional<QByteArray> font_file_bytes_for_font(const QFont& font);

} // namespace vnm_terminal::internal
