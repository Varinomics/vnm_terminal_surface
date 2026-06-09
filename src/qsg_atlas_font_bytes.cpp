#include "vnm_terminal/internal/qsg_atlas_font_bytes.h"

#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QFile>
#include <QFont>
#include <QString>

#include <algorithm>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vnm_terminal::internal {

namespace {

constexpr const char* k_bundled_monospace_font_resource =
    ":/vnm_terminal_surface/fonts/vnm_framework_monospace.ttf";

std::optional<QByteArray> bundled_font_bytes()
{
    QFile font_file(QString::fromLatin1(k_bundled_monospace_font_resource));
    if (!font_file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QByteArray data = font_file.readAll();
    if (data.isEmpty()) {
        return std::nullopt;
    }

    return data;
}

bool font_is_bundled_family(const QFont& font)
{
    const QString family = font.family().trimmed();
    return family.isEmpty() ||
        family == vnm_terminal_default_monospace_font_family();
}

#if defined(Q_OS_WIN)
// GetFontData(hdc, 0, 0, ...) returns the entire font file for the font selected
// into the device context, which is exactly the sfnt blob FreeType / msdfgen
// consume. GDI resolves the family the same way the rest of the app does, and
// for a collection file it returns the whole collection (msdfgen loads face 0).
std::optional<QByteArray> installed_font_bytes_win(const QFont& font)
{
    HDC hdc = CreateCompatibleDC(nullptr);
    if (hdc == nullptr) {
        return std::nullopt;
    }

    LOGFONTW logfont = {};
    logfont.lfHeight         = -64;
    logfont.lfWeight         = font.bold() ? FW_BOLD : FW_NORMAL;
    logfont.lfItalic         = font.italic() ? TRUE : FALSE;
    logfont.lfCharSet        = DEFAULT_CHARSET;
    logfont.lfOutPrecision   = OUT_TT_PRECIS;
    logfont.lfQuality        = DEFAULT_QUALITY;
    logfont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

    const QString family       = font.family();
    const int     face_length  = std::min<int>(
        family.size(), static_cast<int>(LF_FACESIZE) - 1);
    family.left(face_length).toWCharArray(logfont.lfFaceName);
    logfont.lfFaceName[face_length] = L'\0';

    HFONT hfont = CreateFontIndirectW(&logfont);
    if (hfont == nullptr) {
        DeleteDC(hdc);
        return std::nullopt;
    }

    HGDIOBJ previous = SelectObject(hdc, hfont);

    std::optional<QByteArray> result;
    const DWORD size = GetFontData(hdc, 0, 0, nullptr, 0);
    if (size != GDI_ERROR && size > 0) {
        QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
        const DWORD copied = GetFontData(hdc, 0, 0, data.data(), size);
        if (copied == size) {
            result = std::move(data);
        }
    }

    SelectObject(hdc, previous);
    DeleteObject(hfont);
    DeleteDC(hdc);
    return result;
}
#endif

} // namespace

std::optional<QByteArray> font_file_bytes_for_font(const QFont& font)
{
    if (font_is_bundled_family(font)) {
        if (std::optional<QByteArray> bytes = bundled_font_bytes()) {
            return bytes;
        }
    }

#if defined(Q_OS_WIN)
    if (std::optional<QByteArray> bytes = installed_font_bytes_win(font)) {
        return bytes;
    }
#endif

    return std::nullopt;
}

} // namespace vnm_terminal::internal
