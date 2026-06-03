#include "vnm_terminal/internal/qsg_rhi_stage0_probe.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QMatrix4x4>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegion>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QThread>
#include <private/qquickitem_p.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr int k_unsupported_backend_skip_return_code = 77;

QString g_requested_backend_name;
QString g_expected_backend_name;
bool    g_unsupported_backend = false;

struct render_capture_t
{
    QImage                            image;
    term::qsg_rhi_stage0_probe_frame_t
                                      frame;
};

class Fake_render_state final : public QSGRenderNode::RenderState
{
public:
    const QMatrix4x4* projectionMatrix() const override { return &m_projection; }
    QRect scissorRect() const override { return m_scissor_rect; }
    bool scissorEnabled() const override { return m_scissor_enabled; }
    int stencilValue() const override { return m_stencil_value; }
    bool stencilEnabled() const override { return m_stencil_enabled; }
    const QRegion* clipRegion() const override { return &m_clip_region; }

    QMatrix4x4 m_projection;
    QRect      m_scissor_rect = QRect(11, 13, 17, 19);
    QRegion    m_clip_region;
    bool       m_scissor_enabled = true;
    bool       m_stencil_enabled = true;
    int        m_stencil_value   = 23;
};

bool has_argument(int argc, char** argv, const char* expected)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], expected) == 0) {
            return true;
        }
    }

    return false;
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
    else
    if (std::strcmp(backend, "null") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::NullRhi);
    }
    else {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
    }
}

QString expected_backend_name(const char* backend)
{
    if (std::strcmp(backend, "d3d11") == 0) {
        return QStringLiteral("D3D11");
    }
    if (std::strcmp(backend, "d3d12") == 0) {
        return QStringLiteral("D3D12");
    }
    if (std::strcmp(backend, "vulkan") == 0) {
        return QStringLiteral("Vulkan");
    }
    if (std::strcmp(backend, "opengl") == 0) {
        return QStringLiteral("OpenGL");
    }
    if (std::strcmp(backend, "null") == 0) {
        return QStringLiteral("Null");
    }

    return {};
}

bool note_unsupported_backend(const term::qsg_rhi_stage0_probe_frame_t& frame)
{
    if (g_expected_backend_name.isEmpty() || frame.window_rhi_probe_count == 0U) {
        return false;
    }

    if (frame.rhi_backend_name == g_expected_backend_name) {
        return false;
    }

    std::cerr
        << "SKIP: requested backend " << g_requested_backend_name.toStdString()
        << " but active QRhi backend is "
        << (frame.rhi_backend_name.isEmpty()
            ? std::string("<none>")
            : frame.rhi_backend_name.toStdString())
        << '\n';
    g_unsupported_backend = true;
    return true;
}

bool nearly_equal(qreal left, qreal right, qreal tolerance)
{
    return std::abs(left - right) <= tolerance;
}

bool is_probe_pixel(const QColor& color)
{
    return
        color.green() >= 70              &&
        color.green() > color.red() + 30 &&
        color.green() > color.blue() + 20;
}

int count_probe_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (is_probe_pixel(image.pixelColor(x, y))) {
                ++count;
            }
        }
    }

    return count;
}

QRect sample_rect_for_scene_point(
    const QQuickWindow& window,
    QPointF             scene_point,
    int                 radius)
{
    const qreal  dpr = window.devicePixelRatio();
    const QPoint center(
        static_cast<int>(std::lround(scene_point.x() * dpr)),
        static_cast<int>(std::lround(scene_point.y() * dpr)));
    return QRect(
        center.x() - radius,
        center.y() - radius,
        radius * 2 + 1,
        radius * 2 + 1);
}

