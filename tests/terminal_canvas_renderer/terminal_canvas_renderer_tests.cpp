#include "helpers/test_check.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_canvas_render_bridge.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QColor>
#include <QEventLoop>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPoint>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QThread>
#include <QTest>
#include <QTouchEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using vnm_terminal::test_helpers::check;

class Touch_route_observer final : public QObject
{
public:
    struct observation_t
    {
        bool canvas = false;
        QEvent::Type type = QEvent::None;
        int point_id = -1;
        QEventPoint::State state = QEventPoint::State::Unknown;
        QPointF position;
        QPointF scene_position;
    };

    explicit Touch_route_observer(QObject& window, QObject& canvas)
    :
        m_canvas(&canvas)
    {
        window.installEventFilter(this);
        canvas.installEventFilter(this);
    }
    std::vector<observation_t> observations;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate ||
            event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel)
        {
            const auto* touch = static_cast<QTouchEvent*>(event);
            if (touch->points().isEmpty()) {
                observations.push_back({watched == m_canvas, event->type()});
            }
            for (const auto& point : touch->points()) {
                observations.push_back({watched == m_canvas, event->type(), point.id(), point.state(),
                    point.position(), point.scenePosition()});
            }
        }
        return false;
    }

private:
    QObject* m_canvas;
};

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
        const qreal dpr = window.devicePixelRatio();
        if (rendered.width() == qRound(window.width() * dpr) &&
            rendered.height() == qRound(window.height() * dpr) &&
            canvas.rendered_frame_generation() == canvas.frame_generation() &&
            image_has_canvas_pixels(rendered))
        {
            return true;
        }
    }
    return false;
}

bool recorder_distinguishes_failed_candidate(
    QGuiApplication& application,
    QQuickWindow& window,
    VNM_TerminalCanvas& canvas)
{
    namespace renderer = vnm_terminal::internal;
    const auto recorder = renderer::VNM_TerminalCanvas_render_bridge::qsg_atlas_recorder(canvas);
    const auto observe = [&](std::uint64_t sequence, bool committed) {
        const auto previous_render_count = recorder->snapshot().render_count;
        for (int attempt = 0; attempt < 30; ++attempt) {
            canvas.update();
            window.requestUpdate();
            application.processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(20);
            (void)window.grabWindow();
            const auto report = recorder->snapshot();
            if (report.prepared_snapshot_sequence == sequence &&
                report.prepared_generation_committed == committed &&
                report.render_count > previous_render_count && (!committed || report.drew))
            {
                return report;
            }
        }
        return recorder->snapshot();
    };
    bool ok = check(canvas.set_canvas_frame(make_frame(701U)), "recorder baseline frame is accepted");
    const auto baseline = observe(701U, true);
    ok &= check(baseline.render_snapshot_sequence == 701U && baseline.drew &&
        baseline.render_canvas_frame_generation == canvas.frame_generation(),
        "recorder identifies the committed canvas draw");
    if (!ok) {
        return false;
    }
    renderer::qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(702U);
    const auto clear_failure = qScopeGuard([] {
        renderer::qsg_atlas_clear_resource_prepare_failure_for_testing();
    });
    ok &= check(canvas.set_canvas_frame(make_frame(702U)), "failed candidate frame is accepted by the model");
    const auto failed = observe(702U, false);
    std::fprintf(stderr,
        "recorder retention: baseline count=%llu drew=%d snapshot=%llu canvas=%llu owner=%llu pub=%llu; "
        "failed count=%llu drew=%d snapshot=%llu canvas=%llu owner=%llu pub=%llu "
        "prepared=%llu committed=%d installed=%llu\n",
        static_cast<unsigned long long>(baseline.render_count), int(baseline.drew),
        static_cast<unsigned long long>(baseline.render_snapshot_sequence),
        static_cast<unsigned long long>(baseline.render_canvas_frame_generation),
        static_cast<unsigned long long>(baseline.render_ownership_generation),
        static_cast<unsigned long long>(baseline.render_publication_generation),
        static_cast<unsigned long long>(failed.render_count), int(failed.drew),
        static_cast<unsigned long long>(failed.render_snapshot_sequence),
        static_cast<unsigned long long>(failed.render_canvas_frame_generation),
        static_cast<unsigned long long>(failed.render_ownership_generation),
        static_cast<unsigned long long>(failed.render_publication_generation),
        static_cast<unsigned long long>(failed.prepared_snapshot_sequence),
        int(failed.prepared_generation_committed),
        static_cast<unsigned long long>(canvas.frame_generation()));
    ok &= check(failed.prepared_snapshot_sequence == 702U && !failed.prepared_generation_committed,
        "recorder exposes the failed prepare separately from its retained draw");
    const bool retained_draw =
        failed.render_snapshot_sequence == baseline.render_snapshot_sequence &&
        failed.render_canvas_frame_generation == baseline.render_canvas_frame_generation &&
        failed.render_ownership_generation == baseline.render_ownership_generation &&
        failed.render_publication_generation == baseline.render_publication_generation &&
        failed.render_canvas_frame_generation != canvas.frame_generation();
    // A failed prepare can retain the earlier draw or issue no draw at all.
    // Neither outcome may be credited as presentation of the candidate.
    ok &= check(failed.render_count > baseline.render_count && (!failed.drew || retained_draw),
        "a failed candidate cannot be attributed as the rendered canvas identity");
    renderer::qsg_atlas_clear_resource_prepare_failure_for_testing();
    ok &= check(canvas.set_canvas_frame(make_frame(703U)), "recovered frame is accepted");
    const auto recovered = observe(703U, true);
    ok &= check(recovered.drew && recovered.prepared_generation_committed &&
        recovered.render_snapshot_sequence == 703U &&
        recovered.render_snapshot_sequence == recovered.prepared_snapshot_sequence &&
        recovered.render_capture_sequence == recovered.prepared_capture_sequence &&
        recovered.render_canvas_frame_generation == recovered.prepared_canvas_frame_generation &&
        recovered.render_ownership_generation == recovered.prepared_ownership_generation &&
        recovered.render_publication_generation == recovered.prepared_publication_generation &&
        recovered.render_canvas_frame_generation == canvas.frame_generation(),
        "recorder advances to the recovered committed frame");
    return ok;
}

