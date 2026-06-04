#include "vnm_terminal/internal/qsg_atlas_renderer_stage1.h"

#include <QFile>
#include <QGlyphRun>
#include <QMatrix4x4>
#include <QSGRenderNode>
#include <QTextLayout>
#include <QTextOption>
#include <QThread>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr const char* k_stage1_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/stage0_quad.vert.qsb";
constexpr const char* k_stage1_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/stage0_quad.frag.qsb";
constexpr const char* k_stage2_glyph_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/stage2_glyph.vert.qsb";
constexpr const char* k_stage2_glyph_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/stage2_glyph.frag.qsb";
constexpr qreal k_no_wrap_text_line_width = 1024.0 * 1024.0;

struct Stage1_vertex
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Stage1_instance
{
    float rect[4]  = {};
    float color[4] = {};
};

struct Stage1_glyph_instance
{
    float rect[4]    = {};
    float uv_rect[4] = {};
    float color[4]   = {};
};

struct Stage1_uniform
{
    float matrix[16] = {};
};

struct Stage1_pass_range
{
    quint32 first = 0U;
    quint32 count = 0U;

    bool has_instances() const { return count > 0U; }
};

struct Stage1_prepare_result
{
    bool          raw_font_rasterized = false;
    std::uint64_t raster_thread       = 0U;
    int           rasterized_glyphs   = 0;
};

const std::array<Stage1_vertex, 6> k_stage1_quad_vertices = {{
    {0.0f, 0.0f},
    {1.0f, 0.0f},
    {0.0f, 1.0f},
    {0.0f, 1.0f},
    {1.0f, 0.0f},
    {1.0f, 1.0f},
}};

template <typename T>
void delete_resource(T*& resource)
{
    delete resource;
    resource = nullptr;
}

std::uint64_t current_thread_id()
{
    return static_cast<std::uint64_t>(
        reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

QShader load_shader(const char* path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    return QShader::fromSerialized(file.readAll());
}

bool captured_options_are_light(const Captured_atlas_frame& frame)
{
    return frame.options.default_background.red() > 128;
}

std::uint64_t snapshot_sequence(const Captured_atlas_frame& frame)
{
    return frame.snapshot != nullptr
        ? frame.snapshot->metadata.sequence
        : 0U;
}

QRhiGraphicsPipeline::TargetBlend stage1_blend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

QRhiVertexInputLayout stage1_vertex_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(Stage1_vertex)),
        QRhiVertexInputBinding(
            sizeof(Stage1_instance),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(Stage1_vertex, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(Stage1_instance, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(Stage1_instance, color)),
    });
    return layout;
}

QRhiVertexInputLayout stage1_glyph_vertex_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(Stage1_vertex)),
        QRhiVertexInputBinding(
            sizeof(Stage1_glyph_instance),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(Stage1_vertex, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(Stage1_glyph_instance, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(Stage1_glyph_instance, uv_rect)),
        QRhiVertexInputAttribute(
            1,
            3,
            QRhiVertexInputAttribute::Float4,
            offsetof(Stage1_glyph_instance, color)),
    });
    return layout;
}

void store_color(float* target, QColor color, qreal opacity)
{
    const qreal alpha_ratio = std::clamp(color.alphaF() * opacity, 0.0, 1.0);
    target[0] = static_cast<float>(
        std::round(static_cast<qreal>(color.red())   * alpha_ratio) / 255.0);
    target[1] = static_cast<float>(
        std::round(static_cast<qreal>(color.green()) * alpha_ratio) / 255.0);
    target[2] = static_cast<float>(
        std::round(static_cast<qreal>(color.blue())  * alpha_ratio) / 255.0);
    target[3] = static_cast<float>(std::round(alpha_ratio * 255.0) / 255.0);
}

QColor atlas_cursor_graphic_overlay_color(QColor color)
{
    if (color.alpha() == 254) {
        // Match QSG's framebuffer rounding for the near-opaque cursor
        // graphic overlay over its graphic underlay.
        color.setRed(std::min(255, color.red() + 1));
        color.setGreen(std::min(255, color.green() + 1));
        color.setBlue(std::min(255, color.blue() + 1));
    }
    return color;
}

void store_rect(float* target, const QRectF& rect)
{
    target[0] = static_cast<float>(rect.x());
    target[1] = static_cast<float>(rect.y());
    target[2] = static_cast<float>(rect.width());
    target[3] = static_cast<float>(rect.height());
}

void store_uv_rect(float* target, const QRectF& rect)
{
    target[0] = static_cast<float>(rect.x());
    target[1] = static_cast<float>(rect.y());
    target[2] = static_cast<float>(rect.width());
    target[3] = static_cast<float>(rect.height());
}

class Stage1_atlas_render_node final : public QSGRenderNode
{
public:
    explicit Stage1_atlas_render_node(
        std::shared_ptr<Qsg_atlas_stage1_recorder> recorder)
    :
        m_recorder(std::move(recorder))
    {}

    ~Stage1_atlas_render_node() override
    {
        releaseResources();
    }

    void set_frame(
        Captured_atlas_frame                    frame,
        std::shared_ptr<Qsg_atlas_stage1_recorder>
                                                recorder)
    {
        m_frame    = std::move(frame);
        m_recorder = std::move(recorder);
        if (m_recorder != nullptr) {
            m_recorder->record_capture(m_frame);
        }
    }

    StateFlags changedStates() const override
    {
        return ViewportState | ScissorState;
    }

    RenderingFlags flags() const override
    {
        return NoExternalRendering;
    }

    QRectF rect() const override
    {
        return QRectF(QPointF(0.0, 0.0), m_frame.logical_size);
    }

