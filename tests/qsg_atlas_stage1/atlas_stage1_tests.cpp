#include "vnm_terminal/internal/qsg_atlas_renderer_stage1.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QColor>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QQuickWindow>
#include <QRawFont>
#include <QSGRendererInterface>
#include <QThread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

#ifndef VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH
#define VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH ""
#endif

#ifndef VNM_TERMINAL_SURFACE_SOURCE_PATH
#define VNM_TERMINAL_SURFACE_SOURCE_PATH ""
#endif

constexpr int k_unsupported_backend_skip_return_code = 77;

bool has_argument(int argc, char** argv, const char* expected)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], expected) == 0) {
            return true;
        }
    }

    return false;
}

QByteArray byte_array(std::initializer_list<int> values)
{
    QByteArray out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (int value : values) {
        out.append(static_cast<char>(value));
    }
    return out;
}

const char* argument_value(int argc, char** argv, const char* option, const char* fallback)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], option) == 0) {
            return argv[index + 1];
        }
    }

    return fallback;
}

void configure_graphics_api(const char* backend)
{
    if (std::strcmp(backend, "d3d11") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
    }
    else
    if (std::strcmp(backend, "d3d12") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
    }
    else
    if (std::strcmp(backend, "vulkan") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::VulkanRhi);
    }
    else
    if (std::strcmp(backend, "opengl") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGLRhi);
    }
    else {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
    }
}

term::Terminal_render_snapshot make_text_snapshot(std::uint64_t sequence, QString text)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = 1;
    viewport.follow_tail      = true;
    viewport.active_buffer    = term::Terminal_buffer_id::PRIMARY;
    viewport.offset_from_tail = 0;
    viewport.scrollback_rows  = 0;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({1, 8}, viewport, sequence);
    snapshot.dirty_row_ranges = {{0, 1}};
    if (!text.isEmpty()) {
        snapshot.cells.push_back({
            {0, 0},
            term::Terminal_render_cell_text::from_source_cell(text, 1, false),
            0U,
            1,
            false,
            term::k_default_terminal_style_id,
            term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        });
    }
    return snapshot;
}

bool read_source_file(const char* path, QByteArray& out)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "FAIL: could not open source file: " << path << '\n';
        return false;
    }

    out = file.readAll();
    return true;
}

bool source_contains_once(const QByteArray& source, const QByteArray& token)
{
    const qsizetype first = source.indexOf(token);
    return first >= 0 && source.indexOf(token, first + token.size()) < 0;
}

bool test_source_posture()
{
    QByteArray atlas_source;
    QByteArray surface_source;
    bool ok = true;
    ok &= check(read_source_file(VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH, atlas_source),
        "atlas Stage 1 source path is readable");
    ok &= check(read_source_file(VNM_TERMINAL_SURFACE_SOURCE_PATH, surface_source),
        "surface source path is readable");
    if (!ok) {
        return false;
    }

    const std::vector<QByteArray> forbidden_atlas_tokens = {
        QByteArrayLiteral("VNM_TerminalSurface"),
        QByteArrayLiteral("m_private"),
        QByteArrayLiteral("boundingRect()"),
        QByteArrayLiteral("render_options_for_surface"),
        QByteArrayLiteral("QQuickItem"),
        QByteArrayLiteral("QQuickWindow"),
        QByteArrayLiteral("requestUpdate"),
        QByteArrayLiteral("request_render_update"),
        QByteArrayLiteral("updateInputMethod"),
        QByteArrayLiteral("->update("),
        QByteArrayLiteral(".update("),
    };
    for (const QByteArray& token : forbidden_atlas_tokens) {
        if (atlas_source.contains(token)) {
            std::cerr << "FAIL: Stage 1 atlas render-node source contains forbidden token "
                << token.constData() << '\n';
            ok = false;
        }
    }

    ok &= check(atlas_source.contains(QByteArrayLiteral("void prepare() override")),
        "Stage 1 atlas source defines QSGRenderNode::prepare");
    ok &= check(atlas_source.contains(QByteArrayLiteral("void render(const RenderState* state) override")),
        "Stage 1 atlas source defines QSGRenderNode::render");
    ok &= check(atlas_source.contains(QByteArrayLiteral("commandBuffer()")) &&
            atlas_source.contains(QByteArrayLiteral("renderTarget()")),
        "Stage 1 atlas prepare/render use QSGRenderNode render-thread accessors");
    ok &= check(atlas_source.contains(QByteArrayLiteral("m_frame")),
        "Stage 1 atlas prepare/render consume the captured frame value");

    const QByteArray capture_call =
        QByteArrayLiteral("capture_qsg_atlas_stage1_frame(");
    ok &= check(source_contains_once(surface_source, capture_call),
        "Captured_atlas_frame builder is called exactly once from the surface source");

    const qsizetype update_paint_node = surface_source.indexOf(
        QByteArrayLiteral("QSGNode* VNM_TerminalSurface::updatePaintNode"));
    const qsizetype bridge_start = surface_source.indexOf(
        QByteArrayLiteral("void term::VNM_TerminalSurface_render_bridge::set_render_snapshot"));
    const qsizetype capture_pos = surface_source.indexOf(capture_call);
    ok &= check(update_paint_node >= 0 &&
            bridge_start > update_paint_node &&
            capture_pos > update_paint_node &&
            capture_pos < bridge_start,
        "Captured_atlas_frame builder call is inside updatePaintNode");
    return ok;
}