bool rendered_composition_handle_contract(
    QGuiApplication& application, QQuickWindow& window, VNM_TerminalCanvas& canvas)
{
    namespace touch = vnm_terminal;
    namespace renderer = vnm_terminal::internal;
    auto frame = make_frame(801U);
    frame->rows = 6;
    frame->columns = 20;
    frame->content_width = 200;
    frame->content_height = 120;
    touch::Terminal_canvas_selection selection;
    selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 6, 20};
    selection.selection_generation = 1U;
    selection.has_selection = true;
    selection.touch_handles_visible = true;
    selection.start = {2, 3, true};
    selection.end = {2, 10, true};
    selection.spans = {{2, 3, 7}};
    selection.composition = touch::terminal_touch_selection_composition_t{1, 1};
    for (int row = 0; row < 6; ++row) {
        selection.visible_rows.push_back({100U + (std::uint64_t)row, 1U, 0});
    }
    frame->selection = selection;
    window.resize(200, 150);
    canvas.setSize(QSizeF(200, 150));
    canvas.set_authoritative_cell_metrics_enabled(true);
    canvas.set_touch_selection_enabled(true);
    // A partial first row remains touchable. Its visible prefix must use the
    // same floor boundary as the controller's retained-row hit testing.
    canvas.set_touch_viewport(QRectF(0, 5, 200, 90));
    std::vector<touch::Terminal_touch_selection_request> requests;
    Touch_route_observer touch_route(window, canvas);
    const auto connection = QObject::connect(&canvas, &VNM_TerminalCanvas::touch_selection_requested,
        &canvas, [&](const auto& request) { requests.push_back(request); });
    const auto cleanup = qScopeGuard([&] {
        canvas.set_touch_selection_enabled(false);
        QObject::disconnect(connection);
        renderer::qsg_atlas_clear_resource_prepare_failure_for_testing();
    });
    QImage rendered;
    if (!check(canvas.set_canvas_frame(frame) && pump_until_rendered(application, window, canvas, rendered),
            "the initial composition reaches a real committed canvas draw"))
    {
        return false;
    }
    const auto recorder = renderer::VNM_TerminalCanvas_render_bridge::qsg_atlas_recorder(canvas);
    std::uint64_t completed_request_id = 0U;
    const auto complete = [&] {
        const auto request = requests.back();
        completed_request_id = request.request_id;
        canvas.complete_touch_selection_request({request.request_id, request.action,
            touch::Terminal_touch_selection_status::OK, 1U, {}, {}});
    };
    const auto expect_scroll = [&](int padding, int native_rows, std::uint64_t row_id, const char* message) {
        const auto& request = requests.back();
        return check(request.action == touch::Terminal_touch_selection_action::SCROLL_EXTEND &&
            request.target.row.retained_line_id == row_id && request.presentation &&
            request.presentation->top_padding_rows == padding &&
            request.presentation->visible_native_rows == native_rows, message);
    };
    const auto next_scroll = [&] {
        const auto before = requests.size();
        complete();
        return check(QTest::qWaitFor([&] { return requests.size() > before; }, 1000),
            "the held handle requests its next edge scroll after acknowledgement");
    };
    const auto install_held = [&](const std::shared_ptr<touch::Terminal_canvas_frame>& candidate) {
        const auto committed = recorder->snapshot();
        renderer::qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(candidate->sequence);
        if (!canvas.set_canvas_frame(candidate)) {
            return false;
        }
        for (int attempt = 0; attempt < 30; ++attempt) {
            canvas.update();
            window.requestUpdate();
            application.processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(20);
            (void)window.grabWindow();
            const auto report = recorder->snapshot();
            if (report.prepared_snapshot_sequence == candidate->sequence &&
                !report.prepared_generation_committed && report.rendered_touch_selection &&
                report.rendered_touch_selection == committed.rendered_touch_selection &&
                report.rendered_touch_cell_metrics.width == committed.rendered_touch_cell_metrics.width &&
                report.rendered_touch_cell_metrics.height == committed.rendered_touch_cell_metrics.height &&
                report.rendered_touch_cell_metrics.ascent == committed.rendered_touch_cell_metrics.ascent &&
                report.rendered_touch_cell_metrics.descent == committed.rendered_touch_cell_metrics.descent)
            {
                return true;
            }
        }
        const auto report = recorder->snapshot();
        std::fprintf(stderr,
            "held composition: candidate=%llu prepared=%llu committed=%d drew=%d retained_selection=%d "
            "retained_cell_height=%.3f previous_cell_height=%.3f\n",
            static_cast<unsigned long long>(candidate->sequence),
            static_cast<unsigned long long>(report.prepared_snapshot_sequence),
            int(report.prepared_generation_committed), int(report.drew),
            int(report.rendered_touch_selection == committed.rendered_touch_selection),
            double(report.rendered_touch_cell_metrics.height), double(committed.rendered_touch_cell_metrics.height));
        return false;
    };
    auto* device = QTest::createTouchDevice();
    QTest::touchEvent(&window, device).press(0, QPoint(100, 67), &window);
    if (!check(!requests.empty() && requests.back().action == touch::Terminal_touch_selection_action::BEGIN_HANDLE,
            "a touch on the drawn handle enters the real canvas selection request path"))
    {
        return false;
    }
    complete();
    auto increased = std::make_shared<touch::Terminal_canvas_frame>(*frame);
    increased->sequence = 802U;
    increased->cell_height = 25;
    increased->content_height = 150;
    increased->selection->composition = touch::terminal_touch_selection_composition_t{4, 3};
    for (auto& row : increased->selection->visible_rows) { row.retained_line_id += 100U; }
    if (!check(install_held(increased), "an installed capacity increase remains behind the last committed draw")) {
        return false;
    }
    QTest::touchEvent(&window, device).move(0, QPoint(100, 8), &window);
    if (!check(QTest::qWaitFor([&] {
                return requests.back().action == touch::Terminal_touch_selection_action::SCROLL_EXTEND;
            }, 1000), "dragging the actual handle reaches the clipped top edge"))
    {
        const auto report = recorder->snapshot();
        const auto viewport = canvas.touch_viewport();
        std::fprintf(stderr,
            "handle edge: requests=%zu completed=%llu keep_grab=%d active=%d visible=%d exposed=%d "
            "viewport=%.3f,%.3f,%.3f,%.3f item_y=%.3f prepared=%llu committed=%d drew=%d "
            "prepare_count=%llu render_count=%llu prepared_owner=%llu render_owner=%llu "
            "installed=%llu retained_touch=%d metrics=%.3f,%.3f\n",
            requests.size(), static_cast<unsigned long long>(completed_request_id),
            int(canvas.keepTouchGrab()), int(canvas.touch_selection_active()),
            int(window.isVisible()), int(window.isExposed()),
            double(viewport.x()), double(viewport.y()), double(viewport.width()), double(viewport.height()),
            double(canvas.y()), static_cast<unsigned long long>(report.prepared_snapshot_sequence),
            int(report.prepared_generation_committed), int(report.drew),
            static_cast<unsigned long long>(report.prepare_count), static_cast<unsigned long long>(report.render_count),
            static_cast<unsigned long long>(report.prepared_ownership_generation),
            static_cast<unsigned long long>(report.render_ownership_generation),
            static_cast<unsigned long long>(canvas.frame_generation()), int(report.rendered_touch_selection.has_value()),
            double(report.rendered_touch_cell_metrics.width), double(report.rendered_touch_cell_metrics.height));
        for (const auto& event : touch_route.observations) {
            std::fprintf(stderr, "touch route: receiver=%s type=%d point=%d state=%d local=%.3f,%.3f scene=%.3f,%.3f\n",
                event.canvas ? "canvas" : "window", int(event.type), event.point_id, int(event.state),
                double(event.position.x()), double(event.position.y()),
                double(event.scene_position.x()), double(event.scene_position.y()));
        }
        for (const auto& request : requests) {
            std::fprintf(stderr, "touch request: id=%llu action=%d row=%llu scroll=%d\n",
                static_cast<unsigned long long>(request.request_id), int(request.action),
                static_cast<unsigned long long>(request.target.row.retained_line_id), request.scroll_lines);
        }
        return false;
    }
    bool ok = expect_scroll(1, 4, 100U,
        "pending increase uses the last drawn row identity, metrics and prefix context");
    renderer::qsg_atlas_clear_resource_prepare_failure_for_testing();
    ++increased->sequence;
    if (!check(canvas.set_canvas_frame(increased) && pump_until_rendered(application, window, canvas, rendered),
            "the increased composition reaches a real committed successor draw"))
    {
        return false;
    }
    ok &= next_scroll();
    ok &= expect_scroll(3, 1, 200U,
        "committed increase adopts context and metrics together while retaining the partial first row");
    auto reduced = std::make_shared<touch::Terminal_canvas_frame>(*frame);
    reduced->sequence = 804U;
    for (auto& row : reduced->selection->visible_rows) { row.retained_line_id += 200U; }
    if (!check(install_held(reduced), "an installed capacity decrease remains behind the last committed draw")) {
        return false;
    }
    ok &= next_scroll();
    ok &= expect_scroll(3, 1, 200U,
        "pending decrease also uses the last drawn row identity, metrics and prefix context");
    renderer::qsg_atlas_clear_resource_prepare_failure_for_testing();
    ++reduced->sequence;
    if (!check(canvas.set_canvas_frame(reduced) && pump_until_rendered(application, window, canvas, rendered),
            "the reduced composition reaches a real committed successor draw"))
    {
        return false;
    }
    ok &= next_scroll();
    ok &= expect_scroll(1, 4, 300U,
        "committed decrease adopts the reduced context and row identity together");
    auto metadata_only = std::make_shared<touch::Terminal_canvas_frame>(*reduced);
    metadata_only->selection->composition->leading_capacity = 2;
    ok &= check(canvas.set_canvas_frame(metadata_only) &&
            pump_until_rendered(application, window, canvas, rendered) &&
            recorder->snapshot().rendered_touch_selection->composition == metadata_only->selection->composition,
        "a composition-only replacement reaches the renderer even with unchanged source sequence and cells");
    canvas.set_touch_selection_enabled(false);
    QTest::touchEvent(&window, device).release(0, QPoint(100, 8), &window);
    return ok;
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

