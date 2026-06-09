#include "vnm_terminal/font_metrics.h"

#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

namespace vnm_terminal {

QString default_monospace_font_family()
{
    return internal::vnm_terminal_default_monospace_font_family();
}

Cell_metrics cell_metrics_for_font(
    const QString& family,
    qreal          pixel_size,
    qreal          device_pixel_ratio)
{
    const internal::Qt_grid_metrics_provider provider(
        internal::vnm_terminal_font(family, pixel_size),
        device_pixel_ratio);
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
