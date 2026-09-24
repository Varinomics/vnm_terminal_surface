#pragma once

#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QQuickWindow>
#include <QScreen>
#include <cmath>

namespace vnm_terminal::internal {

inline qreal window_device_pixel_ratio(const QQuickWindow* window)
{
    const qreal ratio =
        window != nullptr ? window->effectiveDevicePixelRatio() : 1.0;
    return std::isfinite(ratio) && ratio > 0.0 ? ratio : 1.0;
}

inline qreal window_logical_dpi(const QQuickWindow* window)
{
    const QScreen* const screen = window != nullptr ? window->screen() : nullptr;
    return normalized_logical_dpi(
        screen != nullptr
            ? screen->logicalDotsPerInch()
            : k_vnm_terminal_default_logical_dpi);
}

}
