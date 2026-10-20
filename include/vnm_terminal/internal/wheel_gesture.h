#pragma once

#include "vnm_terminal/font_metrics.h"

#include <QString>
#include <QtGlobal>

class QWheelEvent;

namespace vnm_terminal::internal {

enum class Terminal_text_renderer_policy;

// Qt reports one physical wheel notch as 120 units of angle delta.
inline constexpr qreal k_angle_delta_per_wheel_step = 120.0;

// The ctrl+wheel zoom range. A notch moves the font size within it.
inline constexpr qreal k_font_zoom_min_pixel_size = 6.0;
inline constexpr qreal k_font_zoom_max_pixel_size = 72.0;

// An unmodified vertical wheel notch scrolls the viewport by this many terminal lines.
// Pixel-delta devices report their own distance and are not multiplied.
inline constexpr int k_plain_scroll_lines_per_angle_step = 3;

// Vertical wheel gesture normalization shared with the first-party application, whose
// scrollbar sits beside the surface and must answer the same gesture identically. Two
// owners that normalize the same event differently produce a visible discontinuity as
// the pointer crosses between them.

bool has_vertical_wheel_delta(const QWheelEvent& event);

// Converts the event's vertical delta into whole notches, carrying the sub-notch
// remainder in the caller's accumulator so slow trackpad scrolling still advances. The
// caller owns one accumulator pair per gesture route; angle and pixel deltas never
// accumulate together, so entering one route resets the other's remainder.
int vertical_wheel_steps(
    const QWheelEvent&  event,
    qreal               pixel_step_size,
    qreal&              angle_remainder,
    qreal&              pixel_remainder);

// The font size that a ctrl+wheel zoom of `steps` notches produces from
// `current_font_size`, for a terminal drawing `font_family` at `device_pixel_ratio`
// and `logical_dpi` with `advance_policy` and `renderer_policy`.
//
// Every notch changes how every text run is drawn. Different requests can draw alike:
// under ADJUST_FONT_SIZE neighbouring sizes round up to one device-pixel advance, Qt
// resolves the render font to a whole pixel size, and the MSDF path draws from the
// snapped cell ascent rather than from the raster font. A notch therefore moves to the
// next size that qsg_atlas_text_draws_differently() separates under `renderer_policy`,
// and settles on the largest size drawing like it; the adjustment only rounds up, so
// that size is the request nearest to what is shown. Under AUTO a notch has to change
// both paths, so where AUTO ends up drawing glyphs alone it can pass over a size that
// would have changed only the glyphs.
//
// From a whole-pixel size that is the largest drawing alike, a notch and its reverse
// return to it. Any other size can come back as another size drawing like it, and which
// size that is depends on the screen; a fractional size keeps its fraction until the
// end of the zoom range clamps it, so 70.2 steps to 71.2 and 72, and back to 71. The
// result is a request, not the effective size: a host that stores it lets every screen
// derive its own rendering. A notch with no differing size left before the end of the
// zoom range leaves the size unchanged.
//
// A non-finite or non-positive current size steps from the default size instead. The
// fontSize property passes such values through unchanged, so it can legitimately hold
// them: NaN would survive the clamp because both of its comparisons are false, and zero
// or a negative would step from a size the terminal never rendered.
qreal font_size_after_wheel_zoom(
    qreal                             current_font_size,
    int                               steps,
    const QString&                    font_family,
    qreal                             device_pixel_ratio,
    vnm_terminal::Font_advance_policy advance_policy,
    qreal                             logical_dpi,
    Terminal_text_renderer_policy     renderer_policy);

}
