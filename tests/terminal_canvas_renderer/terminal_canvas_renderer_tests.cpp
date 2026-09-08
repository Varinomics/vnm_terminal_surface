#include "helpers/test_check.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QColor>
#include <QEventLoop>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace {

using vnm_terminal::test_helpers::check;

std::shared_ptr<vnm_terminal::Terminal_canvas_frame> make_frame(
    std::uint64_t sequence)
{
    auto frame = std::make_shared<vnm_terminal::Terminal_canvas_frame>();
    frame->rows                        = 2;
    frame->columns                     = 12;
    frame->cell_width                  = 10.0;
    frame->cell_height                 = 20.0;
    frame->content_width               = 120.0;
    frame->content_height              = 40.0;
    frame->sequence                    = sequence;
    frame->publication_generation      = 3U;
    frame->row_origin_generation       = 2U;
    frame->default_foreground_rgba      = 0xffffffffU;
    frame->default_background_rgba      = 0xff091018U;
    frame->cursor_rgba                  = 0xff80ff80U;
    frame->styles.push_back({
        frame->default_foreground_rgba,
        frame->default_background_rgba,
        0U,
    });
    frame->styles.push_back({
        0xffff8030U,
        frame->default_background_rgba,
        static_cast<std::uint16_t>(
            vnm_terminal::Terminal_canvas_style_attribute::UNDERLINE),
    });
    frame->cells = {
        {0, 0, 1, 1U, QStringLiteral("A")},
        {0, 1, 2, 1U, QString::fromUtf8("\xe7\x95\x8c")},
        {1, 0, 1, 0U, QStringLiteral("B")},
    };
    frame->cursor = {
        1,
        1,
        vnm_terminal::Terminal_canvas_cursor_shape::BLOCK,
        true,
        false,
    };
    return frame;
}

bool image_has_canvas_pixels(const QImage& image)
{
    if (image.isNull()) {
        return false;
    }

    int dark_pixels    = 0;
    int colored_pixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 40 && color.green() < 50 && color.blue() < 60) {
                ++dark_pixels;
            }
            if (color.red() > 120 &&
                std::max(color.green(), color.blue()) > 70)
            {
                ++colored_pixels;
            }
        }
    }
    return dark_pixels > image.width() * image.height() / 2 && colored_pixels > 2;
}

bool renderer_is_software_or_headless(QQuickWindow& window)
{
    QSGRendererInterface* const renderer_interface = window.rendererInterface();
    if (renderer_interface == nullptr) {
        return true;
    }

    const QSGRendererInterface::GraphicsApi graphics_api =
        renderer_interface->graphicsApi();
    return
        QGuiApplication::platformName() == QLatin1String("offscreen") ||
        graphics_api == QSGRendererInterface::Unknown ||
        graphics_api == QSGRendererInterface::Software ||
        graphics_api == QSGRendererInterface::Null;
}

bool pump_until_rendered(
    QGuiApplication&  application,
    QQuickWindow&     window,
    VNM_TerminalCanvas& canvas,
    QImage&           rendered)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        canvas.update();
        window.requestUpdate();
        application.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        rendered = window.grabWindow();
        if (image_has_canvas_pixels(rendered)) {
            return true;
        }
    }
    return false;
}

bool authoritative_grid_glyph_positions(
    QGuiApplication& application,
    QQuickWindow& window,
    VNM_TerminalCanvas& canvas)
{
    const qreal dpr = window.devicePixelRatio();
    QFont font(canvas.font_family());
    font.setPixelSize(qRound(canvas.font_size()));
    const qreal physical_width = std::ceil(
        QFontMetricsF(font).horizontalAdvance(QLatin1Char('H')) * dpr) + 2.0;
    bool ok = true;
    for (const qreal fraction : {0.0, 0.4, 0.6}) {
        auto frame = make_frame(12U);
        frame->rows           = 3;
        frame->columns        = 48;
        frame->cell_width     = (physical_width + fraction) / dpr;
        frame->content_width  = frame->columns * frame->cell_width;
        frame->content_height = frame->rows * frame->cell_height;
        frame->cursor.visible = false;
        frame->cells.clear();
        for (int row = 0; row < frame->rows; ++row) {
            frame->cells.push_back({row, 2 + row * 20, 1, 0U, QStringLiteral("H")});
        }
        canvas.set_authoritative_cell_metrics_enabled(true);
        ok &= check(canvas.set_canvas_frame(frame), "fractional authoritative grid is accepted");
        window.resize(qCeil(frame->content_width), qCeil(frame->content_height));
        canvas.setSize(QSizeF(window.width(), window.height()));
        QImage image;
        ok &= check(pump_until_rendered(application, window, canvas, image),
            "fractional authoritative grid renders");
        if (image.isNull()) {
            return false;
        }
        int first_ink = -1;
        for (int row = 0; row < frame->rows; ++row) {
            int left = image.width();
            const int top    = qRound(row * frame->cell_height * dpr);
            const int bottom = std::min(image.height(), qRound((row + 1) * frame->cell_height * dpr));
            for (int y = top; y < bottom; ++y) {
                for (int x = 0; x < left; ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.red() > 160 && color.green() > 160 && color.blue() > 160) {
                        left = x;
                    }
                }
            }
            ok &= check(left < image.width(), "each grid probe row contains its glyph");
            if (row == 0) {
                first_ink = left;
            }
            const int expected_offset =
                qRound((2 + row * 20) * frame->cell_width * dpr) -
                qRound(2 * frame->cell_width * dpr);
            ok &= check(std::abs(left - first_ink - expected_offset) <= 1,
                "distant glyph origins follow authoritative cell positions without accumulated rounding");
        }
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
#if defined(Q_OS_WIN)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
#endif

    QGuiApplication application(argc, argv);
    bool            ok = true;

    VNM_TerminalCanvas canvas;
    const std::shared_ptr<vnm_terminal::Terminal_canvas_frame> source = make_frame(11U);
    ok &= check(canvas.set_canvas_frame(source), "valid public frame is accepted");
    if (!ok) {
        return 1;
    }

    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(
        std::max(1, static_cast<int>(std::ceil(canvas.implicitWidth()))),
        std::max(1, static_cast<int>(std::ceil(canvas.implicitHeight()))));
    canvas.setParentItem(window.contentItem());
    canvas.setSize(QSizeF(window.width(), window.height()));
    window.show();

    QImage rendered;
    if (!pump_until_rendered(application, window, canvas, rendered)) {
        if (renderer_is_software_or_headless(window)) {
            return ok ? 77 : 1;
        }
        ok &= check(false, "host window renders public canvas pixels");
    }

    if (!rendered.isNull()) {
        ok &= authoritative_grid_glyph_positions(application, window, canvas);
    }

    ok &= check(canvas.set_canvas_frame({}), "null frame clears the canvas");
    ok &= check(canvas.rows() == 0 && canvas.columns() == 0,
        "clear releases published canvas geometry");
    canvas.update();
    window.requestUpdate();
    application.processEvents(QEventLoop::AllEvents, 50);
    (void)window.grabWindow();
    window.hide();
    application.processEvents(QEventLoop::AllEvents, 50);

    return ok ? 0 : 1;
}
