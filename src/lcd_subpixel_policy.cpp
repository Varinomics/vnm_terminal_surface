#include "vnm_terminal/lcd_subpixel_policy.h"

#include "vnm_terminal/internal/lcd_subpixel_policy_platform.h"

#include <QScreen>
#include <qpa/qplatformscreen.h>

#include <algorithm>
#include <optional>

#if defined(_WIN32)
#include <QSettings>
#include <QtGui/qscreen_platform.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr unsigned int k_windows_font_smoothing_orientation_bgr = 0x0000U;
constexpr unsigned int k_windows_font_smoothing_orientation_rgb = 0x0001U;

#if defined(_WIN32)
constexpr unsigned int k_win_spi_get_font_smoothing_orientation = 0x2012U;

vnm_terminal::internal::windows_screen_layout_t
read_windows_screen_subpixel_order(const QScreen& screen)
{
    using vnm_terminal::Resolved_lcd_subpixel_order;
    using vnm_terminal::internal::Windows_screen_layout_state;
    using vnm_terminal::internal::windows_screen_layout_t;

    const auto* const native_screen =
        screen.nativeInterface<QNativeInterface::QWindowsScreen>();
    // Without an identified monitor there is no per-monitor answer at all;
    // the resolver fails closed instead of borrowing the global orientation.
    if (native_screen == nullptr || native_screen->handle() == nullptr) {
        return {};
    }

    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(native_screen->handle(), &monitor_info) == FALSE) {
        return {};
    }

    const QString device_name = QString::fromWCharArray(monitor_info.szDevice);
    const QString device_prefix = QStringLiteral("\\\\.\\DISPLAY");
    if (!device_name.startsWith(device_prefix)) {
        return {};
    }
    const QString display_number = device_name.mid(device_prefix.size());
    if (display_number.isEmpty() ||
        !std::all_of(
            display_number.cbegin(),
            display_number.cend(),
            [](QChar character) { return character.isDigit(); }))
    {
        return {};
    }

    QSettings registry(
        QStringLiteral(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Avalon.Graphics\\DISPLAY%1")
            .arg(display_number),
        QSettings::NativeFormat);
    const QVariant raw_pixel_structure =
        registry.value(QStringLiteral("PixelStructure"));
    if (!raw_pixel_structure.isValid()) {
        // The monitor is identified but stores no override; only here may the
        // resolver fall back to the global orientation.
        return {
            Windows_screen_layout_state::ABSENT,
            Resolved_lcd_subpixel_order::NONE};
    }
    bool pixel_structure_valid = false;
    const int pixel_structure = raw_pixel_structure.toInt(&pixel_structure_valid);
    if (pixel_structure_valid) {
        switch (pixel_structure) {
            case 0:
                return {
                    Windows_screen_layout_state::RESOLVED,
                    Resolved_lcd_subpixel_order::NONE};
            case 1:
                return {
                    Windows_screen_layout_state::RESOLVED,
                    Resolved_lcd_subpixel_order::RGB};
            case 2:
                return {
                    Windows_screen_layout_state::RESOLVED,
                    Resolved_lcd_subpixel_order::BGR};
            default:
                break;
        }
    }
    return {Windows_screen_layout_state::INVALID, Resolved_lcd_subpixel_order::NONE};
}

std::optional<int> windows_screen_rotation_degrees(const QScreen& screen)
{
    Qt::ScreenOrientation orientation = screen.orientation();
    if (orientation == Qt::PrimaryOrientation) {
        orientation = screen.primaryOrientation();
    }
    switch (orientation) {
        case Qt::LandscapeOrientation:
            return 0;
        case Qt::PortraitOrientation:
            return 90;
        case Qt::InvertedLandscapeOrientation:
            return 180;
        case Qt::InvertedPortraitOrientation:
            return 270;
        default:
            return std::nullopt;
    }
}
#endif

vnm_terminal::internal::windows_lcd_orientation_query_t
read_windows_lcd_orientation()
{
    vnm_terminal::internal::windows_lcd_orientation_query_t query;
#if defined(_WIN32)
    unsigned int orientation = 0U;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_orientation,
            0U,
            &orientation,
            0U) != 0)
    {
        query.orientation = orientation;
    }
#endif
    return query;
}

}

