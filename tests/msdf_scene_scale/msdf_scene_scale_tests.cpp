#include "helpers/test_check.h"

#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QtCore/qfloat16.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>

namespace {

using vnm_terminal::test_helpers::check;

QShader load_shader(const char* name)
{
    QFile file(QStringLiteral(VNM_TERMINAL_MSDF_SHADER_DIR "/") +
        QString::fromLatin1(name));
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "FAIL: cannot open MSDF shader package " << name << '\n';
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

bool test_scene_scale(
    QRhi&          rhi,
    const QShader& vertex_shader,
    const QShader& fragment_shader,
    float          layout_dpr,
    float          scene_scale,
    bool           sdf_alpha)
{
    constexpr float k_layout_extent   = 96.0f;
    constexpr float k_glyph_origin    = 16.0f;
    constexpr float k_glyph_extent    = 32.0f;
    constexpr float k_distance_offset = 1.0f / 128.0f;
    const int target_extent = static_cast<int>(k_layout_extent * layout_dpr * scene_scale);
    const QSize target_size(target_extent, target_extent);

    // A linear signed-distance field has an exact one-pixel coverage ramp.
    // Floating-point texels and texel-center UV bounds avoid normalized-texture
    // quantization. A fractional edge phase also distinguishes minification
    // from a sharper ramp that happens to cross between two pixel centers.
    const QSize atlas_size(64, 2);
    std::array<qfloat16, 64 * 2 * 4> atlas_pixels;
    for (int y = 0; y < atlas_size.height(); ++y) {
        for (int x = 0; x < atlas_size.width(); ++x) {
            const int offset = (y * atlas_size.width() + x) * 4;
            const qfloat16 distance(static_cast<float>(x) / 64.0f);
            atlas_pixels[offset]     = distance;
            atlas_pixels[offset + 1] = distance;
            atlas_pixels[offset + 2] = distance;
            atlas_pixels[offset + 3] = sdf_alpha ? distance : qfloat16(0.0f);
        }
    }

    constexpr std::array<std::array<float, 2>, 4> k_corners = {{
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f},
    }};
    std::array<std::array<float, 26>, 4> vertices;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        vertices[index] = {
            k_corners[index][0], k_corners[index][1],
            k_glyph_origin, k_glyph_origin, k_glyph_extent, k_glyph_extent,
            0.0f, 0.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.5f / 64.0f, 0.25f, 63.5f / 64.0f, 0.75f,
            k_glyph_origin * layout_dpr, k_glyph_origin * layout_dpr,
            k_glyph_extent * layout_dpr, k_glyph_extent * layout_dpr,
        };
    }

    QMatrix4x4 projection;
    projection.ortho(0.0f, k_layout_extent * scene_scale,
        k_layout_extent * scene_scale, 0.0f, -1.0f, 1.0f);
    QMatrix4x4 model;
    model.scale(scene_scale);
    const QMatrix4x4 mvp = rhi.clipSpaceCorrMatrix() * projection * model;
    std::array<float, 24> uniforms = {};
    std::memcpy(uniforms.data(), mvp.constData(), 16 * sizeof(float));
    uniforms[16] = k_glyph_extent * layout_dpr;
    uniforms[17] = static_cast<float>(target_extent);
    uniforms[18] = static_cast<float>(target_extent);
    uniforms[20] = rhi.isYUpInFramebuffer() ? 1.0f : 0.0f;
    uniforms[21] = rhi.isYUpInNDC() ? 1.0f : 0.0f;
    constexpr std::array<float, 4> k_invert = {};

