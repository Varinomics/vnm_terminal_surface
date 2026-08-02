#include "vnm_terminal/internal/wheel_gesture.h"

#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vnm_terminal::internal {

namespace {

constexpr qreal k_font_zoom_min_pixel_size = 6.0;
constexpr qreal k_font_zoom_max_pixel_size = 72.0;
constexpr qreal k_font_zoom_wheel_step     = 1.0;

int wheel_steps_from_delta(int delta, qreal step_size, qreal& remainder)
{
    if (delta == 0 || !std::isfinite(step_size) || step_size <= 0.0) {
        return 0;
    }

    remainder += static_cast<qreal>(delta);
    const int steps = static_cast<int>(std::trunc(remainder / step_size));
    remainder -= static_cast<qreal>(steps) * step_size;
    return steps;
}

}

bool has_vertical_wheel_delta(const QWheelEvent& event)
{
    return event.angleDelta().y() != 0 || event.pixelDelta().y() != 0;
}

int vertical_wheel_steps(
    const QWheelEvent&  event,
    qreal               pixel_step_size,
    qreal&              angle_remainder,
    qreal&              pixel_remainder)
{
    const int angle_delta = event.angleDelta().y();
    if (angle_delta != 0) {
        pixel_remainder = 0.0;
        return wheel_steps_from_delta(
            angle_delta,
            k_angle_delta_per_wheel_step,
            angle_remainder);
    }

    const int pixel_delta = event.pixelDelta().y();
    if (pixel_delta == 0) {
        return 0;
    }

    angle_remainder = 0.0;
    return wheel_steps_from_delta(pixel_delta, pixel_step_size, pixel_remainder);
}

qreal font_size_after_wheel_zoom(qreal current_font_size, int steps)
{
    const qreal base_font_size = std::isfinite(current_font_size) && current_font_size > 0.0
        ? current_font_size
        : static_cast<qreal>(k_vnm_terminal_default_font_pixel_size);

    return std::clamp(
        base_font_size + static_cast<qreal>(steps) * k_font_zoom_wheel_step,
        k_font_zoom_min_pixel_size,
        k_font_zoom_max_pixel_size);
}

}
