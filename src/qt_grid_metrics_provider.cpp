#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QFontMetricsF>
#include <QLatin1Char>
#include <algorithm>
#include <cmath>
#include <utility>

namespace vnm_terminal::internal {

namespace {

qreal normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return device_pixel_ratio;
}

qreal snap_metric_up_to_device_pixel(qreal value, qreal device_pixel_ratio)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return value;
    }

    const qreal physical_value =
        value * normalized_device_pixel_ratio(device_pixel_ratio);
    return std::ceil(physical_value) /
        normalized_device_pixel_ratio(device_pixel_ratio);
}

qreal snap_metric_to_device_pixel(
    qreal                         value,
    qreal                         device_pixel_ratio,
    vnm_terminal::Font_advance_policy policy)
{
    switch (policy) {
        case vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE:
        case vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP:
            return snap_metric_up_to_device_pixel(value, device_pixel_ratio);
        case vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST:
            break;
    }

    if (!std::isfinite(value) || value <= 0.0) {
        return value;
    }

    const qreal ratio = normalized_device_pixel_ratio(device_pixel_ratio);
    const qreal physical_value = value * ratio;
    const qreal snapped_physical_value = std::max<qreal>(
        1.0,
        std::round(physical_value));
    return snapped_physical_value / ratio;
}

}

Qt_grid_metrics_provider::Qt_grid_metrics_provider() = default;

Qt_grid_metrics_provider::Qt_grid_metrics_provider(
    QFont                            font,
    qreal                            device_pixel_ratio,
    vnm_terminal::Font_advance_policy policy,
    qreal                            logical_dpi)
:
    m_font(std::move(font)),
    m_device_pixel_ratio(normalized_device_pixel_ratio(device_pixel_ratio)),
    m_logical_dpi(normalized_logical_dpi(logical_dpi)),
    m_font_advance_policy(
        vnm_terminal::font_advance_policy_is_valid(policy)
            ? policy
            : vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP)
{}

terminal_cell_metrics_t Qt_grid_metrics_provider::cell_metrics() const
{
    const QFontMetricsF metrics(
        vnm_terminal_font_for_logical_dpi(m_font, m_logical_dpi));
    const qreal width =
        snap_metric_to_device_pixel(
            metrics.horizontalAdvance(QLatin1Char('M')),
            m_device_pixel_ratio,
            m_font_advance_policy);
    const qreal ascent =
        snap_metric_up_to_device_pixel(metrics.ascent(), m_device_pixel_ratio);
    const qreal descent =
        snap_metric_up_to_device_pixel(metrics.descent(), m_device_pixel_ratio);
    const qreal height = std::max(
        snap_metric_up_to_device_pixel(metrics.lineSpacing(), m_device_pixel_ratio),
        ascent + descent);

    return {
        width,
        height,
        ascent,
        descent,
    };
}

Terminal_metrics_result Qt_grid_metrics_provider::grid_size_for_item_geometry(
    QSizeF geometry) const
{
    return grid_size_for_geometry(geometry, cell_metrics());
}

QFont Qt_grid_metrics_provider::font() const
{
    return m_font;
}

void Qt_grid_metrics_provider::set_font(QFont font)
{
    m_font = std::move(font);
}

qreal Qt_grid_metrics_provider::device_pixel_ratio() const
{
    return m_device_pixel_ratio;
}

void Qt_grid_metrics_provider::set_device_pixel_ratio(qreal device_pixel_ratio)
{
    m_device_pixel_ratio = normalized_device_pixel_ratio(device_pixel_ratio);
}

qreal Qt_grid_metrics_provider::logical_dpi() const
{
    return m_logical_dpi;
}

void Qt_grid_metrics_provider::set_logical_dpi(qreal logical_dpi)
{
    m_logical_dpi = normalized_logical_dpi(logical_dpi);
}

vnm_terminal::Font_advance_policy Qt_grid_metrics_provider::font_advance_policy() const
{
    return m_font_advance_policy;
}

void Qt_grid_metrics_provider::set_font_advance_policy(
    vnm_terminal::Font_advance_policy policy)
{
    m_font_advance_policy = vnm_terminal::font_advance_policy_is_valid(policy)
        ? policy
        : vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP;
}

}