void print_frame(const char* case_name, const term::qsg_rhi_stage0_probe_frame_t& frame)
{
    std::cout
        << case_name
        << ": backend=" << frame.rhi_backend_name.toStdString()
        << " window_rhi=" << frame.window_rhi_non_null
        << " command_buffer=" << frame.command_buffer_non_null
        << " render_target=" << frame.render_target_non_null
        << " target=" << frame.render_target_pixel_size.width()
        << 'x' << frame.render_target_pixel_size.height()
        << " rp_words=" << frame.render_pass_serialized_format.size()
        << " rebuilds=" << frame.resource_rebuild_count
        << " model_rect=(" << frame.model_mapped_rect.x()
        << ',' << frame.model_mapped_rect.y()
        << ' ' << frame.model_mapped_rect.width()
        << 'x' << frame.model_mapped_rect.height()
        << ')'
        << " scissor=" << frame.contract.scissor_enabled
        << " scissor_rect=(" << frame.contract.scissor_rect.x()
        << ',' << frame.contract.scissor_rect.y()
        << ' ' << frame.contract.scissor_rect.width()
        << 'x' << frame.contract.scissor_rect.height()
        << ')'
        << " stencil=" << frame.contract.stencil_enabled
        << " opacity=" << frame.inherited_opacity
        << '\n';
}

bool common_probe_contract(
    const term::qsg_rhi_stage0_probe_frame_t& frame,
    const char*                               case_name)
{
    print_frame(case_name, frame);

    const int expected_changed_states =
        static_cast<int>(QSGRenderNode::ViewportState | QSGRenderNode::ScissorState);
    bool ok = true;
    ok &= check(frame.window_rhi_probe_count > 0U,
        "stage0 probe must inspect window()->rhi()");
    ok &= check(frame.window_rhi_non_null,
        "stage0 probe must see non-null window()->rhi()");
    ok &= check(frame.rhi_backend_name == g_expected_backend_name,
        "stage0 probe must use the requested QRhi backend");
    ok &= check(frame.command_buffer_non_null,
        "stage0 probe must see non-null QSGRenderNode::commandBuffer()");
    ok &= check(frame.render_target_non_null,
        "stage0 probe must see non-null QSGRenderNode::renderTarget()");
    ok &= check(frame.shader_packages_valid,
        "stage0 probe shader packages must load");
    ok &= check(frame.resources_ready,
        "stage0 probe QRhi resources must prepare");
    ok &= check(frame.projection_matrix_non_null,
        "stage0 probe must see QSGRenderNode::projectionMatrix()");
    ok &= check(frame.matrix_non_null,
        "stage0 probe must see QSGRenderNode::matrix()");
    ok &= check(frame.used_projection_times_matrix,
        "stage0 probe must prepare projectionMatrix() * matrix()");
    ok &= check(frame.pass_count == 1,
        "stage0 probe must record its one pass");
    ok &= check(nearly_equal(frame.pass_opacity, frame.inherited_opacity, 0.001),
        "stage0 probe pass opacity must come from inheritedOpacity()");
    ok &= check(!frame.contract.depth_test_enabled,
        "stage0 probe must leave depth testing disabled");
    ok &= check(!frame.contract.depth_write_enabled,
        "stage0 probe must leave depth writes disabled");
    ok &= check(frame.changed_states == expected_changed_states,
        "stage0 probe changedStates must report ViewportState | ScissorState");
    ok &= check(frame.rendering_flags == static_cast<int>(QSGRenderNode::NoExternalRendering),
        "stage0 probe flags must report NoExternalRendering");
    ok &= check(frame.viewport_rect.width() > 0 && frame.viewport_rect.height() > 0,
        "stage0 probe must set a positive viewport");
    ok &= check(frame.drew,
        "stage0 probe must issue the instanced quad draw");
    return ok;
}

bool pump_until_probe(
    QGuiApplication&       app,
    QQuickWindow&          window,
    VNM_TerminalSurface&   surface,
    const QRect&           colored_probe,
    render_capture_t&      out)
{
    for (int attempt = 0; attempt < 80; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out.image = window.grabWindow();
        out.frame = term::VNM_TerminalSurface_render_bridge::rhi_stage0_probe_frame(
            surface);

        if (note_unsupported_backend(out.frame)) {
            return false;
        }

        if (out.frame.render_count == 0U || !out.frame.drew || out.image.isNull()) {
            continue;
        }

        if (count_probe_pixels(out.image, colored_probe) > colored_probe.width()) {
            return true;
        }
    }

    return false;
}