bool test_cache_key_includes_physical_size_and_face()
{
    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const QRawFont raw_font = QRawFont::fromFont(font);
    const QString face_id = term::qsg_atlas_face_id_for_raw_font(raw_font);
    const term::Glyph_atlas_cache_key base_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        term::qsg_atlas_physical_pixel_size(font, 1.0),
        0);
    const term::Glyph_atlas_cache_key hidpi_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        term::qsg_atlas_physical_pixel_size(font, 2.0),
        0);
    const term::Glyph_atlas_cache_key fallback_key = term::qsg_atlas_cache_key(
        65U,
        QStringLiteral("fallback-face"),
        term::qsg_atlas_physical_pixel_size(font, 1.0),
        0);
    const qreal raw_physical_pixel_size =
        term::qsg_atlas_physical_pixel_size(raw_font, 2.0);

    bool ok = true;
    ok &= check(!face_id.isEmpty(),
        "raw-font face id is populated for cache keys");
    ok &= check(!(base_key == hidpi_key),
        "glyph cache key includes physical pixel size");
    ok &= check(!(base_key == fallback_key),
        "glyph cache key includes fallback face id");
    ok &= check(std::abs(raw_physical_pixel_size - raw_font.pixelSize() * 2.0) < 0.001,
        "raw-font physical pixel size is derived from the run font");
    return ok;
}

