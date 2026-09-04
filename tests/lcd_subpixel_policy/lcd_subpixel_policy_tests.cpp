// Oracles for these fixtures:
//   - Qt's QPlatformScreen subpixel hint vocabulary fixes the five effective
//     results.
//   - Windows documents ClearType as smoothing type 2 and its LCD orientation
//     values as BGR=0 and RGB=1. Disabled, unavailable, and unknown settings
//     cannot safely select an LCD shader order and therefore resolve to NONE.

#include "vnm_terminal/internal/lcd_subpixel_policy_platform.h"
#include "vnm_terminal/lcd_subpixel_policy.h"
#include "helpers/test_check.h"

#include <qpa/qplatformscreen.h>

#include <iostream>
#include <optional>

namespace term = vnm_terminal;
namespace term_internal = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr unsigned int k_documented_windows_cleartype       = 2U;
constexpr unsigned int k_documented_windows_orientation_bgr = 0U;
constexpr unsigned int k_documented_windows_orientation_rgb = 1U;

bool test_fixed_policies_do_not_require_a_screen()
{
    bool ok = true;
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::NONE) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "fixed NONE resolves without a screen");
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::RGB) ==
            term::Resolved_lcd_subpixel_order::RGB,
        "fixed RGB resolves without a screen");
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::BGR) ==
            term::Resolved_lcd_subpixel_order::BGR,
        "fixed BGR resolves without a screen");
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::VRGB) ==
            term::Resolved_lcd_subpixel_order::VRGB,
        "fixed VRGB resolves without a screen");
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::VBGR) ==
            term::Resolved_lcd_subpixel_order::VBGR,
        "fixed VBGR resolves without a screen");
    return ok;
}

bool test_auto_and_unknown_policy_fail_closed()
{
    bool ok = true;
    ok &= check(
        term::resolve_lcd_subpixel_order(term::Lcd_subpixel_order_policy::AUTO) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "AUTO without a screen resolves to NONE");
    ok &= check(
        term::resolve_lcd_subpixel_order(
            static_cast<term::Lcd_subpixel_order_policy>(255U)) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "an unknown requested policy resolves to NONE");
    return ok;
}

bool test_qt_screen_hint_vocabulary()
{
    bool ok = true;

    const auto none = term_internal::resolved_lcd_subpixel_order_from_qt_hint(
        QPlatformScreen::Subpixel_None);
    const auto rgb = term_internal::resolved_lcd_subpixel_order_from_qt_hint(
        QPlatformScreen::Subpixel_RGB);
    const auto bgr = term_internal::resolved_lcd_subpixel_order_from_qt_hint(
        QPlatformScreen::Subpixel_BGR);
    const auto vrgb = term_internal::resolved_lcd_subpixel_order_from_qt_hint(
        QPlatformScreen::Subpixel_VRGB);
    const auto vbgr = term_internal::resolved_lcd_subpixel_order_from_qt_hint(
        QPlatformScreen::Subpixel_VBGR);

    ok &= check(
        none == term::Resolved_lcd_subpixel_order::NONE,
        "Qt's NONE screen hint maps to NONE");
    ok &= check(
        rgb == term::Resolved_lcd_subpixel_order::RGB,
        "Qt's RGB screen hint maps to RGB");
    ok &= check(
        bgr == term::Resolved_lcd_subpixel_order::BGR,
        "Qt's BGR screen hint maps to BGR");
    ok &= check(
        vrgb == term::Resolved_lcd_subpixel_order::VRGB,
        "Qt's VRGB screen hint maps to VRGB");
    ok &= check(
        vbgr == term::Resolved_lcd_subpixel_order::VBGR,
        "Qt's VBGR screen hint maps to VBGR");
    ok &= check(
        !term_internal::resolved_lcd_subpixel_order_from_qt_hint(-1).has_value(),
        "an unknown Qt screen hint is rejected");
    return ok;
}

bool test_windows_smoothing_settings()
{
    using settings_t = term_internal::windows_font_smoothing_settings_t;

    bool ok = true;
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            std::nullopt) == term::Resolved_lcd_subpixel_order::NONE,
        "an unavailable Windows smoothing query resolves to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{false, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "disabled Windows smoothing resolves to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{true, 1U,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "non-ClearType Windows smoothing resolves to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{true, 255U, k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "an unknown Windows smoothing type resolves to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::RGB,
        "Windows RGB ClearType resolves to RGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_bgr}) ==
            term::Resolved_lcd_subpixel_order::BGR,
        "Windows BGR ClearType resolves to BGR");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_settings(
            settings_t{true, k_documented_windows_cleartype, 2U}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "an unknown Windows ClearType orientation resolves to NONE");
    return ok;
}

bool test_windows_smoothing_gates_the_per_screen_layout()
{
    using settings_t = term_internal::windows_font_smoothing_settings_t;

    bool ok = true;
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::RGB,
            0,
            settings_t{false, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "disabled Windows smoothing gates an RGB screen layout to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::BGR,
            0,
            settings_t{true, 1U, k_documented_windows_orientation_bgr}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "non-ClearType Windows smoothing gates a BGR screen layout to NONE");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::RGB,
            90,
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::VRGB,
        "a 90-degree screen rotation maps RGB to VRGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::RGB,
            270,
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_bgr}) ==
            term::Resolved_lcd_subpixel_order::VBGR,
        "a 270-degree screen rotation maps RGB to VBGR");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::BGR,
            180,
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::RGB,
        "a 180-degree screen rotation reverses BGR to RGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            std::nullopt,
            0,
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "an unavailable per-screen layout fails closed");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            term::Resolved_lcd_subpixel_order::RGB,
            -1,
            settings_t{true, k_documented_windows_cleartype,
                k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::NONE,
        "an unknown screen rotation fails closed");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_fixed_policies_do_not_require_a_screen();
    ok &= test_auto_and_unknown_policy_fail_closed();
    ok &= test_qt_screen_hint_vocabulary();
    ok &= test_windows_smoothing_settings();
    ok &= test_windows_smoothing_gates_the_per_screen_layout();

    if (!ok) {
        std::cerr << "lcd_subpixel_policy_tests: FAILED\n";
        return 1;
    }

    std::cout << "lcd_subpixel_policy_tests: OK\n";
    return 0;
}
