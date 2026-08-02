#pragma once

#include <QtGlobal>

class QWheelEvent;

namespace vnm_terminal::internal {

// Qt reports one physical wheel notch as 120 units of angle delta.
inline constexpr qreal k_angle_delta_per_wheel_step = 120.0;

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
// `current_font_size`, clamped to the zoom range.
//
// A non-finite or non-positive current size steps from the default size instead. The
// fontSize property passes such values through unchanged, so it can legitimately hold
// them: NaN would survive the clamp because both of its comparisons are false, and zero
// or a negative would step from a size the terminal never rendered.
qreal font_size_after_wheel_zoom(qreal current_font_size, int steps);

}