bool test_packing_and_stride_copy()
{
    term::Glyph_atlas_packer packer(QSize(10, 8), 1);
    const std::optional<term::Glyph_atlas_slot> first  = packer.pack(QSize(3, 2));
    const std::optional<term::Glyph_atlas_slot> second = packer.pack(QSize(3, 2));
    const std::optional<term::Glyph_atlas_slot> third  = packer.pack(QSize(3, 2));

    bool ok = true;
    ok &= check(first.has_value() && second.has_value() && third.has_value(),
        "shelf packer places multiple glyph tiles");
    ok &= check(first->page == 0 && second->page == 0 && third->page == 0,
        "shelf packer keeps fitting glyphs on one page");
    ok &= check(!first->rect.intersects(second->rect) &&
            !first->rect.intersects(third->rect) &&
            !second->rect.intersects(third->rect),
        "shelf packer places non-overlapping tile rects");

    term::Glyph_coverage_tile tile;
    tile.size           = QSize(2, 2);
    tile.bytes_per_line = 4;
    tile.bytes          = byte_array({1, 2, 90, 91, 3, 4, 92, 93});

    term::Glyph_atlas_cache cache(QSize(8, 8));
    cache.set_epoch(1U);
    const term::Glyph_atlas_cache_key key =
        term::qsg_atlas_cache_key(1U, QStringLiteral("face"), 12.0, 0);
    const term::Glyph_atlas_slot slot = cache.insert_or_get(key, tile);
    const QByteArray& page = cache.page_bytes(slot.page);
    const int stride = cache.stats().page_size.width();
    ok &= check(page.at(slot.rect.y() * stride + slot.rect.x()) == 1 &&
            page.at(slot.rect.y() * stride + slot.rect.x() + 1) == 2 &&
            page.at((slot.rect.y() + 1) * stride + slot.rect.x()) == 3 &&
            page.at((slot.rect.y() + 1) * stride + slot.rect.x() + 1) == 4,
        "atlas cache copies coverage rows using tile stride");

    term::Glyph_atlas_cache capped_cache(QSize(4, 4));
    capped_cache.set_epoch(1U);
    term::Glyph_coverage_tile capped_tile;
    capped_tile.size           = QSize(2, 2);
    capped_tile.bytes_per_line = 2;
    capped_tile.bytes          = byte_array({1, 2, 3, 4});
    const term::Glyph_atlas_slot capped_first = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(2U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    const term::Glyph_atlas_slot capped_second = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(3U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    ok &= check(capped_first.is_valid() &&
            !capped_second.is_valid() &&
            capped_cache.stats().page_count == 1,
        "Stage 1 atlas cache rejects glyphs that would require page 1");
    return ok;
}

bool test_indexed8_and_grayscale_conversion()
{
    uchar indexed_storage[16] = {
        10, 20, 30, 200, 201, 202, 203, 204,
        40, 50, 60, 205, 206, 207, 208, 209,
    };
    QImage indexed(
        indexed_storage,
        3,
        2,
        8,
        QImage::Format_Indexed8);
    QVector<QRgb> color_table(256);
    for (int index = 0; index < color_table.size(); ++index) {
        color_table[index] = qRgb(index, index, index);
    }
    indexed.setColorTable(color_table);

    const term::Glyph_coverage_tile indexed_tile =
        term::qsg_atlas_coverage_tile_from_image(indexed);

    uchar gray_storage[16] = {
        7, 8, 9, 200, 201, 202, 203, 204,
        17, 18, 19, 205, 206, 207, 208, 209,
    };
    QImage gray(
        gray_storage,
        3,
        2,
        8,
        QImage::Format_Grayscale8);
    const term::Glyph_coverage_tile gray_tile =
        term::qsg_atlas_coverage_tile_from_image(gray);

    QImage rgb(3, 2, QImage::Format_RGB32);
    rgb.fill(QColor(10, 20, 30));
    const term::Glyph_coverage_tile rgb_tile =
        term::qsg_atlas_coverage_tile_from_image(rgb);

    bool ok = true;
    ok &= check(indexed_tile.is_valid() &&
            indexed_tile.bytes_per_line == 3 &&
            indexed_tile.bytes == byte_array({10, 20, 30, 40, 50, 60}),
        "Format_Indexed8 glyph alpha map converts to tight R8 coverage rows");
    ok &= check(gray_tile.is_valid() &&
            gray_tile.bytes_per_line == 3 &&
            gray_tile.bytes == byte_array({7, 8, 9, 17, 18, 19}),
        "Format_Grayscale8 glyph alpha map conversion honors source stride");
    ok &= check(!rgb_tile.is_valid(),
        "RGB subpixel glyph maps are rejected for the R8 atlas");
    return ok;
}

bool test_epoch_invalidation()
{
    term::Glyph_coverage_tile tile;
    tile.size           = QSize(2, 2);
    tile.bytes_per_line = 2;
    tile.bytes          = byte_array({11, 12, 13, 14});

    term::Glyph_atlas_cache cache(QSize(8, 8));
    const term::Glyph_atlas_cache_key key =
        term::qsg_atlas_cache_key(4U, QStringLiteral("face"), 14.0, 0);

    cache.set_epoch(100U);
    const term::Glyph_atlas_slot first = cache.insert_or_get(key, tile);
    cache.set_epoch(100U);
    const term::Glyph_atlas_cache_stats stable_stats = cache.stats();
    cache.set_epoch(101U);
    const term::Glyph_atlas_cache_stats invalidated_stats = cache.stats();

    bool ok = true;
    ok &= check(first.is_valid(),
        "atlas cache inserts a glyph before epoch invalidation");
    ok &= check(stable_stats.invalidations == 0U &&
            stable_stats.page_count == 1,
        "same epoch does not invalidate atlas cache");
    ok &= check(cache.find(key) == nullptr &&
            invalidated_stats.invalidations == 1U &&
            invalidated_stats.page_count == 0,
        "changed font epoch invalidates atlas cache pages and keys");
    return ok;
}

bool report_ready_for_render(const term::Qsg_atlas_stage1_frame_report& report)
{
    return
        report.render_count > 0U            &&
        report.drew                         &&
        report.command_buffer_non_null      &&
        report.render_target_non_null       &&
        report.rhi_non_null                 &&
        report.r8_texture_created           &&
        report.r8_upload_recorded           &&
        report.raw_font_rasterized          &&
        report.raw_font_rasterized_in_prepare;
}

bool pump_until(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    const std::function<bool(const term::Qsg_atlas_stage1_frame_report&)>&
                        predicate,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_stage1_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_stage1_frame(surface);
        if (predicate(report)) {
            return true;
        }
    }

    return false;
}

void mutate_surface_after_sync(VNM_TerminalSurface& surface)
{
    surface.set_color_theme(QStringLiteral("default"));
    surface.set_font_size(surface.font_size() + 1.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_text_snapshot(777U, QStringLiteral("B"))));
}

