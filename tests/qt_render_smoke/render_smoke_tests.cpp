#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>
#include <rhi/qrhi.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

namespace {

namespace term = vnm_terminal::internal;

int fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

constexpr int k_unsupported_backend_skip_return_code = 77;

int skip(const char* message)
{
    std::cerr << "SKIP: " << message << '\n';
    return k_unsupported_backend_skip_return_code;
}

// The accelerated-atlas pixel assertions below compare against a hardware GPU
// reference. Software and headless RHI backends (the llvmpipe/WARP devices on
// CI runners, or an offscreen platform with no usable GPU context) do not
// produce byte-identical rasterization, so the smoke test classifies the active
// backend and skips the pixel-content checks there. A null renderer interface
// or RHI means no usable accelerated context, which is treated the same way.
bool renderer_is_software_or_headless(QQuickWindow& window)
{
    QSGRendererInterface* const renderer_interface = window.rendererInterface();
    if (renderer_interface == nullptr) {
        return true;
    }

    QRhi* const rhi = static_cast<QRhi*>(
        renderer_interface->getResource(
            &window,
            QSGRendererInterface::RhiResource));
    if (rhi == nullptr) {
        return true;
    }

    return term::qsg_atlas_driver_info_is_known_software_renderer(rhi->driverInfo());
}

// Pump the event loop until the scene graph brings up its RHI so the backend
// can be classified, or give up after a bounded number of iterations (which
// itself indicates a headless/unusable context).
bool wait_for_renderer_interface(QGuiApplication& app, QQuickWindow& window)
{
    for (int i = 0; i < 20; ++i) {
        QSGRendererInterface* const renderer_interface =
            window.rendererInterface();
        if (renderer_interface != nullptr &&
            renderer_interface->getResource(
                &window,
                QSGRendererInterface::RhiResource) != nullptr)
        {
            return true;
        }

        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    return false;
}

int count_dark_background_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() >= 6 && color.red() <= 14 &&
                color.green() >= 9 && color.green() <= 16 &&
                color.blue() >= 13 && color.blue() <= 20)
            {
                ++count;
            }
        }
    }

    return count;
}

int count_light_background_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() >= 240 && color.red() <= 252 &&
                color.green() >= 240 && color.green() <= 252 &&
                color.blue() >= 235 && color.blue() <= 248)
            {
                ++count;
            }
        }
    }

    return count;
}

int count_dark_glyph_ink_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 150 && color.green() > 150 && color.blue() > 150) {
                ++count;
            }
        }
    }

    return count;
}

int count_light_glyph_ink_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 90 && color.green() < 90 && color.blue() < 90) {
                ++count;
            }
        }
    }

    return count;
}

term::Terminal_color_state smoke_color_state(
    quint32 foreground_rgba,
    quint32 background_rgba)
{
    term::Terminal_color_state state;
    state.default_foreground_rgba = foreground_rgba;
    state.default_background_rgba = background_rgba;
    state.cursor_rgba             = 0xffffffffU;
    return state;
}

std::shared_ptr<const term::Terminal_render_snapshot> make_smoke_snapshot(
    std::uint64_t sequence,
    quint32       foreground_rgba,
    quint32       background_rgba)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 12}, viewport, sequence);
    snapshot.color_state      = smoke_color_state(foreground_rgba, background_rgba);
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 2}};
    const QString marker_text = QStringLiteral("atlas");
    const int marker_width    = static_cast<int>(marker_text.size());
    snapshot.cells.push_back({
        {0, 0},
        term::Terminal_render_cell_text::from_source_cell(marker_text, marker_width, false),
        0U,
        marker_width,
        false,
        term::k_default_terminal_style_id,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
    });

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

bool pump_until_rendered(
    QGuiApplication&   app,
    QQuickWindow&      window,
    QImage&            out,
    bool               light_theme)
{
    for (int i = 0; i < 20; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out = window.grabWindow();

        if (out.isNull()) {
            continue;
        }

        const QRect probe(250, 120, 40, 24);
        const int background_pixels = light_theme
            ? count_light_background_pixels(out, probe)
            : count_dark_background_pixels(out, probe);
        if (background_pixels > 600) {
            return true;
        }
    }

    return false;
}

}

int main(int argc, char** argv)
{
#if defined(Q_OS_WIN)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
#endif

    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(320, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(320.0, 160.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);

    if (!surface.flags().testFlag(QQuickItem::ItemHasContents)) {
        return fail("terminal surface does not declare scene graph content");
    }

    if (!surface.clip()) {
        return fail("terminal surface must clip scene graph content to its item bounds");
    }

    if (!surface.childItems().empty()) {
        return fail("terminal surface must not allocate child QQuickItems for smoke text");
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_smoke_snapshot(1U, 0xffffffffU, 0xff090c10U));

    window.show();

    if (!wait_for_renderer_interface(app, window) ||
        renderer_is_software_or_headless(window))
    {
        return skip(
            "accelerated atlas render smoke requires a hardware GPU; the active "
            "RHI backend is software or headless and does not reproduce the "
            "accelerated rasterization the pixel checks compare against");
    }

    QImage rendered;
    if (!pump_until_rendered(app, window, rendered, false)) {
        return fail("accelerated atlas render smoke did not produce offscreen pixels");
    }

    if (count_dark_background_pixels(rendered, QRect(250, 120, 40, 24)) < 600) {
        return fail("terminal surface did not preserve dark background outside the smoke text");
    }

    if (count_dark_glyph_ink_pixels(rendered, QRect(0, 0, 160, 80)) < 20) {
        return fail("terminal surface did not render dark-theme smoke glyph ink");
    }

    if (!surface.childItems().empty()) {
        return fail("terminal surface created child QQuickItems during smoke rendering");
    }

    if (rendered.size().width() < 160 || rendered.size().height() < 80) {
        return fail("grabbed render target is unexpectedly small");
    }

    surface.set_color_theme(QStringLiteral("light"));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_smoke_snapshot(2U, 0xff1f2c26U, 0xfff6f7f2U));

    QImage light_rendered;
    if (!pump_until_rendered(app, window, light_rendered, true)) {
        return fail("accelerated atlas render smoke did not produce light-theme offscreen pixels");
    }

    if (count_dark_background_pixels(light_rendered, QRect(250, 120, 40, 24)) > 20) {
        return fail("light-theme render did not repaint the background");
    }

    if (count_light_glyph_ink_pixels(light_rendered, QRect(0, 0, 160, 80)) < 20) {
        return fail("terminal surface did not render light-theme smoke glyph ink");
    }

    return 0;
}