namespace vnm_terminal::internal {

std::optional<Resolved_lcd_subpixel_order>
resolved_lcd_subpixel_order_from_qt_hint(int hint)
{
    switch (hint) {
        case QPlatformScreen::Subpixel_None:
            return Resolved_lcd_subpixel_order::NONE;
        case QPlatformScreen::Subpixel_RGB:
            return Resolved_lcd_subpixel_order::RGB;
        case QPlatformScreen::Subpixel_BGR:
            return Resolved_lcd_subpixel_order::BGR;
        case QPlatformScreen::Subpixel_VRGB:
            return Resolved_lcd_subpixel_order::VRGB;
        case QPlatformScreen::Subpixel_VBGR:
            return Resolved_lcd_subpixel_order::VBGR;
    }

    return std::nullopt;
}

std::optional<Resolved_lcd_subpixel_order>
resolved_lcd_subpixel_order_from_windows_orientation(
    const windows_lcd_orientation_query_t& query)
{
    if (!query.orientation.has_value()) {
        return std::nullopt;
    }
    switch (*query.orientation) {
        case k_windows_font_smoothing_orientation_rgb: return Resolved_lcd_subpixel_order::RGB;
        case k_windows_font_smoothing_orientation_bgr: return Resolved_lcd_subpixel_order::BGR;
        default:                                       return std::nullopt;
    }
}

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_sources(
    const windows_screen_layout_t&         screen_layout,
    int                                    rotation_degrees,
    const windows_lcd_orientation_query_t& orientation_query)
{
    // UNAVAILABLE and INVALID both mean the monitor's layout is genuinely
    // unknown; borrowing the global orientation would risk wrong-direction
    // fringing. Any state added later fails closed here as well.
    std::optional<Resolved_lcd_subpixel_order> layout;
    switch (screen_layout.state) {
        case Windows_screen_layout_state::RESOLVED:
            layout = screen_layout.order;
            break;
        case Windows_screen_layout_state::ABSENT:
            // The global ClearType orientation is a best-effort fallback,
            // expressed in final-screen terms for the primary display.
            layout = resolved_lcd_subpixel_order_from_windows_orientation(
                orientation_query);
            break;
        case Windows_screen_layout_state::UNAVAILABLE:
        case Windows_screen_layout_state::INVALID:
        default:
            return Resolved_lcd_subpixel_order::NONE;
    }

    // A RESOLVED Flat answer is a real layout, and the orientation fallback
    // never yields one: anything other than horizontal RGB/BGR here is an
    // explicit or unknown NONE.
    if (!layout.has_value() ||
        (*layout != Resolved_lcd_subpixel_order::RGB &&
            *layout != Resolved_lcd_subpixel_order::BGR))
    {
        return Resolved_lcd_subpixel_order::NONE;
    }

    switch (rotation_degrees) {
        case 0:
            return *layout;
        case 90:
            return *layout == Resolved_lcd_subpixel_order::RGB
                ? Resolved_lcd_subpixel_order::VRGB
                : Resolved_lcd_subpixel_order::VBGR;
        case 180:
            return *layout == Resolved_lcd_subpixel_order::RGB
                ? Resolved_lcd_subpixel_order::BGR
                : Resolved_lcd_subpixel_order::RGB;
        case 270:
            return *layout == Resolved_lcd_subpixel_order::RGB
                ? Resolved_lcd_subpixel_order::VBGR
                : Resolved_lcd_subpixel_order::VRGB;
    }
    return Resolved_lcd_subpixel_order::NONE;
}

}

namespace vnm_terminal {

Resolved_lcd_subpixel_order resolve_lcd_subpixel_order(
    Lcd_subpixel_order_policy policy,
    const QScreen*            screen)
{
    switch (policy) {
        case Lcd_subpixel_order_policy::NONE:
            return Resolved_lcd_subpixel_order::NONE;
        case Lcd_subpixel_order_policy::RGB:
            return Resolved_lcd_subpixel_order::RGB;
        case Lcd_subpixel_order_policy::BGR:
            return Resolved_lcd_subpixel_order::BGR;
        case Lcd_subpixel_order_policy::VRGB:
            return Resolved_lcd_subpixel_order::VRGB;
        case Lcd_subpixel_order_policy::VBGR:
            return Resolved_lcd_subpixel_order::VBGR;
        case Lcd_subpixel_order_policy::AUTO:
            break;
        default:
            return Resolved_lcd_subpixel_order::NONE;
    }

    if (screen == nullptr) {
        return Resolved_lcd_subpixel_order::NONE;
    }

    const QPlatformScreen* const platform_screen = screen->handle();
    if (platform_screen == nullptr) {
        return Resolved_lcd_subpixel_order::NONE;
    }

#if defined(_WIN32)
    const std::optional<int> rotation_degrees =
        windows_screen_rotation_degrees(*screen);
    if (!rotation_degrees.has_value()) {
        return Resolved_lcd_subpixel_order::NONE;
    }
    return internal::resolved_lcd_subpixel_order_from_windows_sources(
        read_windows_screen_subpixel_order(*screen),
        *rotation_degrees,
        read_windows_lcd_orientation());
#else
    const int qt_hint =
        static_cast<int>(platform_screen->subpixelAntialiasingTypeHint());
    const std::optional<Resolved_lcd_subpixel_order> qt_order =
        internal::resolved_lcd_subpixel_order_from_qt_hint(qt_hint);
    if (!qt_order.has_value()) {
        return Resolved_lcd_subpixel_order::NONE;
    }
    return *qt_order;
#endif
}

}
