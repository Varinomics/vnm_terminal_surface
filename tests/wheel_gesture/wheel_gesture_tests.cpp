// Oracles for these fixtures:
//   - Qt documents one physical wheel notch as 120 units of angle delta, which fixes the
//     angle-to-notch conversion and the sub-notch remainder carry.
//   - The zoom guard is fixed by the fontSize property contract, not by taste:
//     normalized_font_pixel_size passes non-finite and non-positive sizes through
//     unchanged, so fontSize can hold NaN, zero, a negative, or an infinity, and the zoom
//     has to produce a size the terminal can render from any of them. The surface has
//     applied that guard since before this seam existed; the fixtures pin it so the
//     scrollbar, which is the other caller, cannot drift away from it again.

#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/wheel_gesture.h"
#include "helpers/test_check.h"

#include <QPoint>
#include <QPointF>
#include <QWheelEvent>

#include <cmath>
#include <iostream>
#include <limits>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

QWheelEvent make_wheel_event(QPoint angle_delta, QPoint pixel_delta)
{
    return QWheelEvent(
        QPointF(0.0, 0.0),
        QPointF(0.0, 0.0),
        pixel_delta,
        angle_delta,
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
}

bool test_angle_delta_steps()
{
    bool  ok              = true;
    qreal angle_remainder = 0.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent one_notch = make_wheel_event(QPoint(0, 120), QPoint());
    ok &= check(
        term::vertical_wheel_steps(
            one_notch, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder) == 1,
        "one notch of angle delta is one step");
    ok &= check(angle_remainder == 0.0, "a whole notch leaves no remainder");

    const QWheelEvent three_notches = make_wheel_event(QPoint(0, -360), QPoint());
    ok &= check(
        term::vertical_wheel_steps(
            three_notches, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder)
            == -3,
        "three notches backwards are three negative steps");
    return ok;
}

bool test_sub_notch_deltas_accumulate()
{
    bool  ok              = true;
    qreal angle_remainder = 0.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent partial = make_wheel_event(QPoint(0, 40), QPoint());
    for (int i = 0; i < 2; ++i) {
        ok &= check(
            term::vertical_wheel_steps(
                partial, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder)
                == 0,
            "a sub-notch delta yields no step yet");
    }

    ok &= check(
        term::vertical_wheel_steps(
            partial, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder) == 1,
        "accumulated sub-notch deltas reach a whole step");
    return ok;
}

bool test_pixel_delta_uses_the_caller_step_size()
{
    bool  ok              = true;
    qreal angle_remainder = 7.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent pixels = make_wheel_event(QPoint(), QPoint(0, 45));
    ok &= check(
        term::vertical_wheel_steps(pixels, 15.0, angle_remainder, pixel_remainder) == 3,
        "pixel delta is divided by the caller's step size");
    ok &= check(
        angle_remainder == 0.0,
        "entering the pixel route clears the angle remainder");

    const QWheelEvent none = make_wheel_event(QPoint(), QPoint());
    ok &= check(
        term::vertical_wheel_steps(none, 15.0, angle_remainder, pixel_remainder) == 0,
        "an event with no vertical delta yields no step");
    ok &= check(
        !term::has_vertical_wheel_delta(none),
        "an event with no vertical delta reports none");
    ok &= check(
        term::has_vertical_wheel_delta(pixels),
        "an event with a pixel delta reports a vertical delta");
    return ok;
}

bool test_zoom_steps_from_a_renderable_font_size()
{
    const qreal default_size = static_cast<qreal>(term::k_vnm_terminal_default_font_pixel_size);

    bool ok = true;
    ok &= check(
        term::font_size_after_wheel_zoom(13.0, 1) == 14.0,
        "a positive size steps by one pixel per notch");
    ok &= check(
        term::font_size_after_wheel_zoom(13.0, -1) == 12.0,
        "a negative step count zooms out");
    ok &= check(
        term::font_size_after_wheel_zoom(6.0, -1) == 6.0,
        "zooming out is clamped at the minimum");
    ok &= check(
        term::font_size_after_wheel_zoom(72.0, 1) == 72.0,
        "zooming in is clamped at the maximum");

    // fontSize passes these through, so the zoom must recover from every one of them.
    ok &= check(
        term::font_size_after_wheel_zoom(0.0, 1) == default_size + 1.0,
        "a zero size zooms from the default size");
    ok &= check(
        term::font_size_after_wheel_zoom(-4.0, 1) == default_size + 1.0,
        "a negative size zooms from the default size");
    ok &= check(
        term::font_size_after_wheel_zoom(std::numeric_limits<qreal>::quiet_NaN(), 1)
            == default_size + 1.0,
        "a NaN size zooms from the default size instead of staying NaN");
    ok &= check(
        term::font_size_after_wheel_zoom(std::numeric_limits<qreal>::infinity(), -1)
            == default_size - 1.0,
        "an infinite size zooms from the default size instead of the maximum");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_angle_delta_steps();
    ok &= test_sub_notch_deltas_accumulate();
    ok &= test_pixel_delta_uses_the_caller_step_size();
    ok &= test_zoom_steps_from_a_renderable_font_size();

    if (!ok) {
        std::cerr << "wheel_gesture_tests: FAILED\n";
        return 1;
    }

    std::cout << "wheel_gesture_tests: OK\n";
    return 0;
}