    void prepare() override
    {
        QRhiCommandBuffer* const command_buffer = commandBuffer();
        QRhiRenderTarget* const  target         = renderTarget();
        QRhi* const              rhi            = target != nullptr ? target->rhi() : nullptr;
        const bool command_buffer_non_null      = command_buffer != nullptr;
        const bool render_target_non_null       = target != nullptr;
        const bool rhi_non_null                 = rhi != nullptr;
        const std::uint64_t prepare_thread      = current_thread_id();

        bool r8_texture_created  = false;
        bool r8_upload_recorded  = false;
        bool raw_font_rasterized = false;
        std::uint64_t raster_thread = 0U;
        int rasterized_glyphs       = 0;

        const Stage1_prepare_result prepare_result = prepare_stage2_instances();
        raw_font_rasterized = prepare_result.raw_font_rasterized;
        raster_thread       = prepare_result.raster_thread;
        rasterized_glyphs   = prepare_result.rasterized_glyphs;

        if (rhi != nullptr && command_buffer != nullptr && target != nullptr) {
            const bool rect_ready = ensure_rect_resources(rhi, target);
            const bool atlas_ready = upload_coverage_texture(
                rhi,
                command_buffer,
                &r8_texture_created,
                &r8_upload_recorded);
            const bool glyph_ready =
                m_glyph_instances.empty() || ensure_glyph_resources(rhi, target);
            m_resources_ready = rect_ready &&
                atlas_ready &&
                glyph_ready &&
                update_stage2_buffers(rhi, command_buffer);
        }
        else {
            m_resources_ready = false;
        }

        if (m_recorder != nullptr) {
            m_recorder->record_prepare(
                m_frame,
                command_buffer_non_null,
                render_target_non_null,
                rhi_non_null,
                r8_texture_created,
                r8_upload_recorded,
                raw_font_rasterized,
                raw_font_rasterized && raster_thread == prepare_thread,
                rasterized_glyphs,
                prepare_thread,
                raster_thread,
                m_cache.stats());
        }
    }

    void render(const RenderState* state) override
    {
        QRhiCommandBuffer* const command_buffer = commandBuffer();
        QRhiRenderTarget* const  target         = renderTarget();

        QRect viewport_rect;
        bool drew = false;
        if (command_buffer != nullptr && target != nullptr && m_resources_ready) {
            const QSize target_size = target->pixelSize();
            viewport_rect = QRect(QPoint(0, 0), target_size);
            command_buffer->setViewport(QRhiViewport(
                0.0f,
                0.0f,
                static_cast<float>(target_size.width()),
                static_cast<float>(target_size.height())));

            const bool scissor_enabled = state != nullptr && state->scissorEnabled();
            const QRect scissor_rect = scissor_enabled
                ? state->scissorRect()
                : viewport_rect;
            command_buffer->setScissor(QRhiScissor(
                scissor_rect.x(),
                scissor_rect.y(),
                scissor_rect.width(),
                scissor_rect.height()));

            draw_rect_pass(command_buffer, m_background_pass);
            draw_rect_pass(command_buffer, m_selection_pass);
            draw_rect_pass(command_buffer, m_cursor_graphic_underlay_pass);
            draw_glyph_pass(command_buffer, m_text_pass);
            draw_rect_pass(command_buffer, m_decoration_pass);
            draw_rect_pass(command_buffer, m_cursor_pass);
            draw_rect_pass(command_buffer, m_cursor_graphic_pass);
            draw_glyph_pass(command_buffer, m_cursor_text_pass);
            drew = total_instance_count() > 0U;
        }

        if (m_recorder != nullptr) {
            m_recorder->record_render(
                m_frame,
                viewport_rect,
                drew);
        }
    }

    void releaseResources() override
    {
        delete_resource(m_glyph_pipeline);
        delete_resource(m_rect_pipeline);
        delete_resource(m_glyph_shader_resources);
        delete_resource(m_rect_shader_resources);
        delete_resource(m_coverage_sampler);
        delete_resource(m_uniform_buffer);
        delete_resource(m_glyph_instance_buffer);
        delete_resource(m_rect_instance_buffer);
        delete_resource(m_vertex_buffer);
        delete_resource(m_coverage_texture);
        m_resource_rhi                  = nullptr;
        m_render_pass_serialized_format.clear();
        m_render_target_samples         = 0;
        m_rect_instance_buffer_size     = 0U;
        m_glyph_instance_buffer_size    = 0U;
        m_static_vertex_upload_needed   = true;
        m_resources_ready               = false;
    }

private:
    bool ensure_shaders()
    {
        if (!m_shader_packages_checked) {
            m_vertex_shader              = load_shader(k_stage1_vertex_shader_path);
            m_fragment_shader            = load_shader(k_stage1_fragment_shader_path);
            m_glyph_vertex_shader        = load_shader(k_stage2_glyph_vertex_shader_path);
            m_glyph_fragment_shader      = load_shader(k_stage2_glyph_fragment_shader_path);
            m_shader_packages_checked = true;
        }

        return
            m_vertex_shader.isValid()         &&
            m_fragment_shader.isValid()       &&
            m_glyph_vertex_shader.isValid()   &&
            m_glyph_fragment_shader.isValid();
    }

    bool ensure_rect_resources(QRhi* rhi, QRhiRenderTarget* target)
    {
        QRhiRenderPassDescriptor* const render_pass_descriptor =
            target->renderPassDescriptor();
        const QVector<quint32> render_pass_format =
            render_pass_descriptor != nullptr
                ? render_pass_descriptor->serializedFormat()
                : QVector<quint32>();
        const int sample_count = target->sampleCount();
        if (m_resource_rhi                  != rhi                ||
            m_render_pass_serialized_format != render_pass_format ||
            m_render_target_samples         != sample_count)
        {
            releaseResources();
            m_resource_rhi                  = rhi;
            m_render_pass_serialized_format = render_pass_format;
            m_render_target_samples         = sample_count;
        }

        if (m_vertex_buffer != nullptr) {
            return true;
        }

        if (!ensure_shaders()) {
            return false;
        }

        m_vertex_buffer = rhi->newBuffer(
            QRhiBuffer::Immutable,
            QRhiBuffer::VertexBuffer,
            static_cast<quint32>(
                sizeof(Stage1_vertex) * k_stage1_quad_vertices.size()));
        m_uniform_buffer = rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            static_cast<quint32>(rhi->ubufAligned(sizeof(Stage1_uniform))));
        if (!m_vertex_buffer->create() ||
            !m_uniform_buffer->create())
        {
            releaseResources();
            return false;
        }

