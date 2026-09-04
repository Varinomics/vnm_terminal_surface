#pragma once

#include "vnm_terminal/lcd_subpixel_policy.h"

#include <optional>

namespace vnm_terminal::internal {

struct windows_font_smoothing_settings_t
{
    bool         enabled     = false;
    unsigned int type        = 0U;
    unsigned int orientation = 0U;
};

std::optional<Resolved_lcd_subpixel_order>
resolved_lcd_subpixel_order_from_qt_hint(int hint);

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_settings(
    const std::optional<windows_font_smoothing_settings_t>& settings);

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_sources(
    const std::optional<Resolved_lcd_subpixel_order>&        screen_order,
    int                                                     rotation_degrees,
    const std::optional<windows_font_smoothing_settings_t>& settings);

}