bool rendered_pixel_matches(
    QGuiApplication&  application,
    QQuickWindow&     window,
    VNM_TerminalCanvas& canvas,
    QPoint            logical_position,
    QColor            expected)
{
    const qreal dpr = window.devicePixelRatio();
    const QPoint position(
        qRound(logical_position.x() * dpr),
        qRound(logical_position.y() * dpr));
    for (int attempt = 0; attempt < 30; ++attempt) {
        canvas.update();
        window.requestUpdate();
        application.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const QImage image = window.grabWindow();
        if (!image.rect().contains(position) ||
            canvas.rendered_frame_generation() != canvas.frame_generation())
        {
            continue;
        }
        const QColor actual = image.pixelColor(position);
        if (std::abs(actual.red()   - expected.red())   <= 1 &&
            std::abs(actual.green() - expected.green()) <= 1 &&
            std::abs(actual.blue()  - expected.blue())  <= 1)
        {
            return true;
        }
    }
    return false;
}

bool row_images_follow_pixels_placement_and_lifecycle(
    QGuiApplication&  application,
    QQuickWindow&     window,
    VNM_TerminalCanvas& canvas)
{
    bool ok = true;
    auto frame = make_frame(30U);
    frame->cursor.visible = false;
    QImage pixels(30, 20, QImage::Format_RGBA8888_Premultiplied);
    const QColor red(200, 40, 40);
    const QColor green(20, 160, 80);
    const QColor background = QColor::fromRgba(frame->default_background_rgba);
    pixels.fill(red);
    frame->images.emplace();
    frame->images->rows.push_back({1, 2, 10, 20, 3U, pixels});
    canvas.set_authoritative_cell_metrics_enabled(true);
    window.resize(120, 40);
    canvas.setSize(QSizeF(120, 40));
    ok &= check(canvas.set_canvas_frame(frame), "public row image frame is accepted");
    ok &= check(rendered_pixel_matches(application, window, canvas, {35, 30}, red),
        "row image pixels reach their source cell location through the canvas renderer");
    ok &= check(rendered_pixel_matches(application, window, canvas, {65, 30}, background),
        "image pixels occupy only their source width");

    auto replacement = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*frame);
    ++replacement->sequence;
    replacement->images->rows.front().pixels.fill(green);
    ok &= check(canvas.set_canvas_frame(replacement), "replacement image frame is accepted");
    ok &= check(rendered_pixel_matches(application, window, canvas, {35, 30}, green),
        "a new source with a repeated sender revision cannot reuse stale image texels");

    auto moved = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*replacement);
    ++moved->sequence;
    moved->images->rows.front().row = 0;
    moved->images->rows.front().first_column = 4;
    ok &= check(canvas.set_canvas_frame(moved), "moved row image frame is accepted");
    ok &= check(rendered_pixel_matches(application, window, canvas, {55, 10}, green) &&
            rendered_pixel_matches(application, window, canvas, {35, 30}, background),
        "image row and column movement retires the original placement");

    auto scaled = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*moved);
    ++scaled->sequence;
    scaled->cell_width = 20.0;
    scaled->cell_height = 40.0;
    scaled->content_width = 240.0;
    scaled->content_height = 80.0;
    window.resize(240, 80);
    canvas.setSize(QSizeF(240, 80));
    ok &= check(canvas.set_canvas_frame(scaled), "scaled row image frame is accepted");
    ok &= check(rendered_pixel_matches(application, window, canvas, {110, 20}, green),
        "image pixels scale from placement cell size to authoritative presentation cells");

    auto cleared = std::make_shared<vnm_terminal::Terminal_canvas_frame>(*scaled);
    ++cleared->sequence;
    cleared->images->rows.clear();
    ok &= check(canvas.set_canvas_frame(cleared), "image-clear publication is accepted");
    ok &= check(rendered_pixel_matches(application, window, canvas, {110, 20}, background),
        "image-free replacement clears prior pixels while keeping terminal text");
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
        ok &= recorder_distinguishes_failed_candidate(application, window, canvas);
        ok &= authoritative_grid_glyph_positions(application, window, canvas);
        ok &= row_images_follow_pixels_placement_and_lifecycle(application, window, canvas);
        ok &= rendered_composition_handle_contract(application, window, canvas);
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