        m_rect_shader_resources = rhi->newShaderResourceBindings();
        m_rect_shader_resources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_uniform_buffer),
        });
        if (!m_rect_shader_resources->create()) {
            releaseResources();
            return false;
        }

        m_rect_pipeline = rhi->newGraphicsPipeline();
        m_rect_pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        m_rect_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        m_rect_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_rect_pipeline->setTargetBlends({stage1_blend()});
        m_rect_pipeline->setDepthTest(false);
        m_rect_pipeline->setDepthWrite(false);
        m_rect_pipeline->setSampleCount(m_render_target_samples);
        m_rect_pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_fragment_shader),
        });
        m_rect_pipeline->setVertexInputLayout(stage1_vertex_input_layout());
        m_rect_pipeline->setShaderResourceBindings(m_rect_shader_resources);
        m_rect_pipeline->setRenderPassDescriptor(render_pass_descriptor);
        if (!m_rect_pipeline->create()) {
            releaseResources();
            return false;
        }

        m_static_vertex_upload_needed = true;
        return true;
    }

    bool ensure_coverage_texture(QRhi* rhi)
    {
        const QSize page_size = m_cache.stats().page_size;
        if (m_coverage_texture != nullptr &&
            m_coverage_texture->pixelSize() == page_size)
        {
            return true;
        }

        delete_resource(m_glyph_pipeline);
        delete_resource(m_glyph_shader_resources);
        delete_resource(m_coverage_texture);

        QRhiTexture* texture = rhi->newTexture(QRhiTexture::R8, page_size);
        if (texture == nullptr || !texture->create()) {
            delete_resource(texture);
            return false;
        }

        m_coverage_texture = texture;
        return true;
    }

    bool ensure_glyph_resources(QRhi* rhi, QRhiRenderTarget* target)
    {
        if (m_glyph_pipeline != nullptr) {
            return true;
        }

        if (m_coverage_texture == nullptr || !ensure_shaders()) {
            return false;
        }

        if (m_coverage_sampler == nullptr) {
            QRhiSampler* sampler = rhi->newSampler(
                QRhiSampler::Nearest,
                QRhiSampler::Nearest,
                QRhiSampler::None,
                QRhiSampler::ClampToEdge,
                QRhiSampler::ClampToEdge);
            if (sampler == nullptr || !sampler->create()) {
                delete_resource(sampler);
                return false;
            }

            m_coverage_sampler = sampler;
        }

        delete_resource(m_glyph_shader_resources);
        QRhiShaderResourceBindings* shader_resources =
            rhi->newShaderResourceBindings();
        if (shader_resources == nullptr) {
            delete_resource(shader_resources);
            return false;
        }

        shader_resources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_uniform_buffer),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_coverage_texture,
                m_coverage_sampler),
        });
        if (!shader_resources->create()) {
            delete_resource(shader_resources);
            return false;
        }

        QRhiRenderPassDescriptor* const render_pass_descriptor =
            target->renderPassDescriptor();
        QRhiGraphicsPipeline* glyph_pipeline = rhi->newGraphicsPipeline();
        if (glyph_pipeline == nullptr) {
            delete_resource(shader_resources);
            delete_resource(glyph_pipeline);
            return false;
        }

        glyph_pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        glyph_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        glyph_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        glyph_pipeline->setTargetBlends({stage1_blend()});
        glyph_pipeline->setDepthTest(false);
        glyph_pipeline->setDepthWrite(false);
        glyph_pipeline->setSampleCount(m_render_target_samples);
        glyph_pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_glyph_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_glyph_fragment_shader),
        });
        glyph_pipeline->setVertexInputLayout(stage1_glyph_vertex_input_layout());
        glyph_pipeline->setShaderResourceBindings(shader_resources);
        glyph_pipeline->setRenderPassDescriptor(render_pass_descriptor);
        if (!glyph_pipeline->create()) {
            delete_resource(glyph_pipeline);
            delete_resource(shader_resources);
            return false;
        }

        m_glyph_shader_resources = shader_resources;
        m_glyph_pipeline         = glyph_pipeline;
        return true;
    }

    Stage1_pass_range append_rect_pass(
        const std::vector<Terminal_render_rect>& rects,
        qreal                                    opacity)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : rects) {
            append_rect_instance(rect.rect, rect.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    Stage1_pass_range append_decoration_pass(
        const std::vector<Terminal_render_decoration>& decorations,
        qreal                                          opacity)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_decoration& decoration : decorations) {
            append_rect_instance(decoration.rect, decoration.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    Stage1_pass_range append_cursor_pass(
        const std::vector<Terminal_render_cursor_primitive>& cursors,
        qreal                                                opacity)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_cursor_primitive& cursor : cursors) {
            append_rect_instance(cursor.rect, cursor.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    Stage1_pass_range append_cursor_graphic_pass(
        const std::vector<Terminal_render_rect>& rects,
        qreal                                    opacity)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : rects) {
            append_rect_instance(
                rect.rect,
                atlas_cursor_graphic_overlay_color(rect.color),
                opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    Stage1_pass_range append_text_pass(
        const std::vector<Terminal_render_text_run>& runs,
        qreal                                        opacity,
        Stage1_prepare_result&                       result)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_glyph_instances.size());
        for (const Terminal_render_text_run& run : runs) {
            append_text_run(run, opacity, result);
        }
        range.count =
            static_cast<quint32>(m_glyph_instances.size()) - range.first;
        return range;
    }

    void append_rect_instance(
        const QRectF& rect,
        QColor        color,
        qreal         opacity)
    {
        if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
            return;
        }

        Stage1_instance instance;
        store_rect(instance.rect, rect);
        store_color(instance.color, color, opacity);
        m_rect_instances.push_back(instance);
    }

    Stage1_prepare_result prepare_stage2_instances()
    {
        m_cache.set_epoch(m_frame.font_epoch);
        m_rect_instances.clear();
        m_glyph_instances.clear();
        m_background_pass     = {};
        m_selection_pass      = {};
        m_cursor_graphic_underlay_pass = {};
        m_text_pass           = {};
        m_decoration_pass     = {};
        m_cursor_pass         = {};
        m_cursor_graphic_pass = {};
        m_cursor_text_pass    = {};

        Stage1_prepare_result result;
        Terminal_render_options options = m_frame.options;
        options.packed_text_sidecars_enabled = false;
        const Terminal_render_frame render_frame = build_terminal_render_frame(
            m_frame.snapshot.get(),
            m_frame.logical_size,
            m_frame.cell_metrics,
            options,
            m_frame.cursor_blink_visible,
            &m_frame.ime_preedit);

        const qreal opacity = std::clamp(inheritedOpacity(), 0.0, 1.0);
        m_background_pass     = append_rect_pass(render_frame.background_rects, opacity);
        m_selection_pass      = append_rect_pass(render_frame.selection_rects, opacity);
        m_cursor_graphic_underlay_pass =
            append_cursor_graphic_underlay_pass(render_frame, opacity);
        m_text_pass           = append_text_pass(render_frame.text_runs, opacity, result);
        m_decoration_pass     = append_decoration_pass(render_frame.decorations, opacity);
        m_cursor_pass         = append_cursor_pass(render_frame.cursors, opacity);
        // Full graphics rendering is a Stage 3 concern; Stage 2 only needs
        // the block-cursor carve-out rects emitted by the shared frame builder.
        m_cursor_graphic_pass =
            append_cursor_graphic_pass(render_frame.cursor_graphic_rects, opacity);
        m_cursor_text_pass    = append_text_pass(render_frame.cursor_text_runs, opacity, result);
        result.raw_font_rasterized = result.rasterized_glyphs > 0;
        return result;
    }

    Stage1_pass_range append_cursor_graphic_underlay_pass(
        const Terminal_render_frame& render_frame,
        qreal                        opacity)
    {
        Stage1_pass_range range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& graphic_rect : render_frame.graphic_rects) {
            const bool intersects_cursor_graphic = std::any_of(
                render_frame.cursor_graphic_rects.begin(),
                render_frame.cursor_graphic_rects.end(),
                [&](const Terminal_render_rect& cursor_graphic_rect) {
                    return graphic_rect.rect.intersects(cursor_graphic_rect.rect);
                });
            if (intersects_cursor_graphic) {
                append_rect_instance(graphic_rect.rect, graphic_rect.color, opacity);
            }
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    void append_text_run(
        const Terminal_render_text_run& run,
        qreal                           opacity,
        Stage1_prepare_result&          result)
    {
        if (run.text.isEmpty()) {
            return;
        }

        QTextLayout layout(run.text, m_frame.font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.setCacheEnabled(false);

        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            layout.endLayout();
            return;
        }
        line.setLineWidth(k_no_wrap_text_line_width);
        line.setPosition(QPointF(0.0, 0.0));
        const qreal line_ascent = line.ascent();
        const QList<QGlyphRun> glyph_runs = line.glyphRuns(
            0,
            run.text.size(),
            QTextLayout::RetrieveGlyphIndexes |
                QTextLayout::RetrieveGlyphPositions);
        layout.endLayout();

        const QPointF layout_origin(
            run.baseline_origin.x(),
            run.baseline_origin.y() - line_ascent);
        for (const QGlyphRun& glyph_run : glyph_runs) {
            append_glyph_run(glyph_run, run, layout_origin, opacity, result);
        }
    }

    void append_glyph_run(
        const QGlyphRun&                glyph_run,
        const Terminal_render_text_run& run,
        QPointF                         layout_origin,
        qreal                           opacity,
        Stage1_prepare_result&          result)
    {
        const QList<quint32> glyph_indexes = glyph_run.glyphIndexes();
        const QList<QPointF> positions     = glyph_run.positions();
        const int glyph_count = std::min(glyph_indexes.size(), positions.size());
        if (glyph_count <= 0) {
            return;
        }

        const QRawFont raw_font = glyph_run.rawFont();
        const qreal physical_pixel_size =
            qsg_atlas_physical_pixel_size(raw_font, m_frame.device_pixel_ratio);
        QRawFont raster_font = raw_font;
        raster_font.setPixelSize(physical_pixel_size);
        const QString face_id = qsg_atlas_face_id_for_raw_font(raw_font);
        const qreal device_pixel_ratio = std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        for (int index = 0; index < glyph_count; ++index) {
            const quint32 glyph_index = glyph_indexes.at(index);
            const Glyph_atlas_cache_key key = qsg_atlas_cache_key(
                glyph_index,
                face_id,
                physical_pixel_size,
                0);
            Glyph_atlas_slot slot;
            if (const Glyph_atlas_slot* cached_slot = m_cache.find(key);
                cached_slot != nullptr)
            {
                slot = *cached_slot;
            }
            else {
                result.raster_thread = current_thread_id();
                const QImage alpha_map = raster_font.alphaMapForGlyph(
                    glyph_index,
                    QRawFont::PixelAntialiasing);
                const Glyph_coverage_tile tile =
                    qsg_atlas_coverage_tile_from_image(alpha_map);
                if (!tile.is_valid()) {
                    continue;
                }

                slot = m_cache.insert_or_get(key, tile);
                if (slot.is_valid()) {
                    ++result.rasterized_glyphs;
                }
            }
            if (!slot.is_valid()) {
                continue;
            }

            const QRectF bounds = raw_font.boundingRect(glyph_index);
            const QPointF glyph_position = layout_origin + positions.at(index) + bounds.topLeft();
            QRectF glyph_rect(
                glyph_position,
                QSizeF(
                    static_cast<qreal>(slot.rect.width())  / device_pixel_ratio,
                    static_cast<qreal>(slot.rect.height()) / device_pixel_ratio));
            QRectF uv_rect(
                static_cast<qreal>(slot.rect.x()) / static_cast<qreal>(m_cache.stats().page_size.width()),
                static_cast<qreal>(slot.rect.y()) / static_cast<qreal>(m_cache.stats().page_size.height()),
                static_cast<qreal>(slot.rect.width()) / static_cast<qreal>(m_cache.stats().page_size.width()),
                static_cast<qreal>(slot.rect.height()) / static_cast<qreal>(m_cache.stats().page_size.height()));
            if (run.clip_rect.isValid() &&
                !clip_glyph_instance(glyph_rect, uv_rect, run.clip_rect))
            {
                continue;
            }

            Stage1_glyph_instance instance;
            store_rect(instance.rect, glyph_rect);
            store_uv_rect(instance.uv_rect, uv_rect);
            store_color(instance.color, run.foreground, opacity);
            m_glyph_instances.push_back(instance);
        }
    }

    bool clip_glyph_instance(
        QRectF&       glyph_rect,
        QRectF&       uv_rect,
        const QRectF& clip_rect) const
    {
        const QRectF clipped = glyph_rect.intersected(clip_rect);
        if (clipped.width() <= 0.0 || clipped.height() <= 0.0) {
            return false;
        }
        const qreal x_ratio = uv_rect.width()  / glyph_rect.width();
        const qreal y_ratio = uv_rect.height() / glyph_rect.height();
        uv_rect.setLeft(uv_rect.left() + (clipped.left() - glyph_rect.left()) * x_ratio);
        uv_rect.setTop(uv_rect.top() + (clipped.top() - glyph_rect.top()) * y_ratio);
        uv_rect.setWidth(clipped.width() * x_ratio);
        uv_rect.setHeight(clipped.height() * y_ratio);
        glyph_rect = clipped;
        return true;
    }

    bool upload_coverage_texture(
        QRhi*               rhi,
        QRhiCommandBuffer*  command_buffer,
        bool*               out_r8_texture_created,
        bool*               out_r8_upload_recorded)
    {
        if (m_cache.stats().page_count <= 0) {
            return true;
        }

        const bool texture_ready = ensure_coverage_texture(rhi);
        if (out_r8_texture_created != nullptr) {
            *out_r8_texture_created =
                texture_ready && m_coverage_texture != nullptr &&
                m_coverage_texture->format() == QRhiTexture::R8;
        }
        if (!texture_ready) {
            return false;
        }

        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        const QByteArray& page_bytes = m_cache.page_bytes(0);
        QRhiTextureSubresourceUploadDescription subresource(page_bytes);
        subresource.setDataStride(static_cast<quint32>(m_cache.stats().page_size.width()));
        subresource.setSourceSize(m_cache.stats().page_size);
        QRhiTextureUploadDescription upload(
            QRhiTextureUploadEntry(0, 0, subresource));
        updates->uploadTexture(m_coverage_texture, upload);
        command_buffer->resourceUpdate(updates);

        if (out_r8_upload_recorded != nullptr) {
            *out_r8_upload_recorded = true;
        }
        return true;
    }

    bool ensure_dynamic_buffer(
        QRhi*                 rhi,
        QRhiBuffer*&          buffer,
        quint32&              current_size,
        quint32               required_size,
        QRhiBuffer::UsageFlag usage)
    {
        if (buffer != nullptr && current_size >= required_size) {
            return true;
        }

        delete_resource(buffer);
        buffer = rhi->newBuffer(QRhiBuffer::Dynamic, usage, required_size);
        if (buffer == nullptr || !buffer->create()) {
            current_size = 0U;
            return false;
        }

        current_size = required_size;
        return true;
    }

    bool update_stage2_buffers(QRhi* rhi, QRhiCommandBuffer* command_buffer)
    {
        const quint32 rect_buffer_size = static_cast<quint32>(
            std::max<std::size_t>(1U, m_rect_instances.size()) * sizeof(Stage1_instance));
        const quint32 glyph_buffer_size = static_cast<quint32>(
            std::max<std::size_t>(1U, m_glyph_instances.size()) * sizeof(Stage1_glyph_instance));
        if (!ensure_dynamic_buffer(
                rhi,
                m_rect_instance_buffer,
                m_rect_instance_buffer_size,
                rect_buffer_size,
                QRhiBuffer::VertexBuffer) ||
            !ensure_dynamic_buffer(
                rhi,
                m_glyph_instance_buffer,
                m_glyph_instance_buffer_size,
                glyph_buffer_size,
                QRhiBuffer::VertexBuffer))
        {
            return false;
        }

        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        if (m_static_vertex_upload_needed) {
            updates->uploadStaticBuffer(
                m_vertex_buffer,
                k_stage1_quad_vertices.data());
            m_static_vertex_upload_needed = false;
        }

        Stage1_uniform uniform;
        const QMatrix4x4 projection =
            projectionMatrix() != nullptr ? *projectionMatrix() : QMatrix4x4();
        const QMatrix4x4 model =
            matrix() != nullptr ? *matrix() : QMatrix4x4();
        const QMatrix4x4 mvp = projection * model;
        std::copy(
            mvp.constData(),
            mvp.constData() + 16,
            std::begin(uniform.matrix));
        updates->updateDynamicBuffer(
            m_uniform_buffer,
            0U,
            sizeof(uniform),
            &uniform);
        if (!m_rect_instances.empty()) {
            updates->updateDynamicBuffer(
                m_rect_instance_buffer,
                0U,
                static_cast<quint32>(m_rect_instances.size() * sizeof(Stage1_instance)),
                m_rect_instances.data());
        }
        if (!m_glyph_instances.empty()) {
            updates->updateDynamicBuffer(
                m_glyph_instance_buffer,
                0U,
                static_cast<quint32>(m_glyph_instances.size() * sizeof(Stage1_glyph_instance)),
                m_glyph_instances.data());
        }

        command_buffer->resourceUpdate(updates);
        return true;
    }

    void draw_rect_pass(
        QRhiCommandBuffer*        command_buffer,
        const Stage1_pass_range&  pass)
    {
        if (!pass.has_instances()) {
            return;
        }

        command_buffer->setGraphicsPipeline(m_rect_pipeline);
        command_buffer->setShaderResources(m_rect_shader_resources);
        const quint32 instance_offset = pass.first * sizeof(Stage1_instance);
        const QRhiCommandBuffer::VertexInput bindings[] = {
            {m_vertex_buffer,          0U},
            {m_rect_instance_buffer,   instance_offset},
        };
        command_buffer->setVertexInput(0, 2, bindings);
        command_buffer->draw(
            static_cast<quint32>(k_stage1_quad_vertices.size()),
            pass.count);
    }

    void draw_glyph_pass(
        QRhiCommandBuffer*        command_buffer,
        const Stage1_pass_range&  pass)
    {
        if (!pass.has_instances()) {
            return;
        }

        command_buffer->setGraphicsPipeline(m_glyph_pipeline);
        command_buffer->setShaderResources(m_glyph_shader_resources);
        const quint32 instance_offset = pass.first * sizeof(Stage1_glyph_instance);
        const QRhiCommandBuffer::VertexInput bindings[] = {
            {m_vertex_buffer,          0U},
            {m_glyph_instance_buffer,  instance_offset},
        };
        command_buffer->setVertexInput(0, 2, bindings);
        command_buffer->draw(
            static_cast<quint32>(k_stage1_quad_vertices.size()),
            pass.count);
    }

    quint32 total_instance_count() const
    {
        return
            static_cast<quint32>(m_rect_instances.size()) +
            static_cast<quint32>(m_glyph_instances.size());
    }

    Captured_atlas_frame                     m_frame;
    std::shared_ptr<Qsg_atlas_stage1_recorder>
                                             m_recorder;
    Glyph_atlas_cache                        m_cache;
    QRhi*                                    m_resource_rhi = nullptr;
    QVector<quint32>                         m_render_pass_serialized_format;
    int                                      m_render_target_samples = 0;
    bool                                     m_static_vertex_upload_needed = true;
    bool                                     m_resources_ready = false;
    bool                                     m_shader_packages_checked = false;
    QShader                                  m_vertex_shader;
    QShader                                  m_fragment_shader;
    QShader                                  m_glyph_vertex_shader;
    QShader                                  m_glyph_fragment_shader;
    std::vector<Stage1_instance>             m_rect_instances;
    std::vector<Stage1_glyph_instance>       m_glyph_instances;
    Stage1_pass_range                        m_background_pass;
    Stage1_pass_range                        m_selection_pass;
    Stage1_pass_range                        m_cursor_graphic_underlay_pass;
    Stage1_pass_range                        m_text_pass;
    Stage1_pass_range                        m_decoration_pass;
    Stage1_pass_range                        m_cursor_pass;
    Stage1_pass_range                        m_cursor_graphic_pass;
    Stage1_pass_range                        m_cursor_text_pass;
    QRhiBuffer*                              m_vertex_buffer = nullptr;
    QRhiBuffer*                              m_rect_instance_buffer = nullptr;
    QRhiBuffer*                              m_glyph_instance_buffer = nullptr;
    QRhiBuffer*                              m_uniform_buffer = nullptr;
    QRhiShaderResourceBindings*              m_rect_shader_resources = nullptr;
    QRhiShaderResourceBindings*              m_glyph_shader_resources = nullptr;
    QRhiGraphicsPipeline*                    m_rect_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_glyph_pipeline = nullptr;
    QRhiTexture*                             m_coverage_texture = nullptr;
    QRhiSampler*                             m_coverage_sampler = nullptr;
    quint32                                  m_rect_instance_buffer_size = 0U;
    quint32                                  m_glyph_instance_buffer_size = 0U;
};

}

bool operator==(
    const Glyph_atlas_cache_key& left,
    const Glyph_atlas_cache_key& right)
{
    return
        left.glyph_index         == right.glyph_index         &&
        left.fallback_face_id    == right.fallback_face_id    &&
        left.physical_pixel_size == right.physical_pixel_size &&
        left.subpixel_bucket     == right.subpixel_bucket;
}

bool operator<(
    const Glyph_atlas_cache_key& left,
    const Glyph_atlas_cache_key& right)
{
    return std::tie(
        left.glyph_index,
        left.fallback_face_id,
        left.physical_pixel_size,
        left.subpixel_bucket) <
        std::tie(
            right.glyph_index,
            right.fallback_face_id,
            right.physical_pixel_size,
            right.subpixel_bucket);
}

bool Glyph_coverage_tile::is_valid() const
{
    return
        size.width()   > 0             &&
        size.height()  > 0             &&
        bytes_per_line >= size.width() &&
        bytes.size()   >= bytes_per_line * size.height();
}

bool Glyph_atlas_slot::is_valid() const
{
    return page >= 0 && rect.width() > 0 && rect.height() > 0;
}

Glyph_atlas_packer::Glyph_atlas_packer(QSize page_size, int gutter, int max_pages)
:
    m_page_size(page_size),
    m_gutter(std::max(0, gutter)),
    m_max_pages(std::max(1, max_pages))
{}

std::optional<Glyph_atlas_slot> Glyph_atlas_packer::pack(QSize tile_size)
{
    if (tile_size.width()   <= 0 || tile_size.height()  <= 0 ||
        m_page_size.width() <= 0 || m_page_size.height() <= 0)
    {
        return std::nullopt;
    }

    const QSize padded_size(
        tile_size.width()  + m_gutter * 2,
        tile_size.height() + m_gutter * 2);
    if (padded_size.width() > m_page_size.width() ||
        padded_size.height() > m_page_size.height())
    {
        return std::nullopt;
    }

    for (std::size_t page = 0; page < m_pages.size(); ++page) {
        std::optional<Glyph_atlas_slot> slot = pack_in_page(
            static_cast<int>(page),
            padded_size,
            tile_size);
        if (slot.has_value()) {
            return slot;
        }
    }

    if (static_cast<int>(m_pages.size()) >= m_max_pages) {
        return std::nullopt;
    }

    m_pages.push_back({});
    return pack_in_page(
        static_cast<int>(m_pages.size() - 1U),
        padded_size,
        tile_size);
}

void Glyph_atlas_packer::reset()
{
    m_pages.clear();
}

int Glyph_atlas_packer::page_count() const
{
    return static_cast<int>(std::min<std::size_t>(
        m_pages.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

std::optional<Glyph_atlas_slot> Glyph_atlas_packer::pack_in_page(
    int   page_index,
    QSize padded_size,
    QSize tile_size)
{
    Page& page = m_pages[static_cast<std::size_t>(page_index)];
    for (Shelf& shelf : page.shelves) {
        if (padded_size.height() <= shelf.height &&
            shelf.x + padded_size.width() <= m_page_size.width())
        {
            const QRect rect(
                shelf.x + m_gutter,
                shelf.y + m_gutter,
                tile_size.width(),
                tile_size.height());
            shelf.x += padded_size.width();
            return Glyph_atlas_slot{page_index, rect};
        }
    }

    if (page.next_y + padded_size.height() > m_page_size.height()) {
        return std::nullopt;
    }

    Shelf shelf;
    shelf.y      = page.next_y;
    shelf.height = padded_size.height();
    shelf.x      = padded_size.width();
    page.shelves.push_back(shelf);
    page.next_y += padded_size.height();

    return Glyph_atlas_slot{
        page_index,
        QRect(
            m_gutter,
            shelf.y + m_gutter,
            tile_size.width(),
            tile_size.height()),
    };
}

Glyph_atlas_cache::Glyph_atlas_cache(QSize page_size)
:
    m_packer(page_size)
{
    m_stats.page_size = page_size;
}

void Glyph_atlas_cache::set_epoch(std::uint64_t epoch)
{
    if (m_stats.epoch == epoch) {
        return;
    }

    if (m_stats.epoch != 0U || !m_entries.empty()) {
        ++m_stats.invalidations;
    }
    m_stats.epoch = epoch;
    m_entries.clear();
    m_pages.clear();
    m_packer.reset();
    m_stats.page_count = 0;
}

void Glyph_atlas_cache::reset()
{
    const std::uint64_t epoch         = m_stats.epoch;
    const std::uint64_t invalidations = m_stats.invalidations;
    const QSize         page_size     = m_stats.page_size;
    m_entries.clear();
    m_pages.clear();
    m_packer.reset();
    m_stats               = {};
    m_stats.epoch         = epoch;
    m_stats.invalidations = invalidations;
    m_stats.page_size     = page_size;
}

const Glyph_atlas_slot* Glyph_atlas_cache::find(
    const Glyph_atlas_cache_key& key) const
{
    const auto found = m_entries.find(key);
    return found != m_entries.end()
        ? &found->second.slot
        : nullptr;
}

Glyph_atlas_slot Glyph_atlas_cache::insert_or_get(
    const Glyph_atlas_cache_key& key,
    const Glyph_coverage_tile&   tile)
{
    ++m_stats.lookups;
    const auto found = m_entries.find(key);
    if (found != m_entries.end()) {
        ++m_stats.hits;
        return found->second.slot;
    }

    const std::optional<Glyph_atlas_slot> slot = m_packer.pack(tile.size);
    if (!slot.has_value()) {
        return {};
    }

    ensure_page_count(m_packer.page_count());
    copy_tile_to_slot(slot->page, slot->rect, tile);
    m_entries.emplace(key, Entry{*slot});
    ++m_stats.inserts;
    m_stats.page_count = m_packer.page_count();
    return *slot;
}

Glyph_atlas_cache_stats Glyph_atlas_cache::stats() const
{
    Glyph_atlas_cache_stats stats = m_stats;
    stats.page_count = m_packer.page_count();
    return stats;
}

const QByteArray& Glyph_atlas_cache::page_bytes(int page) const
{
    return m_pages[static_cast<std::size_t>(page)];
}

void Glyph_atlas_cache::ensure_page_count(int page_count)
{
    while (static_cast<int>(m_pages.size()) < page_count) {
        m_pages.push_back(
            QByteArray(m_stats.page_size.width() * m_stats.page_size.height(), '\0'));
    }
}

void Glyph_atlas_cache::copy_tile_to_slot(
    int                        page,
    const QRect&               rect,
    const Glyph_coverage_tile& tile)
{
    QByteArray& page_bytes = m_pages[static_cast<std::size_t>(page)];
    const int page_stride  = m_stats.page_size.width();
    for (int y = 0; y < tile.size.height(); ++y) {
        const char* const source = tile.bytes.constData() + y * tile.bytes_per_line;
        char* const destination =
            page_bytes.data() + (rect.y() + y) * page_stride + rect.x();
        std::memcpy(destination, source, static_cast<std::size_t>(tile.size.width()));
    }
}

void Qsg_atlas_stage1_recorder::reset()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_report = {};
}

void Qsg_atlas_stage1_recorder::record_capture(const Captured_atlas_frame& frame)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t frame_snapshot_sequence = snapshot_sequence(frame);
    const QColor frame_probe_color = qsg_atlas_stage1_probe_color(frame);
    const bool frame_light_options = captured_options_are_light(frame);
    if (m_report.capture_count == 0U) {
        m_report.first_captured_snapshot_sequence = frame_snapshot_sequence;
        m_report.first_captured_font_epoch        = frame.font_epoch;
        m_report.first_captured_probe_color       = frame_probe_color;
        m_report.first_captured_light_options     = frame_light_options;
    }
    ++m_report.capture_count;
    m_report.capture_sequence           = frame.capture_sequence;
    m_report.captured_snapshot_sequence = frame_snapshot_sequence;
    m_report.captured_font_epoch        = frame.font_epoch;
    m_report.captured_probe_color       = frame_probe_color;
    m_report.captured_light_options     = frame_light_options;
}

void Qsg_atlas_stage1_recorder::record_prepare(
    const Captured_atlas_frame&    frame,
    bool                           command_buffer_non_null,
    bool                           render_target_non_null,
    bool                           rhi_non_null,
    bool                           r8_texture_created,
    bool                           r8_upload_recorded,
    bool                           raw_font_rasterized,
    bool                           raw_font_rasterized_in_prepare,
    int                            rasterized_glyphs,
    std::uint64_t                  prepare_thread_id,
    std::uint64_t                  raw_font_raster_thread_id,
    const Glyph_atlas_cache_stats& cache)
{
    (void)frame;

    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_report.prepare_count;
    m_report.command_buffer_non_null        = command_buffer_non_null;
    m_report.render_target_non_null         = render_target_non_null;
    m_report.rhi_non_null                   = rhi_non_null;
    m_report.r8_texture_created             = r8_texture_created;
    m_report.r8_upload_recorded             = r8_upload_recorded;
    m_report.raw_font_rasterized            = raw_font_rasterized;
    m_report.raw_font_rasterized_in_prepare = raw_font_rasterized_in_prepare;
    m_report.rasterized_glyphs              = rasterized_glyphs;
    m_report.prepare_thread_id              = prepare_thread_id;
    m_report.raw_font_raster_thread_id      = raw_font_raster_thread_id;
    m_report.atlas_page_count               = cache.page_count;
    m_report.cache                          = cache;
}

void Qsg_atlas_stage1_recorder::record_render(
    const Captured_atlas_frame& frame,
    QRect                       viewport_rect,
    bool                        drew)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_report.render_count;
    m_report.viewport_rect = viewport_rect;
    m_report.drew          = drew;
    if (m_report.first_render_snapshot_sequence == 0U) {
        m_report.first_render_snapshot_sequence = snapshot_sequence(frame);
        m_report.first_render_font_epoch        = frame.font_epoch;
        m_report.first_render_probe_color       = qsg_atlas_stage1_probe_color(frame);
        m_report.first_render_light_options     = captured_options_are_light(frame);
    }
}

Qsg_atlas_stage1_frame_report Qsg_atlas_stage1_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_report;
}

