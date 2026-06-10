#pragma once

#include <QByteArray>

#include <optional>

class QFont;

namespace vnm_terminal::internal {

// Returns the complete in-memory font file (a full sfnt blob) for `font`,
// suitable for FreeType / msdfgen. The bundled monospace family is served from
// its Qt resource; any other installed family is resolved to its on-disk font
// file and verified to be the same physical font Qt's glyph path renders (its
// 'head' and 'name' tables must match the QRawFont tables for `font`), so a
// substituted family or mismatched collection face can never reach the baker.
// Returns std::nullopt when the bytes cannot be obtained or verified, in which
// case the caller must fall back to the glyph renderer rather than claim MSDF.
std::optional<QByteArray> font_file_bytes_for_font(const QFont& font);

} // namespace vnm_terminal::internal
