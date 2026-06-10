#include "vnm_terminal/internal/qsg_atlas_font_bytes.h"

#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QFile>
#include <QFont>
#include <QString>

#if defined(Q_OS_WIN)
#include <QRawFont>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite_3.h>
#include <string>
#include <vector>
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

constexpr quint32 sfnt_tag(char a, char b, char c, char d)
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) |
           (quint32(quint8(c)) << 8)  |  quint32(quint8(d));
}

std::optional<quint32> read_be_u32(const QByteArray& blob, qsizetype offset)
{
    if (offset < 0 || offset + 4 > blob.size()) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const quint8*>(blob.constData()) + offset;
    return (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16) |
           (quint32(bytes[2]) << 8)  |  quint32(bytes[3]);
}

std::optional<quint16> read_be_u16(const QByteArray& blob, qsizetype offset)
{
    if (offset < 0 || offset + 2 > blob.size()) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const quint8*>(blob.constData()) + offset;
    return quint16((quint16(bytes[0]) << 8) | quint16(bytes[1]));
}

// Extracts a top-level sfnt table from the first face of `blob`, which may be
// a bare sfnt or a 'ttcf' collection (the first face is the one msdfgen
// loads). Any structural inconsistency reads as "table absent" so a malformed
// blob can never pass the identity check below.
std::optional<QByteArray> sfnt_first_face_table(const QByteArray& blob, quint32 tag)
{
    qsizetype face_offset = 0;
    const std::optional<quint32> magic = read_be_u32(blob, 0);
    if (!magic.has_value()) {
        return std::nullopt;
    }
    if (*magic == sfnt_tag('t', 't', 'c', 'f')) {
        const std::optional<quint32> first_face_offset = read_be_u32(blob, 12);
        if (!first_face_offset.has_value()) {
            return std::nullopt;
        }
        face_offset = qsizetype(*first_face_offset);
    }

    const std::optional<quint16> table_count = read_be_u16(blob, face_offset + 4);
    if (!table_count.has_value()) {
        return std::nullopt;
    }

    for (quint16 i = 0; i < *table_count; ++i) {
        const qsizetype record = face_offset + 12 + qsizetype(i) * 16;
        const std::optional<quint32> record_tag = read_be_u32(blob, record);
        if (!record_tag.has_value()) {
            return std::nullopt;
        }
        if (*record_tag != tag) {
            continue;
        }

        const std::optional<quint32> offset = read_be_u32(blob, record + 8);
        const std::optional<quint32> length = read_be_u32(blob, record + 12);
        if (!offset.has_value() || !length.has_value() || *length == 0 ||
            quint64(*offset) + quint64(*length) > quint64(blob.size()))
        {
            return std::nullopt;
        }
        return blob.mid(qsizetype(*offset), qsizetype(*length));
    }

    return std::nullopt;
}

// The resolved blob must be the same physical font Qt's glyph path renders;
// otherwise MSDF bakes one font while the glyph path draws another. (The GDI
// resolver this check replaced let the font mapper substitute Arial for any
// family it could not match, which put a proportional fallback into monospace
// cells.) The required 'head' and 'name' tables identify the physical face:
// equality with the tables Qt reports for the same QFont proves both
// pipelines use one font.
bool resolved_bytes_match_rendered_font(const QByteArray& blob, const QFont& font)
{
    const QRawFont raw_font = QRawFont::fromFont(font);
    if (!raw_font.isValid()) {
        return false;
    }

    const QByteArray rendered_head = raw_font.fontTable("head");
    const QByteArray rendered_name = raw_font.fontTable("name");
    if (rendered_head.isEmpty() || rendered_name.isEmpty()) {
        return false;
    }

    return sfnt_first_face_table(blob, sfnt_tag('h', 'e', 'a', 'd')) == rendered_head &&
           sfnt_first_face_table(blob, sfnt_tag('n', 'a', 'm', 'e')) == rendered_name;
}

// Minimal scoped reference for the DirectWrite objects below.
template <typename T>
class Com_ptr
{
public:
    Com_ptr() = default;

    ~Com_ptr()
    {
        if (m_ptr != nullptr) {
            m_ptr->Release();
        }
    }

    Com_ptr(const Com_ptr&) = delete;
    Com_ptr& operator=(const Com_ptr&) = delete;

    T** out() { return &m_ptr; }
    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T* m_ptr = nullptr;
};

