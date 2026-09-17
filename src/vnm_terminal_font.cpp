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

// The family is whatever vnm_fonts registered, never a name spelled here. The
// derivative is intentionally registered under the exact unmarked family
// "Ubuntu Sans Mono derivative vnm", so returning the registry result keeps
// this owner aligned with the shared font definition.
Default_monospace_font load_shipped_monospace_font()
{
    init_vnm_terminal_resources();

    const vnm_fonts::Registered_font shipped_font =
        vnm_fonts::register_shipped_font(
            vnm_fonts::Shipped_font::UBUNTU_SANS_MONO_DERIVATIVE_VNM_REGULAR);
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

}

QString vnm_terminal_default_monospace_font_family()
{
    return default_monospace_font().family;
}

bool vnm_terminal_default_monospace_font_loaded()
{
    return default_monospace_font().embedded_resource_loaded;
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