bool test_stencil_contract()
{
    Fake_render_state state;
    const term::qsg_rhi_stage0_probe_contract_t contract =
        term::qsg_rhi_stage0_probe_contract_for_state(state);
    bool ok = true;
    ok &= check(contract.scissor_enabled,
        "fake render state must drive scissor-enabled contract");
    ok &= check(contract.scissor_rect == state.m_scissor_rect,
        "scissor contract must use RenderState::scissorRect() coordinates");
    ok &= check(contract.stencil_enabled,
        "fake render state must drive stencil-enabled contract");
    ok &= check(contract.stencil_ref == state.m_stencil_value,
        "stencil contract must use RenderState::stencilValue()");
    ok &= check(contract.stencil_ref_from_render_state,
        "stencil contract must mark the stencil ref as coming from RenderState");
    ok &= check(contract.stencil_compare_equal,
        "stencil contract must use compare EQUAL");
    ok &= check(contract.stencil_ops_keep,
        "stencil contract must use KEEP operations");
    ok &= check(contract.stencil_read_mask == 0xff && contract.stencil_write_mask == 0xff,
        "stencil contract must use 0xff read/write masks");
    ok &= check(!contract.depth_test_enabled && !contract.depth_write_enabled,
        "stencil contract must keep depth test/write disabled");
    ok &= check(
        term::qsg_rhi_stage0_probe_changed_states() ==
            (QSGRenderNode::ViewportState | QSGRenderNode::ScissorState),
        "stage0 changedStates helper must return ViewportState | ScissorState");
    return ok;
}

bool test_normal_transformed_host(QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(3, 7, 11));
    window.resize(260, 180);

    QQuickItem host;
    host.setParentItem(window.contentItem());
    host.setPosition(QPointF(20.0, 18.0));
    host.setScale(1.5);
    host.setTransformOrigin(QQuickItem::TopLeft);
    host.setOpacity(0.5);
    host.setSize(QSizeF(80.0, 60.0));

    VNM_TerminalSurface surface;
    surface.setParentItem(&host);
    surface.setClip(false);
    surface.setSize(QSizeF(80.0, 60.0));
    term::VNM_TerminalSurface_render_bridge::set_rhi_stage0_probe_enabled(
        surface,
        true);

    window.show();

    render_capture_t capture;
    if (!pump_until_probe(app, window, surface, QRect(124, 50, 8, 8), capture)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: normal transformed host did not render probe pixels\n";
        return false;
    }

    bool ok = common_probe_contract(capture.frame, "normal");
    ok &= check(!capture.frame.contract.scissor_enabled,
        "normal unclipped host must not receive a scissor clip");
    ok &= check(!capture.frame.contract.stencil_enabled,
        "normal unclipped host must not receive a stencil clip");
    ok &= check(nearly_equal(capture.frame.inherited_opacity, 0.5, 0.02),
        "translated/scaled host opacity must reach inheritedOpacity()");
    ok &= check(count_probe_pixels(capture.image, QRect(124, 50, 8, 8)) > 20,
        "scaled ancestor must extend the QRhi quad through projectionMatrix() * matrix()");
    ok &= check(count_probe_pixels(capture.image, QRect(5, 50, 10, 10)) == 0,
        "translated ancestor must move the QRhi quad away from the origin");
    return ok;
}

bool test_scissor_clipped_host(QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(2, 6, 10));
    window.resize(240, 160);

    QQuickItem clip_host;
    clip_host.setParentItem(window.contentItem());
    clip_host.setPosition(QPointF(30.0, 30.0));
    clip_host.setSize(QSizeF(60.0, 45.0));
    clip_host.setClip(true);

    VNM_TerminalSurface surface;
    surface.setParentItem(&clip_host);
    surface.setClip(false);
    surface.setSize(QSizeF(130.0, 80.0));
    term::VNM_TerminalSurface_render_bridge::set_rhi_stage0_probe_enabled(
        surface,
        true);

    window.show();

    render_capture_t capture;
    if (!pump_until_probe(app, window, surface, QRect(40, 40, 8, 8), capture)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: clipped host did not render probe pixels inside clip\n";
        return false;
    }

    bool ok = common_probe_contract(capture.frame, "scissor");
    ok &= check(capture.frame.contract.scissor_enabled,
        "rectangular clip host must enable scissor");
    ok &= check(capture.frame.contract.scissor_rect.width() > 0 &&
            capture.frame.contract.scissor_rect.height() > 0,
        "rectangular clip host must provide a positive scissor rect");
    ok &= check(count_probe_pixels(capture.image, QRect(40, 40, 8, 8)) > 20,
        "rectangular clip host must render inside state->scissorRect()");

    const QRect outside_scissor(
        capture.frame.contract.scissor_rect.right() + 8,
        capture.frame.contract.scissor_rect.y() + 8,
        8,
        8);
    ok &= check(count_probe_pixels(capture.image, outside_scissor) == 0,
        "rectangular clip host must clip outside state->scissorRect()");
    return ok;
}