// A variable font's named instances all share one physical file, so the
// table-identity check above cannot tell them apart, while msdfgen always
// bakes the file's default instance. Only the default instance is therefore
// MSDF-reproducible: every axis value of the face must equal that axis's
// default. A non-default instance (a variable Bold, a Light) reads as
// unavailable and stays on the glyph path that renders it correctly.
bool face_is_default_variable_instance(IDWriteFontFace5* face)
{
    Com_ptr<IDWriteFontResource> resource;
    if (FAILED(face->GetFontResource(resource.out()))) {
        return false;
    }

    const UINT32 value_count = face->GetFontAxisValueCount();
    std::vector<DWRITE_FONT_AXIS_VALUE> values(value_count);
    if (value_count > 0 &&
        FAILED(face->GetFontAxisValues(values.data(), value_count)))
    {
        return false;
    }

    const UINT32 default_count = resource->GetFontAxisCount();
    std::vector<DWRITE_FONT_AXIS_VALUE> defaults(default_count);
    if (default_count > 0 &&
        FAILED(resource->GetDefaultFontAxisValues(defaults.data(), default_count)))
    {
        return false;
    }

    for (const DWRITE_FONT_AXIS_VALUE& value : values) {
        bool at_default = false;
        for (const DWRITE_FONT_AXIS_VALUE& axis_default : defaults) {
            if (axis_default.axisTag == value.axisTag) {
                at_default = axis_default.value == value.value;
                break;
            }
        }
        if (!at_default) {
            return false;
        }
    }
    return true;
}

// Resolves `font` through DirectWrite, the same font database Qt's family
// names come from, so families whose DirectWrite name differs from their
// legacy GDI face name (family "OCR A", face "OCR A Extended") resolve to the
// correct file. Faces the MSDF baker cannot reproduce faithfully are rejected
// instead of approximated: missing families, synthesized bold/oblique,
// collection faces other than the first (msdfgen loads face 0 of the blob),
// non-default variable instances, and fonts not backed by a local file.
std::optional<QByteArray> installed_font_bytes_win(const QFont& font)
{
    Com_ptr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.out()))))
    {
        return std::nullopt;
    }

    Com_ptr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(collection.out(), FALSE))) {
        return std::nullopt;
    }

    const std::wstring family_name = font.family().toStdWString();
    UINT32 family_index  = 0;
    BOOL   family_exists = FALSE;
    if (FAILED(collection->FindFamilyName(
            family_name.c_str(), &family_index, &family_exists)) ||
        !family_exists)
    {
        return std::nullopt;
    }

    Com_ptr<IDWriteFontFamily> family;
    if (FAILED(collection->GetFontFamily(family_index, family.out()))) {
        return std::nullopt;
    }

    Com_ptr<IDWriteFont> matched_font;
    if (FAILED(family->GetFirstMatchingFont(
            font.bold() ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            font.italic() ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            matched_font.out())))
    {
        return std::nullopt;
    }

    Com_ptr<IDWriteFontFace> face;
    if (FAILED(matched_font->CreateFontFace(face.out()))) {
        return std::nullopt;
    }
    if (face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
        return std::nullopt;
    }
    if (face->GetIndex() != 0) {
        return std::nullopt;
    }

    Com_ptr<IDWriteFontFace5> face5;
    if (FAILED(face->QueryInterface(
            __uuidof(IDWriteFontFace5), reinterpret_cast<void**>(face5.out()))))
    {
        return std::nullopt;
    }
    if (face5->HasVariations() && !face_is_default_variable_instance(face5.get())) {
        return std::nullopt;
    }

    UINT32 file_count = 0;
    if (FAILED(face->GetFiles(&file_count, nullptr)) || file_count != 1) {
        return std::nullopt;
    }
    Com_ptr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&file_count, file.out()))) {
        return std::nullopt;
    }

    const void* reference_key      = nullptr;
    UINT32      reference_key_size = 0;
    if (FAILED(file->GetReferenceKey(&reference_key, &reference_key_size))) {
        return std::nullopt;
    }

    Com_ptr<IDWriteFontFileLoader> loader;
    if (FAILED(file->GetLoader(loader.out()))) {
        return std::nullopt;
    }
    Com_ptr<IDWriteLocalFontFileLoader> local_loader;
    if (FAILED(loader->QueryInterface(
            __uuidof(IDWriteLocalFontFileLoader),
            reinterpret_cast<void**>(local_loader.out()))))
    {
        return std::nullopt;
    }

    UINT32 path_length = 0;
    if (FAILED(local_loader->GetFilePathLengthFromKey(
            reference_key, reference_key_size, &path_length)))
    {
        return std::nullopt;
    }
    std::vector<wchar_t> path(path_length + 1, L'\0');
    if (FAILED(local_loader->GetFilePathFromKey(
            reference_key, reference_key_size, path.data(), path_length + 1)))
    {
        return std::nullopt;
    }

    QFile font_file(QString::fromWCharArray(path.data()));
    if (!font_file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QByteArray data = font_file.readAll();
    if (data.isEmpty()) {
        return std::nullopt;
    }

    return data;
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
        if (resolved_bytes_match_rendered_font(*bytes, font)) {
            return bytes;
        }
    }
#endif

    return std::nullopt;
}

} // namespace vnm_terminal::internal
