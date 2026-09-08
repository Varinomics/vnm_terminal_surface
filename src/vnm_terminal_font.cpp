#include "vnm_terminal/internal/vnm_terminal_font.h"

#include "vnm_terminal/font_metrics.h"

#include <vnm_font_namespace.h>

#include <QtGlobal>

#include <algorithm>
#include <cmath>

static void init_vnm_terminal_resources()
{
    Q_INIT_RESOURCE(vnm_terminal_surface);
}

namespace vnm_terminal::internal {

namespace {

struct Default_monospace_font
{
    QString family;
    bool    embedded_resource_loaded = false;
};

// The family is whatever vnm_fonts registered, never a name spelled here: the
// library marks the family at load time so an installed Ubuntu Mono - Bront
// cannot merge with the shipped one, and a literal copied into this file would
// drift from that mark the moment it changed.
Default_monospace_font load_shipped_monospace_font()
{
    init_vnm_terminal_resources();

    const vnm_fonts::Registered_font shipped_font =
        vnm_fonts::register_shipped_font(vnm_fonts::Shipped_font::UBUNTU_MONO_BRONT);
    if (!shipped_font.is_valid()) {
        return {QStringLiteral("monospace"), false};
    }

    return {shipped_font.family, true};
}

const Default_monospace_font& default_monospace_font()
{
    static const Default_monospace_font font = load_shipped_monospace_font();
    return font;
}

// Family names the shipped monospace face carried in earlier releases. Both
// entries are load-bearing and neither is dead: the terminal shipped the
// verbatim upstream file, whose own family is "Ubuntu Mono - Bront", until
// August 2026, and a renamed derivative, "Ubuntu Mono derivative Bront", from
// then until the face moved to vnm_fonts. A user who last chose a font under
// either release has that exact string in their settings.
//
// The upstream name is here even though an installed copy can still declare it.
// Answering it with the shipped family is the point rather than a side effect:
// an unmarked family name is precisely the ambiguity vnm_fonts removes, because
// two files claiming one name merge into a single font-database entry.
constexpr const char* k_superseded_monospace_font_families[] = {
    "Ubuntu Mono - Bront",
    "Ubuntu Mono derivative Bront",
};

}

QString vnm_terminal_default_monospace_font_family()
{
    return default_monospace_font().family;
}

bool vnm_terminal_default_monospace_font_loaded()
{
    return default_monospace_font().embedded_resource_loaded;
}

QString vnm_terminal_migrated_font_family(QString family)
{
    const QString trimmed = family.trimmed();
    for (const char* superseded : k_superseded_monospace_font_families) {
        if (trimmed == QLatin1String(superseded)) {
            return vnm_terminal_default_monospace_font_family();
        }
    }

    return family;
}

QFont vnm_terminal_font(QString family, qreal pixel_size)
{
    if (family.trimmed().isEmpty()) {
        family = vnm_terminal_default_monospace_font_family();
    }

    QFont font(family);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    if (std::isfinite(pixel_size) && pixel_size > 0.0) {
        const qreal bounded_pixel_size = std::min(
            pixel_size,
            static_cast<qreal>(k_vnm_terminal_max_font_pixel_size));
        font.setPixelSize(std::max(1, static_cast<int>(std::round(bounded_pixel_size))));
    }
    return font;
}

}

namespace vnm_terminal {

QString default_monospace_font_family()
{
    return internal::vnm_terminal_default_monospace_font_family();
}

}
