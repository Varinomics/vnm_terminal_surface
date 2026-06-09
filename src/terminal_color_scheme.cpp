#include "vnm_terminal/internal/terminal_color_scheme.h"

#include "vnm_terminal/internal/terminal_style.h"

#include <QtGlobal>

#include <array>
#include <cstddef>
#include <vector>

namespace vnm_terminal::internal {

namespace {

// Opaque 0xAARRGGBB from a #RRGGBB literal. The bundled scheme values below are
// transcribed verbatim from the Windows Terminal defaults.json built-in
// schemes, so each color reads exactly as its source hex.
constexpr quint32 c(quint32 rrggbb)
{
    return 0xff000000U | rrggbb;
}

constexpr std::array<int, 6> k_256_cube_components = {
    0, 95, 135, 175, 215, 255,
};

// Standard xterm resolution for palette slots 16..255: the 6x6x6 color cube
// (16..231) and the 24-step grayscale ramp (232..255). Slots 0..15 are
// scheme-defined and never routed here.
quint32 xterm_extended_palette_rgba(int index)
{
    if (index >= 16 && index <= 231) {
        const int cube_index = index - 16;
        const int red        = k_256_cube_components[static_cast<std::size_t>(cube_index / 36)];
        const int green      = k_256_cube_components[static_cast<std::size_t>((cube_index / 6) % 6)];
        const int blue       = k_256_cube_components[static_cast<std::size_t>(cube_index % 6)];
        return rgba_from_components(red, green, blue);
    }

    if (index >= 232 && index <= 255) {
        const int component = 8 + ((index - 232) * 10);
        return rgba_from_components(component, component, component);
    }

    return k_terminal_default_foreground_rgba;
}

std::vector<Terminal_color_scheme> build_builtin_color_schemes()
{
    std::vector<Terminal_color_scheme> schemes;
    schemes.reserve(16);

    schemes.push_back({
        QStringLiteral("Campbell"),
        { c(0x0C0C0C), c(0xC50F1F), c(0x13A10E), c(0xC19C00),
          c(0x0037DA), c(0x881798), c(0x3A96DD), c(0xCCCCCC),
          c(0x767676), c(0xE74856), c(0x16C60C), c(0xF9F1A5),
          c(0x3B78FF), c(0xB4009E), c(0x61D6D6), c(0xF2F2F2) },
        c(0xCCCCCC), c(0x0C0C0C), c(0xFFFFFF), c(0xFFFFFF),
    });

    // The terminal's appearance before color schemes were introduced: pure
    // white-on-black with the classic xterm 16-color palette.
    schemes.push_back({
        QStringLiteral("Classic"),
        { c(0x000000), c(0xCD0000), c(0x00CD00), c(0xCDCD00),
          c(0x0000EE), c(0xCD00CD), c(0x00CDCD), c(0xE5E5E5),
          c(0x7F7F7F), c(0xFF0000), c(0x00FF00), c(0xFFFF00),
          c(0x5C5CFF), c(0xFF00FF), c(0x00FFFF), c(0xFFFFFF) },
        c(0xFFFFFF), c(0x000000), c(0xFFFFFF), c(0x3060A0),
    });

    schemes.push_back({
        QStringLiteral("Campbell Powershell"),
        { c(0x0C0C0C), c(0xC50F1F), c(0x13A10E), c(0xC19C00),
          c(0x0037DA), c(0x881798), c(0x3A96DD), c(0xCCCCCC),
          c(0x767676), c(0xE74856), c(0x16C60C), c(0xF9F1A5),
          c(0x3B78FF), c(0xB4009E), c(0x61D6D6), c(0xF2F2F2) },
        c(0xCCCCCC), c(0x012456), c(0xFFFFFF), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("Vintage"),
        { c(0x000000), c(0x800000), c(0x008000), c(0x808000),
          c(0x000080), c(0x800080), c(0x008080), c(0xC0C0C0),
          c(0x808080), c(0xFF0000), c(0x00FF00), c(0xFFFF00),
          c(0x0000FF), c(0xFF00FF), c(0x00FFFF), c(0xFFFFFF) },
        c(0xC0C0C0), c(0x000000), c(0xFFFFFF), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("One Half Dark"),
        { c(0x282C34), c(0xE06C75), c(0x98C379), c(0xE5C07B),
          c(0x61AFEF), c(0xC678DD), c(0x56B6C2), c(0xDCDFE4),
          c(0x5A6374), c(0xE06C75), c(0x98C379), c(0xE5C07B),
          c(0x61AFEF), c(0xC678DD), c(0x56B6C2), c(0xDCDFE4) },
        c(0xDCDFE4), c(0x282C34), c(0xFFFFFF), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("One Half Light"),
        { c(0x383A42), c(0xE45649), c(0x50A14F), c(0xC18301),
          c(0x0184BC), c(0xA626A4), c(0x0997B3), c(0xFAFAFA),
          c(0x4F525D), c(0xDF6C75), c(0x98C379), c(0xE4C07A),
          c(0x61AFEF), c(0xC577DD), c(0x56B5C1), c(0xFFFFFF) },
        c(0x383A42), c(0xFAFAFA), c(0x4F525D), c(0x383A42),
    });

    schemes.push_back({
        QStringLiteral("Solarized Dark"),
        { c(0x002B36), c(0xDC322F), c(0x859900), c(0xB58900),
          c(0x268BD2), c(0xD33682), c(0x2AA198), c(0xEEE8D5),
          c(0x073642), c(0xCB4B16), c(0x586E75), c(0x657B83),
          c(0x839496), c(0x6C71C4), c(0x93A1A1), c(0xFDF6E3) },
        c(0x839496), c(0x002B36), c(0xFFFFFF), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("Solarized Light"),
        { c(0x002B36), c(0xDC322F), c(0x859900), c(0xB58900),
          c(0x268BD2), c(0xD33682), c(0x2AA198), c(0xEEE8D5),
          c(0x073642), c(0xCB4B16), c(0x586E75), c(0x657B83),
          c(0x839496), c(0x6C71C4), c(0x93A1A1), c(0xFDF6E3) },
        c(0x657B83), c(0xFDF6E3), c(0x002B36), c(0x2C4D57),
    });

    schemes.push_back({
        QStringLiteral("Tango Dark"),
        { c(0x000000), c(0xCC0000), c(0x4E9A06), c(0xC4A000),
          c(0x3465A4), c(0x75507B), c(0x06989A), c(0xD3D7CF),
          c(0x555753), c(0xEF2929), c(0x8AE234), c(0xFCE94F),
          c(0x729FCF), c(0xAD7FA8), c(0x34E2E2), c(0xEEEEEC) },
        c(0xD3D7CF), c(0x000000), c(0xFFFFFF), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("Tango Light"),
        { c(0x000000), c(0xCC0000), c(0x4E9A06), c(0xC4A000),
          c(0x3465A4), c(0x75507B), c(0x06989A), c(0xD3D7CF),
          c(0x555753), c(0xEF2929), c(0x8AE234), c(0xFCE94F),
          c(0x729FCF), c(0xAD7FA8), c(0x34E2E2), c(0xEEEEEC) },
        c(0x555753), c(0xFFFFFF), c(0x000000), c(0x141414),
    });

    schemes.push_back({
        QStringLiteral("Dimidium"),
        { c(0x000000), c(0xCF494C), c(0x60B442), c(0xDB9C11),
          c(0x0575D8), c(0xAF5ED2), c(0x1DB6BB), c(0xBAB7B6),
          c(0x817E7E), c(0xFF643B), c(0x37E57B), c(0xFCCD1A),
          c(0x688DFD), c(0xED6FE9), c(0x32E0FB), c(0xDEE3E4) },
        c(0xBAB7B6), c(0x141414), c(0x37E57B), c(0x8DB8E5),
    });

    schemes.push_back({
        QStringLiteral("Ottosson"),
        { c(0x000000), c(0xBE2C21), c(0x3FAE3A), c(0xBE9A4A),
          c(0x204DBE), c(0xBB54BE), c(0x00A7B2), c(0xBEBEBE),
          c(0x808080), c(0xFF3E30), c(0x58EA51), c(0xFFC944),
          c(0x2F6AFF), c(0xFC74FF), c(0x00E1F0), c(0xFFFFFF) },
        c(0xBEBEBE), c(0x000000), c(0xFFFFFF), c(0x92A4FD),
    });

    schemes.push_back({
        QStringLiteral("Dark+"),
        { c(0x000000), c(0xCD3131), c(0x0DBC79), c(0xE5E510),
          c(0x2472C8), c(0xBC3FBC), c(0x11A8CD), c(0xE5E5E5),
          c(0x666666), c(0xF14C4C), c(0x23D18B), c(0xF5F543),
          c(0x3B8EEA), c(0xD670D6), c(0x29B8DB), c(0xE5E5E5) },
        c(0xCCCCCC), c(0x1E1E1E), c(0x808080), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("VSCode Dark Modern"),
        { c(0x000000), c(0xCD3131), c(0x0DBC79), c(0xE5E510),
          c(0x2472C8), c(0xBC3FBC), c(0x11A8CD), c(0xE5E5E5),
          c(0x666666), c(0xF14C4C), c(0x23D18B), c(0xF5F543),
          c(0x3B8EEA), c(0xD670D6), c(0x29B8DB), c(0xE5E5E5) },
        c(0xCCCCCC), c(0x1F1F1F), c(0xFFFFFF), c(0x264F78),
    });

    schemes.push_back({
        QStringLiteral("VSCode Light Modern"),
        { c(0x000000), c(0xCD3131), c(0x00BC00), c(0x949800),
          c(0x0451A5), c(0xBC05BC), c(0x0598BC), c(0x555555),
          c(0x666666), c(0xCD3131), c(0x14CE14), c(0xB5BA00),
          c(0x0451A5), c(0xBC05BC), c(0x0598BC), c(0xA5A5A5) },
        c(0x3B3B3B), c(0xFFFFFF), c(0x000000), c(0xADD6FF),
    });

    schemes.push_back({
        QStringLiteral("CGA"),
        { c(0x000000), c(0xAA0000), c(0x00AA00), c(0xAA5500),
          c(0x0000AA), c(0xAA00AA), c(0x00AAAA), c(0xAAAAAA),
          c(0x555555), c(0xFF5555), c(0x55FF55), c(0xFFFF55),
          c(0x5555FF), c(0xFF55FF), c(0x55FFFF), c(0xFFFFFF) },
        c(0xAAAAAA), c(0x000000), c(0x00AA00), c(0xFFFFFF),
    });

    schemes.push_back({
        QStringLiteral("IBM 5153"),
        { c(0x000000), c(0xAA0000), c(0x00AA00), c(0xC47E00),
          c(0x0000AA), c(0xAA00AA), c(0x00AAAA), c(0xAAAAAA),
          c(0x555555), c(0xFF5555), c(0x55FF55), c(0xFFFF55),
          c(0x5555FF), c(0xFF55FF), c(0x55FFFF), c(0xFFFFFF) },
        c(0xAAAAAA), c(0x000000), c(0x00AA00), c(0xFFFFFF),
    });

    return schemes;
}

} // namespace

const std::vector<Terminal_color_scheme>& builtin_color_schemes()
{
    static const std::vector<Terminal_color_scheme> schemes = build_builtin_color_schemes();
    return schemes;
}

const Terminal_color_scheme& default_color_scheme()
{
    return builtin_color_schemes().front();
}

const Terminal_color_scheme* find_color_scheme(QStringView name)
{
    for (const Terminal_color_scheme& scheme : builtin_color_schemes()) {
        if (name.compare(scheme.name, Qt::CaseInsensitive) == 0) {
            return &scheme;
        }
    }

    return nullptr;
}

Terminal_color_state make_terminal_color_state(const Terminal_color_scheme& scheme)
{
    Terminal_color_state state;
    state.default_foreground_rgba = scheme.foreground_rgba;
    state.default_background_rgba = scheme.background_rgba;
    state.cursor_rgba             = scheme.cursor_rgba;

    for (std::size_t index = 0; index < state.palette_rgba.size(); ++index) {
        state.palette_rgba[index] = index < scheme.ansi_palette_rgba.size()
            ? scheme.ansi_palette_rgba[index]
            : xterm_extended_palette_rgba(static_cast<int>(index));
    }

    return state;
}

} // namespace vnm_terminal::internal