int test_render_smoke(QGuiApplication& app)
{
    const QByteArray render_loop = qgetenv("QSG_RENDER_LOOP");
    const bool threaded_loop = render_loop == QByteArrayLiteral("threaded");
    const bool basic_loop    = render_loop == QByteArrayLiteral("basic");

    QQuickWindow window;
    window.setColor(QColor(4, 6, 8));
    window.resize(260, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(180.0, 96.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_theme(QStringLiteral("light"));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_text_snapshot(601U, QStringLiteral("A"))));
    term::VNM_TerminalSurface_render_bridge::set_qsg_atlas_stage1_probe_enabled(
        surface,
        true);

    std::atomic_bool mutation_started = false;
    std::atomic_bool mutation_done    = false;
    const QMetaObject::Connection mutation_connection = QObject::connect(
        &window,
        &QQuickWindow::beforeRendering,
        &surface,
        [&]() {
            bool expected = false;
            if (!mutation_started.compare_exchange_strong(expected, true)) {
                return;
            }

            const auto mutate = [&]() {
                mutate_surface_after_sync(surface);
                mutation_done.store(true);
            };

            if (QThread::currentThread() == surface.thread()) {
                mutate();
            }
            else {
                QMetaObject::invokeMethod(
                    &surface,
                    [&]() {
                        mutate_surface_after_sync(surface);
                        mutation_done.store(true);
                    },
                    Qt::QueuedConnection);
            }
        },
        Qt::DirectConnection);

    window.show();

    const bool rendered = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_stage1_frame_report& report) {
            return report_ready_for_render(report) && mutation_done.load();
        });
    QObject::disconnect(mutation_connection);

    const term::Qsg_atlas_stage1_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_stage1_frame(surface);
    if (!rendered &&
        (report.prepare_count == 0U ||
            report.render_count == 0U ||
            (report.prepare_count > 0U && !report.rhi_non_null)))
    {
        std::cerr << "SKIP: Stage 1 render smoke did not reach usable QRhi render state\n";
        return k_unsupported_backend_skip_return_code;
    }

    bool ok = true;
    ok &= check(rendered,
        "Stage 1 render smoke draws the captured atlas probe");
    ok &= check(report.first_render_snapshot_sequence == 601U,
        "Stage 1 render smoke uses captured snapshot for first render");
    ok &= check(report.first_render_snapshot_sequence ==
            report.first_captured_snapshot_sequence,
        "Stage 1 first render uses the first captured snapshot value");
    ok &= check(report.first_render_light_options,
        "Stage 1 render smoke uses captured options for first render");
    ok &= check(report.first_render_light_options == report.first_captured_light_options,
        "Stage 1 first render uses the first captured options value");
    ok &= check(report.first_render_font_epoch == report.first_captured_font_epoch,
        "Stage 1 first render uses the first captured font epoch");
    ok &= check(!threaded_loop || mutation_done.load(),
        "threaded Stage 1 smoke mutates GUI-thread state after sync through beforeRendering");
    ok &= check(!basic_loop || mutation_done.load(),
        "basic Stage 1 smoke mutates GUI state after sync through beforeRendering");
    ok &= check(report.raw_font_rasterized_in_prepare &&
            report.prepare_thread_id == report.raw_font_raster_thread_id,
        "Stage 1 smoke rasterizes QRawFont glyphs in prepare");
    return ok ? 0 : 1;
}

bool run_unit_tests()
{
    bool ok = true;
    ok &= test_source_posture();
    ok &= test_cache_key_includes_physical_size_and_face();
    ok &= test_packing_and_stride_copy();
    ok &= test_indexed8_and_grayscale_conversion();
    ok &= test_epoch_invalidation();
    return ok;
}

}

int main(int argc, char** argv)
{
    const bool render_smoke = has_argument(argc, argv, "--render-smoke");
    if (render_smoke) {
        configure_graphics_api(argument_value(argc, argv, "--backend", "d3d11"));
    }

    QGuiApplication app(argc, argv);
    if (render_smoke) {
        return test_render_smoke(app);
    }

    return run_unit_tests() ? 0 : 1;
}
