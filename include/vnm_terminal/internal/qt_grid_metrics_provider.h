#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/font_metrics.h"
#include <QFont>

namespace vnm_terminal::internal {

class Qt_grid_metrics_provider final : public Terminal_grid_metrics_provider
{
public:
    Qt_grid_metrics_provider();
    Qt_grid_metrics_provider(
        QFont                font,
        qreal                device_pixel_ratio,
        vnm_terminal::Font_advance_policy font_advance_policy =
            vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP,
        qreal                logical_dpi = vnm_terminal::k_default_logical_dpi);

    terminal_cell_metrics_t cell_metrics() const override;
    Terminal_metrics_result grid_size_for_item_geometry(QSizeF geometry) const override;

    QFont font() const;
    void set_font(QFont font);

    qreal device_pixel_ratio() const;
    void set_device_pixel_ratio(qreal device_pixel_ratio);

    qreal logical_dpi() const;
    void set_logical_dpi(qreal logical_dpi);

    vnm_terminal::Font_advance_policy font_advance_policy() const;
    void set_font_advance_policy(vnm_terminal::Font_advance_policy policy);

private:
    QFont  m_font;
    qreal  m_device_pixel_ratio = 1.0;
    qreal  m_logical_dpi = vnm_terminal::k_default_logical_dpi;
    vnm_terminal::Font_advance_policy m_font_advance_policy =
        vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP;
};

}