bool test_stencil_clipped_host(QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(4, 8, 12));
    window.resize(280, 220);

    QQuickItem clip_host;
    clip_host.setParentItem(window.contentItem());
    clip_host.setPosition(QPointF(80.0, 54.0));
    clip_host.setSize(QSizeF(70.0, 54.0));
    clip_host.setClip(true);
    clip_host.setTransformOrigin(QQuickItem::TopLeft);
    clip_host.setRotation(16.0);

    VNM_TerminalSurface surface;
    surface.setParentItem(&clip_host);
    surface.setClip(false);
    surface.setSize(QSizeF(130.0, 82.0));
    term::VNM_TerminalSurface_render_bridge::set_rhi_stage0_probe_enabled(
        surface,
        true);

    window.show();

    render_capture_t capture;
    const QRect stencil_probe_region(70, 40, 150, 130);
    if (!pump_until_probe(app, window, surface, stencil_probe_region, capture)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: stencil clipped host did not render probe pixels inside clip\n";
        return false;
    }

    bool ok = common_probe_contract(capture.frame, "stencil");
    ok &= check(capture.frame.contract.stencil_enabled,
        "rotated clip host must enable stencil clipping");
    ok &= check(capture.frame.contract.stencil_pipeline_selected,
        "rotated clip host must use the stencil QRhi pipeline");
    ok &= check(capture.frame.contract.stencil_ref_from_render_state,
        "stencil clip must set the stencil ref from RenderState::stencilValue()");
    ok &= check(capture.frame.contract.stencil_compare_equal,
        "stencil clip must use compare EQUAL");
    ok &= check(capture.frame.contract.stencil_ops_keep,
        "stencil clip must use KEEP operations");
    ok &= check(
        capture.frame.contract.stencil_read_mask == 0xff &&
            capture.frame.contract.stencil_write_mask == 0xff,
        "stencil clip must use 0xff read/write masks");
    ok &= check(count_probe_pixels(capture.image, stencil_probe_region) > 200,
        "stencil clip host must composite probe pixels inside the clipped shape");

    const QRect outside_right = sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(100.0, 12.0)),
        3);
    const QRect outside_lower = sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(92.0, 45.0)),
        3);
    ok &= check(count_probe_pixels(capture.image, outside_right) == 0,
        "stencil clip must reject probe pixels right of the rotated clip");
    ok &= check(count_probe_pixels(capture.image, outside_lower) == 0,
        "stencil clip must reject probe pixels outside the rotated clip body");
    return ok;
}

bool test_layer_host(QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(1, 5, 9));
    window.resize(320, 200);

    QQuickItem layer_host;
    layer_host.setParentItem(window.contentItem());
    layer_host.setPosition(QPointF(26.0, 24.0));
    layer_host.setSize(QSizeF(110.0, 70.0));
    QQuickItemPrivate::get(&layer_host)->layer()->setEnabled(true);

    VNM_TerminalSurface surface;
    surface.setParentItem(&layer_host);
    surface.setClip(false);
    surface.setSize(QSizeF(100.0, 60.0));
    term::VNM_TerminalSurface_render_bridge::set_rhi_stage0_probe_enabled(
        surface,
        true);

    window.show();

    render_capture_t capture;
    if (!pump_until_probe(app, window, surface, QRect(40, 40, 8, 8), capture)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: layer host did not render probe pixels\n";
        return false;
    }

    bool ok = common_probe_contract(capture.frame, "layer");
    ok &= check(count_probe_pixels(capture.image, QRect(40, 40, 8, 8)) > 20,
        "layer host must composite the QRhi probe");
    ok &= check(capture.frame.render_target_pixel_size.width() < capture.image.width(),
        "layer host must render through a smaller redirected render target width");
    ok &= check(capture.frame.render_target_pixel_size.height() < capture.image.height(),
        "layer host must render through a smaller redirected render target height");
    return ok;
}

