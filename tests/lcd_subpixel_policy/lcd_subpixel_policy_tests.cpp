// Oracles for these fixtures:
//   - Qt's QPlatformScreen subpixel hint vocabulary fixes the five effective
//     results.
//   - Windows documents its LCD orientation values as BGR=0 and RGB=1; an
//     absent query result carries no answer at all, because zero is a valid
//     BGR answer rather than a default.
//   - A per-monitor layout takes precedence over the global orientation, an
//     explicit Flat (NONE) monitor answer is never substituted, and the
//     global orientation serves only as a fallback when an identified monitor
//     reports no layout. An unidentified monitor, a malformed value, and any
//     unrecognized state fail closed without fallback.

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

bool test_windows_orientation_query_mapping()
{
    using query_t = term_internal::windows_lcd_orientation_query_t;

    bool ok = true;
    ok &= check(
        !term_internal::resolved_lcd_subpixel_order_from_windows_orientation(
            query_t{std::nullopt}).has_value(),
        "an unqueried Windows orientation yields no order");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_orientation(
            query_t{k_documented_windows_orientation_rgb}) ==
            term::Resolved_lcd_subpixel_order::RGB,
        "a Windows RGB orientation resolves to RGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_orientation(
            query_t{k_documented_windows_orientation_bgr}) ==
            term::Resolved_lcd_subpixel_order::BGR,
        "a Windows BGR orientation resolves to BGR");
    ok &= check(
        !term_internal::resolved_lcd_subpixel_order_from_windows_orientation(
            query_t{2U}).has_value(),
        "an unknown Windows orientation value is rejected");
    return ok;
}

bool test_windows_sources_precedence_and_rotation()
{
    using query_t       = term_internal::windows_lcd_orientation_query_t;
    using layout_t      = term_internal::windows_screen_layout_t;
    using layout_state  = term_internal::Windows_screen_layout_state;
    using order         = term::Resolved_lcd_subpixel_order;

    const auto resolved_layout = [](order value) {
        return layout_t{layout_state::RESOLVED, value};
    };
    const layout_t absent_layout{layout_state::ABSENT};
    const layout_t invalid_layout{layout_state::INVALID};
    const layout_t unavailable_layout{layout_state::UNAVAILABLE};

    bool ok = true;
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            unavailable_layout,
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::NONE,
        "an unidentified monitor fails closed without fallback");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            layout_t{static_cast<layout_state>(255U)},
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::NONE,
        "an unrecognized layout state fails closed");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::RGB),
            0,
            query_t{std::nullopt}) ==
            order::RGB,
        "a per-monitor RGB layout needs no orientation fallback");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::BGR),
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::BGR,
        "a per-monitor layout wins over a disagreeing global orientation");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::NONE),
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::NONE,
        "an explicit per-monitor Flat answer is never substituted");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            invalid_layout,
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::NONE,
        "a malformed per-monitor value fails closed without fallback");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            invalid_layout,
            90,
            query_t{k_documented_windows_orientation_bgr}) ==
            order::NONE,
        "a malformed per-monitor value stays NONE under rotation");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            absent_layout,
            0,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::RGB,
        "a missing per-monitor layout falls back to the global RGB orientation");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            absent_layout,
            0,
            query_t{k_documented_windows_orientation_bgr}) ==
            order::BGR,
        "a missing per-monitor layout falls back to the global BGR orientation");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            absent_layout,
            0,
            query_t{std::nullopt}) ==
            order::NONE,
        "missing monitor and global answers fail closed");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::RGB),
            90,
            query_t{std::nullopt}) ==
            order::VRGB,
        "a 90-degree screen rotation maps RGB to VRGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::RGB),
            270,
            query_t{std::nullopt}) ==
            order::VBGR,
        "a 270-degree screen rotation maps RGB to VBGR");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::BGR),
            180,
            query_t{std::nullopt}) ==
            order::RGB,
        "a 180-degree screen rotation reverses BGR to RGB");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            absent_layout,
            90,
            query_t{k_documented_windows_orientation_rgb}) ==
            order::VRGB,
        "the rotation table also applies to the fallback orientation");
    ok &= check(
        term_internal::resolved_lcd_subpixel_order_from_windows_sources(
            resolved_layout(order::RGB),
            -1,
            query_t{std::nullopt}) ==
            order::NONE,
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
    ok &= test_windows_orientation_query_mapping();
    ok &= test_windows_sources_precedence_and_rotation();

    if (!ok) {
        std::cerr << "lcd_subpixel_policy_tests: FAILED\n";
        return 1;
    }

    std::cout << "lcd_subpixel_policy_tests: OK\n";
    return 0;
}