Glyph_coverage_tile qsg_atlas_coverage_tile_from_image(const QImage& image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }

    if (image.format() != QImage::Format_Indexed8   &&
        image.format() != QImage::Format_Grayscale8 &&
        image.format() != QImage::Format_Alpha8)
    {
        return {};
    }

    Glyph_coverage_tile tile;
    tile.size           = image.size();
    tile.bytes_per_line = image.width();
    tile.bytes.resize(tile.bytes_per_line * image.height());
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(
            tile.bytes.data() + y * tile.bytes_per_line,
            image.constScanLine(y),
            static_cast<std::size_t>(image.width()));
    }
    return tile;
}

QString qsg_atlas_face_id_for_raw_font(const QRawFont& raw_font)
{
    const QByteArray name_table = raw_font.fontTable("name");
    return QStringLiteral("%1\n%2\n%3")
        .arg(raw_font.familyName())
        .arg(raw_font.styleName())
        .arg(QString::number(qHash(name_table), 16));
}

qreal qsg_atlas_physical_pixel_size(const QFont& font, qreal device_pixel_ratio)
{
    const qreal logical_pixel_size = font.pixelSize() > 0
        ? static_cast<qreal>(font.pixelSize())
        : font.pointSizeF();
    return logical_pixel_size * device_pixel_ratio;
}

