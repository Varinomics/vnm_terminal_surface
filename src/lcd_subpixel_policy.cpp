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

constexpr unsigned int k_windows_font_smoothing_cleartype       = 0x0002U;
constexpr unsigned int k_windows_font_smoothing_orientation_bgr = 0x0000U;
constexpr unsigned int k_windows_font_smoothing_orientation_rgb = 0x0001U;

#if defined(_WIN32)
constexpr unsigned int k_win_spi_get_font_smoothing             = 0x004AU;
constexpr unsigned int k_win_spi_get_font_smoothing_type        = 0x200AU;
constexpr unsigned int k_win_spi_get_font_smoothing_orientation = 0x2012U;

std::optional<vnm_terminal::Resolved_lcd_subpixel_order>
read_windows_screen_subpixel_order(const QScreen& screen)
{
    const auto* const native_screen =
        screen.nativeInterface<QNativeInterface::QWindowsScreen>();
    if (native_screen == nullptr || native_screen->handle() == nullptr) {
        return std::nullopt;
    }

    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(native_screen->handle(), &monitor_info) == FALSE) {
        return std::nullopt;
    }

    const QString device_name = QString::fromWCharArray(monitor_info.szDevice);
    const QString device_prefix = QStringLiteral("\\\\.\\DISPLAY");
    if (!device_name.startsWith(device_prefix)) {
        return std::nullopt;
    }
    const QString display_number = device_name.mid(device_prefix.size());
    if (display_number.isEmpty() ||
        !std::all_of(
            display_number.cbegin(),
            display_number.cend(),
            [](QChar character) { return character.isDigit(); }))
    {
        return std::nullopt;
    }

    QSettings registry(
        QStringLiteral(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Avalon.Graphics\\DISPLAY%1")
            .arg(display_number),
        QSettings::NativeFormat);
    bool pixel_structure_valid = false;
    const int pixel_structure = registry.value(
        QStringLiteral("PixelStructure"),
        -1).toInt(&pixel_structure_valid);
    if (!pixel_structure_valid) {
        return std::nullopt;
    }
    switch (pixel_structure) {
        case 0:
            return vnm_terminal::Resolved_lcd_subpixel_order::NONE;
        case 1:
            return vnm_terminal::Resolved_lcd_subpixel_order::RGB;
        case 2:
            return vnm_terminal::Resolved_lcd_subpixel_order::BGR;
    }
    return std::nullopt;
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

std::optional<vnm_terminal::internal::windows_font_smoothing_settings_t>
read_windows_font_smoothing_settings()
{
#if defined(_WIN32)
    vnm_terminal::internal::windows_font_smoothing_settings_t settings;

    int font_smoothing_enabled = 0;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing,
            0U,
            &font_smoothing_enabled,
            0U) == 0)
    {
        return std::nullopt;
    }

    settings.enabled = font_smoothing_enabled != 0;
    if (!settings.enabled) {
        return settings;
    }

    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_type,
            0U,
            &settings.type,
            0U) == 0)
    {
        return std::nullopt;
    }

    if (settings.type != k_windows_font_smoothing_cleartype) {
        return settings;
    }

    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_orientation,
            0U,
            &settings.orientation,
            0U) == 0)
    {
        return std::nullopt;
    }

    return settings;
#else
    return std::nullopt;
#endif
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

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_settings(
    const std::optional<windows_font_smoothing_settings_t>& settings)
{
    if (!settings.has_value() || !settings->enabled ||
        settings->type != k_windows_font_smoothing_cleartype)
    {
        return Resolved_lcd_subpixel_order::NONE;
    }

    switch (settings->orientation) {
        case k_windows_font_smoothing_orientation_rgb:
            return Resolved_lcd_subpixel_order::RGB;
        case k_windows_font_smoothing_orientation_bgr:
            return Resolved_lcd_subpixel_order::BGR;
    }

    return Resolved_lcd_subpixel_order::NONE;
}

Resolved_lcd_subpixel_order resolved_lcd_subpixel_order_from_windows_sources(
    const std::optional<Resolved_lcd_subpixel_order>&        screen_order,
    int                                                     rotation_degrees,
    const std::optional<windows_font_smoothing_settings_t>& settings)
{
    const Resolved_lcd_subpixel_order windows_order =
        resolved_lcd_subpixel_order_from_windows_settings(settings);
    if (windows_order == Resolved_lcd_subpixel_order::NONE) {
        return Resolved_lcd_subpixel_order::NONE;
    }

    if (!screen_order.has_value() ||
        (*screen_order != Resolved_lcd_subpixel_order::RGB &&
            *screen_order != Resolved_lcd_subpixel_order::BGR))
    {
        return Resolved_lcd_subpixel_order::NONE;
    }

    switch (rotation_degrees) {
        case 0:
            return *screen_order;
        case 90:
            return *screen_order == Resolved_lcd_subpixel_order::RGB
                ? Resolved_lcd_subpixel_order::VRGB
                : Resolved_lcd_subpixel_order::VBGR;
        case 180:
            return *screen_order == Resolved_lcd_subpixel_order::RGB
                ? Resolved_lcd_subpixel_order::BGR
                : Resolved_lcd_subpixel_order::RGB;
        case 270:
            return *screen_order == Resolved_lcd_subpixel_order::RGB
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
        read_windows_font_smoothing_settings());
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
