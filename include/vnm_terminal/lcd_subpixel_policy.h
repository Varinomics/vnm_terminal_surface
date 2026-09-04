#pragma once

#include <cstdint>

class QScreen;

namespace vnm_terminal {

enum class Lcd_subpixel_order_policy : std::uint8_t
{
    AUTO,
    NONE,
    RGB,
    BGR,
    VRGB,
    VBGR,
};

enum class Resolved_lcd_subpixel_order : std::uint8_t
{
    NONE,
    RGB,
    BGR,
    VRGB,
    VBGR,
};

// AUTO remains an unresolved caller policy. It is evaluated for the supplied
// screen on every call; without a screen, AUTO fails closed to NONE. Fixed
// policies do not require a screen.
Resolved_lcd_subpixel_order resolve_lcd_subpixel_order(
    Lcd_subpixel_order_policy policy,
    const QScreen*            screen = nullptr);

}
