#include "vnm_terminal/font_metrics.h"

#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QFontMetricsF>
#include <QLatin1Char>
#include <algorithm>
#include <cmath>
#include <limits>

namespace vnm_terminal {

namespace {

qreal normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    return std::isfinite(device_pixel_ratio) && device_pixel_ratio > 0.0
        ? device_pixel_ratio
        : 1.0;
}

qreal normalized_effective_font_size(
    const QString&       family,
    qreal                pixel_size,
    qreal                device_pixel_ratio,
    qreal                logical_dpi,
    Font_advance_policy  policy)
{
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0)
    {
        return pixel_size;
    }

    const qreal bounded_pixel_size = std::clamp(
        pixel_size,
        1.0,
        static_cast<qreal>(internal::k_vnm_terminal_max_font_pixel_size));
    if (policy != Font_advance_policy::ADJUST_FONT_SIZE) {
        return bounded_pixel_size;
    }

    const qreal ratio = normalized_device_pixel_ratio(device_pixel_ratio);
    const auto physical_advance_for_size = [&](qreal candidate_size) {
        const QFont candidate_font = internal::vnm_terminal_font(
            family,
            candidate_size,
            logical_dpi);
        const qreal candidate_advance = QFontMetricsF(candidate_font)
            .horizontalAdvance(QLatin1Char('M'));
        return std::isfinite(candidate_advance) && candidate_advance > 0.0
            ? candidate_advance * ratio
            : std::numeric_limits<qreal>::quiet_NaN();
    };
    const qreal physical_advance = physical_advance_for_size(bounded_pixel_size);
    if (!std::isfinite(physical_advance) || physical_advance <= 0.0) {
        return bounded_pixel_size;
    }

    const qreal target_physical_advance = std::max<qreal>(
        1.0,
        std::ceil(physical_advance));
    if (physical_advance >= target_physical_advance) {
        return bounded_pixel_size;
    }

    const qreal maximum_pixel_size =
        static_cast<qreal>(internal::k_vnm_terminal_max_font_pixel_size);
    if (physical_advance_for_size(maximum_pixel_size) < target_physical_advance) {
        return maximum_pixel_size;
    }

    // Locate the first size that Qt/Windows can actually render with the
    // target advance. Fractional point sizes are retained where possible,
    // but the returned value is always tied to a real backend metric rather
    // than a smooth estimate the rasterizer may silently quantize away.
    qreal lower_bound = bounded_pixel_size;
    qreal upper_bound = maximum_pixel_size;
    for (int iteration = 0; iteration < 48; ++iteration) {
        const qreal candidate_size = (lower_bound + upper_bound) / 2.0;
        if (physical_advance_for_size(candidate_size) >= target_physical_advance) {
            upper_bound = candidate_size;
        }
        else {
            lower_bound = candidate_size;
        }
    }
    return upper_bound;
}

} // namespace

qreal effective_font_size_for_font(
    const QString&      family,
    qreal               pixel_size,
    qreal               device_pixel_ratio,
    Font_advance_policy policy,
    qreal               logical_dpi)
{
    if (!font_advance_policy_is_valid(policy)) {
        policy = Font_advance_policy::ADJUST_FONT_SIZE;
    }
    return normalized_effective_font_size(
        family, pixel_size, device_pixel_ratio, logical_dpi, policy);
}

Cell_metrics cell_metrics_for_font(
    const QString& family,
    qreal          pixel_size,
    qreal          device_pixel_ratio,
    Font_advance_policy policy,
    qreal               logical_dpi)
{
    const qreal effective_size = effective_font_size_for_font(
        family, pixel_size, device_pixel_ratio, policy, logical_dpi);
    const internal::Qt_grid_metrics_provider provider(
        internal::vnm_terminal_font(family, effective_size, logical_dpi),
        device_pixel_ratio,
        policy,
        logical_dpi);
    const internal::terminal_cell_metrics_t metrics = provider.cell_metrics();
    return {metrics.width, metrics.height};
}

bool cell_metrics_valid(const Cell_metrics& metrics)
{
    internal::terminal_cell_metrics_t internal_metrics;
    internal_metrics.width  = metrics.width;
    internal_metrics.height = metrics.height;
    return internal::is_valid_cell_metrics(internal_metrics);
}

}