bool test_layer_toggle_after_first_render(QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(6, 10, 14));
    window.resize(320, 200);

    QQuickItem layer_host;
    layer_host.setParentItem(window.contentItem());
    layer_host.setPosition(QPointF(24.0, 22.0));
    layer_host.setSize(QSizeF(110.0, 70.0));

    VNM_TerminalSurface surface;
    surface.setParentItem(&layer_host);
    surface.setClip(false);
    surface.setSize(QSizeF(100.0, 60.0));
    term::VNM_TerminalSurface_render_bridge::set_rhi_stage0_probe_enabled(
        surface,
        true);

    window.show();

    render_capture_t before_layer;
    if (!pump_until_probe(app, window, surface, QRect(40, 40, 8, 8), before_layer)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: layer-toggle host did not render before enabling layer\n";
        return false;
    }

    bool ok = common_probe_contract(before_layer.frame, "layer-toggle-before");
    const QVector<quint32> before_format =
        before_layer.frame.render_pass_serialized_format;
    const std::uint64_t before_rebuild_count =
        before_layer.frame.resource_rebuild_count;

    QQuickItemPrivate::get(&layer_host)->layer()->setEnabled(true);
    layer_host.update();
    surface.update();

    render_capture_t after_layer;
    if (!pump_until_probe(app, window, surface, QRect(40, 40, 8, 8), after_layer)) {
        if (g_unsupported_backend) {
            return true;
        }
        std::cerr << "FAIL: layer-toggle host did not render after enabling layer\n";
        return false;
    }

    ok &= common_probe_contract(after_layer.frame, "layer-toggle-after");
    ok &= check(after_layer.frame.render_target_pixel_size.width() <
            after_layer.image.width(),
        "layer-toggle host must redirect rendering after enabling layer");

    if (after_layer.frame.render_pass_serialized_format == before_format) {
        ok &= check(after_layer.frame.resource_rebuild_count == before_rebuild_count,
            "compatible render-pass format must not force a pointer-keyed rebuild");
    }
    else {
        ok &= check(after_layer.frame.resource_rebuild_count > before_rebuild_count,
            "changed render-pass format must rebuild QRhi pipelines");
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    const char* backend = argument_value(argc, argv, "--backend", "d3d11");
    g_requested_backend_name = QString::fromLatin1(backend);
    g_expected_backend_name  = expected_backend_name(backend);
    if (g_expected_backend_name.isEmpty()) {
        std::cerr << "FAIL: unknown Stage0 backend " << backend << '\n';
        return 1;
    }

    configure_graphics_api(backend);

    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_stencil_contract();
    ok &= test_normal_transformed_host(app);
    if (g_unsupported_backend) {
        return k_unsupported_backend_skip_return_code;
    }
    ok &= test_scissor_clipped_host(app);
    if (g_unsupported_backend) {
        return k_unsupported_backend_skip_return_code;
    }
    ok &= test_stencil_clipped_host(app);
    if (g_unsupported_backend) {
        return k_unsupported_backend_skip_return_code;
    }
    ok &= test_layer_host(app);
    if (g_unsupported_backend) {
        return k_unsupported_backend_skip_return_code;
    }
    ok &= test_layer_toggle_after_first_render(app);
    if (g_unsupported_backend) {
        return k_unsupported_backend_skip_return_code;
    }

    if (!ok && has_argument(argc, argv, "--allow-failure")) {
        std::cerr << "Stage0 probe failed under backend " << backend << '\n';
        return 0;
    }

    return ok ? 0 : 1;
}
