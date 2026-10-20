#include "vnm_terminal/internal/wheel_gesture.h"

#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vnm_terminal::internal {

namespace {

constexpr qreal k_font_zoom_wheel_step = 1.0;

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

qreal font_size_after_wheel_zoom(
    qreal                             current_font_size,
    int                               steps,
    const QString&                    font_family,
    qreal                             device_pixel_ratio,
    vnm_terminal::Font_advance_policy advance_policy,
    qreal                             logical_dpi,
    Terminal_text_renderer_policy     renderer_policy)
{
    // What the text paths draw a requested size at, from the render font the surface
    // builds for it. Requests compare by what the renderer consumes, never by their
    // effective size: Qt resolves the render font to a whole pixel size, and the MSDF
    // path draws from the cell ascent, so differing effective sizes can draw alike.
    const auto draw_sizes = [&](qreal font_size) {
        const qreal effective_font_size = vnm_terminal::effective_font_size_for_font(
            font_family, font_size, device_pixel_ratio, advance_policy, logical_dpi);
        return qsg_atlas_text_draw_sizes(
            vnm_terminal_font(font_family, effective_font_size, logical_dpi),
            device_pixel_ratio,
            advance_policy,
            logical_dpi);
    };
    const auto draws_differently = [renderer_policy](
        const qsg_atlas_text_draw_sizes_t& lhs,
        const qsg_atlas_text_draw_sizes_t& rhs)
    {
        return qsg_atlas_text_draws_differently(lhs, rhs, renderer_policy);
    };
    const qreal step = steps < 0 ? -k_font_zoom_wheel_step : k_font_zoom_wheel_step;
    const auto next_font_size = [step](qreal font_size) {
        return std::clamp(
            font_size + step,
            k_font_zoom_min_pixel_size,
            k_font_zoom_max_pixel_size);
    };

    qreal font_size = std::isfinite(current_font_size) && current_font_size > 0.0
        ? current_font_size
        : static_cast<qreal>(k_vnm_terminal_default_font_pixel_size);
    for (int notch = 0; notch < std::abs(steps); ++notch) {
        const qsg_atlas_text_draw_sizes_t drawn     = draw_sizes(font_size);
        qreal candidate                             = font_size;
        qsg_atlas_text_draw_sizes_t candidate_drawn = drawn;
        while (!draws_differently(drawn, candidate_drawn)) {
            const qreal next = next_font_size(candidate);
            if (next == candidate) {
                return font_size;
            }
            candidate       = next;
            candidate_drawn = draw_sizes(candidate);
        }

        // Zooming out reaches the largest size of the new rendering first; zooming in
        // reaches its smallest, so it continues to the largest.
        if (step > 0) {
            for (qreal next = next_font_size(candidate);
                 next != candidate && !draws_differently(candidate_drawn, draw_sizes(next));
                 next = next_font_size(candidate))
            {
                candidate = next;
            }
        }
        font_size = candidate;
    }
    return font_size;
}

}