    std::unique_ptr<QRhiBuffer> vertex_buffer(rhi.newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(vertices)));
    std::unique_ptr<QRhiBuffer> uniform_buffer(rhi.newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(uniforms)));
    std::unique_ptr<QRhiBuffer> invert_buffer(rhi.newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(k_invert)));
    std::unique_ptr<QRhiTexture> atlas(rhi.newTexture(
        QRhiTexture::RGBA16F, atlas_size));
    std::unique_ptr<QRhiTexture> target_texture(rhi.newTexture(
        QRhiTexture::RGBA8, target_size, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    std::unique_ptr<QRhiSampler> sampler(rhi.newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!check(vertex_buffer->create() && uniform_buffer->create() &&
            invert_buffer->create() && atlas->create() &&
            target_texture->create() && sampler->create(),
        "scene-scale shader resources create"))
    {
        return false;
    }

    std::unique_ptr<QRhiRenderPassDescriptor> pass;
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi.newTextureRenderTarget(
        QRhiTextureRenderTargetDescription(QRhiColorAttachment(target_texture.get()))));
    pass.reset(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi.newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniform_buffer.get()),
        QRhiShaderResourceBinding::sampledTexture(1,
            QRhiShaderResourceBinding::FragmentStage, atlas.get(), sampler.get()),
        QRhiShaderResourceBinding::uniformBuffer(2,
            QRhiShaderResourceBinding::FragmentStage, invert_buffer.get()),
    });
    if (!check(target->create() && bindings->create(),
            "scene-scale target and shader bindings create"))
    {
        return false;
    }

    QRhiVertexInputLayout input_layout;
    input_layout.setBindings({QRhiVertexInputBinding(sizeof(vertices[0]))});
    input_layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2,  0 * sizeof(float)},
        {0, 1, QRhiVertexInputAttribute::Float4,  2 * sizeof(float)},
        {0, 2, QRhiVertexInputAttribute::Float4,  6 * sizeof(float)},
        {0, 3, QRhiVertexInputAttribute::Float4, 10 * sizeof(float)},
        {0, 4, QRhiVertexInputAttribute::Float4, 14 * sizeof(float)},
        {0, 5, QRhiVertexInputAttribute::Float4, 18 * sizeof(float)},
        {0, 6, QRhiVertexInputAttribute::Float4, 22 * sizeof(float)},
    });
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi.newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader},
        {QRhiShaderStage::Fragment, fragment_shader},
    });
    pipeline->setVertexInputLayout(input_layout);
    pipeline->setShaderResourceBindings(bindings.get());
    pipeline->setRenderPassDescriptor(pass.get());
    if (!check(pipeline->create(), "scene-scale production shader pipeline creates")) {
        return false;
    }

    QRhiCommandBuffer* commands = nullptr;
    if (!check(rhi.beginOffscreenFrame(&commands) == QRhi::FrameOpSuccess,
            "scene-scale shader frame begins"))
    {
        return false;
    }
    QRhiResourceUpdateBatch* updates = rhi.nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertex_buffer.get(), vertices.data());
    updates->updateDynamicBuffer(uniform_buffer.get(), 0, sizeof(uniforms), uniforms.data());
    updates->updateDynamicBuffer(invert_buffer.get(), 0, sizeof(k_invert), k_invert.data());
    updates->uploadTexture(atlas.get(), QRhiTextureUploadDescription({
        QRhiTextureUploadEntry(0, 0,
            QRhiTextureSubresourceUploadDescription(atlas_pixels.data(), sizeof(atlas_pixels))),
    }));
    commands->beginPass(target.get(), Qt::transparent, {1.0f, 0}, updates);
    commands->setGraphicsPipeline(pipeline.get());
    commands->setViewport(QRhiViewport(0.0f, 0.0f,
        static_cast<float>(target_extent), static_cast<float>(target_extent)));
    commands->setShaderResources();
    const QRhiCommandBuffer::VertexInput vertex_input(vertex_buffer.get(), 0);
    commands->setVertexInput(0, 1, &vertex_input);
    commands->draw(4);
    commands->endPass();
    QRhiReadbackResult readback;
    bool completed = false;
    readback.completed = [&]() { completed = true; };
    updates = rhi.nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(target_texture.get()), &readback);
    commands->resourceUpdate(updates);
    if (!check(rhi.endOffscreenFrame() == QRhi::FrameOpSuccess && completed,
            "scene-scale production shader pixels read back"))
    {
        return false;
    }

    const QImage image(reinterpret_cast<const uchar*>(readback.data.constData()),
        target_extent, target_extent, QImage::Format_RGBA8888);
    const float edge =
        (k_glyph_origin + k_glyph_extent * (0.5f + k_distance_offset)) *
        layout_dpr * scene_scale;
    const int row = rhi.isYUpInFramebuffer()
        ? target_extent - 1 - static_cast<int>(edge)
        : static_cast<int>(edge);
    int max_error = 0;
    for (int x = static_cast<int>(edge) - 4; x <= static_cast<int>(edge) + 4; ++x) {
        const float coverage = std::clamp(static_cast<float>(x) + 1.0f - edge, 0.0f, 1.0f);
        const int expected = static_cast<int>(std::lround(coverage * 255.0f));
        max_error = std::max(max_error, std::abs(image.pixelColor(x, row).alpha() - expected));
    }
    std::cout << "msdf_scene_scale dpr=" << layout_dpr << " scale=" << scene_scale
        << " sdf_alpha=" << sdf_alpha << " max_alpha_error=" << max_error << '\n';
    return check(max_error <= 1,
        "MSDF coverage spans one target pixel after the scene transform");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
#if defined(Q_OS_WIN)
    QRhiD3D11InitParams params;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params));
#elif defined(Q_OS_MACOS)
    QRhiMetalInitParams params;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Metal, &params));
#else
    std::unique_ptr<QOffscreenSurface> fallback(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = fallback.get();
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
#endif
    if (rhi == nullptr) {
        std::cerr << "SKIP: native QRhi backend is unavailable\n";
        return 77;
    }
    const QShader vertex_shader   = load_shader("atlas_msdf_text.vert.qsb");
    const QShader fragment_shader = load_shader("atlas_msdf_text.frag.qsb");
    if (!check(vertex_shader.isValid() && fragment_shader.isValid(),
            "production MSDF shader packages load"))
    {
        return 1;
    }
    bool ok = true;
    for (float layout_dpr : {1.0f, 1.25f}) {
        for (float scene_scale : {0.75f, 1.0f, 1.5f, 2.5f, 4.0f}) {
            for (bool sdf_alpha : {false, true}) {
                ok &= test_scene_scale(*rhi, vertex_shader, fragment_shader,
                    layout_dpr, scene_scale, sdf_alpha);
            }
        }
    }
    return ok ? 0 : 1;
}