qreal qsg_atlas_physical_pixel_size(
    const QRawFont& raw_font,
    qreal           device_pixel_ratio)
{
    return raw_font.pixelSize() * device_pixel_ratio;
}

Glyph_atlas_cache_key qsg_atlas_cache_key(
    quint32 glyph_index,
    QString fallback_face_id,
    qreal   physical_pixel_size,
    int     subpixel_bucket)
{
    return {
        glyph_index,
        std::move(fallback_face_id),
        physical_pixel_size,
        subpixel_bucket,
    };
}

Captured_atlas_frame capture_qsg_atlas_stage1_frame(
    std::shared_ptr<const Terminal_render_snapshot>
                                  snapshot,
    Ime_preedit_state             ime_preedit,
    Terminal_render_options       options,
    terminal_cell_metrics_t       cell_metrics,
    QSizeF                        logical_size,
    QFont                         font,
    qreal                         device_pixel_ratio,
    std::uint64_t                 font_epoch,
    std::uint64_t                 capture_sequence,
    bool                          cursor_blink_visible)
{
    Captured_atlas_frame frame;
    frame.snapshot             = std::move(snapshot);
    frame.ime_preedit          = std::move(ime_preedit);
    frame.options              = std::move(options);
    frame.cell_metrics         = cell_metrics;
    frame.logical_size         = logical_size;
    frame.font                 = std::move(font);
    frame.device_pixel_ratio   = device_pixel_ratio;
    frame.font_epoch           = font_epoch;
    frame.capture_sequence     = capture_sequence;
    frame.cursor_blink_visible = cursor_blink_visible;
    return frame;
}

QColor qsg_atlas_stage1_probe_color(const Captured_atlas_frame& frame)
{
    const int sequence_component = 32 + static_cast<int>(snapshot_sequence(frame) % 160U);
    const int options_component  = captured_options_are_light(frame) ? 214 : 72;
    const int epoch_component    = 32 + static_cast<int>(frame.font_epoch % 160U);
    return QColor(sequence_component, options_component, epoch_component, 255);
}

QSGNode* update_qsg_atlas_stage1_node(
    QSGNode*                                      old_node,
    Captured_atlas_frame                         frame,
    const std::shared_ptr<Qsg_atlas_stage1_recorder>&
                                                  recorder)
{
    Stage1_atlas_render_node* node =
        dynamic_cast<Stage1_atlas_render_node*>(old_node);
    if (node == nullptr) {
        delete old_node;
        node = new Stage1_atlas_render_node(recorder);
    }

    node->set_frame(std::move(frame), recorder);
    return node;
}

}
