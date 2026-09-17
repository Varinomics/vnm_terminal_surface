#pragma once

#include "vnm_terminal/lcd_subpixel_policy.h"

#include <cstdint>
#include <optional>

namespace vnm_terminal::internal {

// Why four states: an identified monitor without an override may fall back
// to the global orientation, an explicit Flat/RGB/BGR value is the monitor's
// answer, and both an unidentified monitor and a malformed value mean the
// layout is genuinely unknown, which must fail closed instead of borrowing
// an unrelated global answer.
enum class Windows_screen_layout_state : std::uint8_t
{
    UNAVAILABLE,
    ABSENT,
    INVALID,
    RESOLVED,
};

struct windows_screen_layout_t
{
    Windows_screen_layout_state state = Windows_screen_layout_state::UNAVAILABLE;
    Resolved_lcd_subpixel_order order = Resolved_lcd_subpixel_order::NONE;
};

struct windows_lcd_orientation_query_t
{
    // The raw SPI_GETFONTSMOOTHINGORIENTATION result (0 = BGR, 1 = RGB), or
    // std::nullopt when the query failed. Absence must never be read as a
    // value: zero is a real BGR answer, not a default.
    std::optional<unsigned int> orientation;
};

std::optional<Resolved_lcd_subpixel_order>
resolved_lcd_subpixel_order_from_qt_hint(int hint);

std::optional<Resolved_lcd_subpixel_order>
resolved_lcd_subpixel_order_from_windows_orientation(
    const windows_lcd_orientation_query_t& query);

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_sources(
    const windows_screen_layout_t&          screen_layout,
    int                                     rotation_degrees,
    const windows_lcd_orientation_query_t&  orientation_query);

}
