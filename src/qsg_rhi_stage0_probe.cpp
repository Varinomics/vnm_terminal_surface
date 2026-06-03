#include "vnm_terminal/internal/qsg_rhi_stage0_probe.h"

#include <QFile>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr const char* k_stage0_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/stage0_quad.vert.qsb";
constexpr const char* k_stage0_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/stage0_quad.frag.qsb";
constexpr int k_stage0_stencil_mask = 0xff;

struct stage0_vertex_t
{
    float x = 0.0f;
    float y = 0.0f;
};

struct stage0_instance_t
{
    float rect[4]  = {};
    float color[4] = {};
};

struct stage0_uniform_t
{
    float matrix[16] = {};
};

const std::array<stage0_vertex_t, 6> k_stage0_quad_vertices = {{
    {0.0f, 0.0f},
    {1.0f, 0.0f},
    {0.0f, 1.0f},
    {0.0f, 1.0f},
    {1.0f, 0.0f},
    {1.0f, 1.0f},
}};

QShader load_shader(const char* path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    return QShader::fromSerialized(file.readAll());
}

template <typename T>
void delete_resource(T*& resource)
{
    delete resource;
    resource = nullptr;
}

QRhiGraphicsPipeline::TargetBlend stage0_blend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

QRhiGraphicsPipeline::StencilOpState stage0_stencil_state()
{
    QRhiGraphicsPipeline::StencilOpState state;
    state.failOp      = QRhiGraphicsPipeline::Keep;
    state.depthFailOp = QRhiGraphicsPipeline::Keep;
    state.passOp      = QRhiGraphicsPipeline::Keep;
    state.compareOp   = QRhiGraphicsPipeline::Equal;
    return state;
}

qsg_rhi_stage0_probe_contract_t stage0_pipeline_contract(bool stencil_enabled)
{
    qsg_rhi_stage0_probe_contract_t contract;
    contract.stencil_enabled    = stencil_enabled;
    contract.depth_test_enabled  = false;
    contract.depth_write_enabled = false;
    if (stencil_enabled) {
        const QRhiGraphicsPipeline::StencilOpState stencil_state =
            stage0_stencil_state();
        contract.stencil_compare_equal =
            stencil_state.compareOp == QRhiGraphicsPipeline::Equal;
        contract.stencil_ops_keep =
            stencil_state.failOp      == QRhiGraphicsPipeline::Keep &&
            stencil_state.depthFailOp == QRhiGraphicsPipeline::Keep &&
            stencil_state.passOp      == QRhiGraphicsPipeline::Keep;
        contract.stencil_read_mask  = k_stage0_stencil_mask;
        contract.stencil_write_mask = k_stage0_stencil_mask;
    }
    return contract;
}

QRhiVertexInputLayout stage0_vertex_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(stage0_vertex_t)),
        QRhiVertexInputBinding(
            sizeof(stage0_instance_t),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(stage0_vertex_t, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(stage0_instance_t, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(stage0_instance_t, color)),
    });
    return layout;
}

class Stage0_probe_render_node final : public QSGRenderNode
{
public:
    explicit Stage0_probe_render_node(
        std::shared_ptr<Qsg_rhi_stage0_probe_recorder> recorder)
    :
        m_recorder(std::move(recorder))
    {}

    ~Stage0_probe_render_node() override
    {
        releaseResources();
    }

    void set_frame(
        QRhi*                                          rhi,
        QSizeF                                         logical_size,
        std::shared_ptr<Qsg_rhi_stage0_probe_recorder> recorder)
    {
        if (m_rhi != rhi) {
            releaseResources();
        }

        m_rhi          = rhi;
        m_logical_size = logical_size;
        m_recorder     = std::move(recorder);
    }

    StateFlags changedStates() const override
    {
        return qsg_rhi_stage0_probe_changed_states();
    }

    RenderingFlags flags() const override
    {
        return NoExternalRendering;
    }

    QRectF rect() const override
    {
        return QRectF(QPointF(0.0, 0.0), m_logical_size);
    }

    void prepare() override
    {
        QRhiCommandBuffer* const command_buffer = commandBuffer();
        QRhiRenderTarget* const  target         = renderTarget();
        QRhi* const              rhi            = active_rhi(target);
        const bool command_buffer_non_null      = command_buffer != nullptr;
        const bool render_target_non_null       = target != nullptr;
        const bool projection_matrix_non_null   = projectionMatrix() != nullptr;
        const bool matrix_non_null              = matrix() != nullptr;
        const bool used_projection_times_matrix =
            projection_matrix_non_null && matrix_non_null;
        const qreal opacity = inheritedOpacity();
        QRectF model_mapped_rect;
        QVector<quint32> render_pass_serialized_format;

        bool shader_packages_valid = false;
        bool resources_ready       = false;
        QSize target_pixel_size;
        int target_sample_count = 0;
        if (target != nullptr) {
            target_pixel_size   = target->pixelSize();
            target_sample_count = target->sampleCount();
        }

        if (rhi != nullptr && command_buffer != nullptr && target != nullptr) {
            shader_packages_valid = ensure_shaders();
            resources_ready = shader_packages_valid &&
                ensure_resources(rhi, target, &render_pass_serialized_format) &&
                update_buffers(rhi, command_buffer, opacity, &model_mapped_rect);
        }

        if (m_recorder != nullptr) {
            m_recorder->record_prepare(
                command_buffer_non_null,
                render_target_non_null,
                shader_packages_valid,
                resources_ready,
                projection_matrix_non_null,
                matrix_non_null,
                used_projection_times_matrix,
                model_mapped_rect,
                render_pass_serialized_format,
                m_resource_rebuild_count,
                target_pixel_size,
                target_sample_count,
                opacity,
                opacity);
        }
    }

    void render(const RenderState* state) override
    {
        QRhiCommandBuffer* const command_buffer = commandBuffer();
        QRhiRenderTarget* const  target         = renderTarget();
        const bool command_buffer_non_null      = command_buffer != nullptr;
        const bool render_target_non_null       = target != nullptr;

        qsg_rhi_stage0_probe_contract_t contract;
        bool stencil_ref_from_render_state = false;
        if (state != nullptr) {
            contract = qsg_rhi_stage0_probe_contract_for_state(*state);
            stencil_ref_from_render_state =
                contract.stencil_ref == state->stencilValue();
        }

        QRect viewport_rect;
        bool drew = false;
        if (command_buffer != nullptr && target != nullptr && m_resources_ready) {
            const QSize target_size = target->pixelSize();
            viewport_rect = QRect(QPoint(0, 0), target_size);
            const qsg_rhi_stage0_probe_contract_t pipeline_contract =
                contract.stencil_enabled
                    ? m_stencil_pipeline_contract
                    : m_pipeline_contract;
            contract.depth_test_enabled    = pipeline_contract.depth_test_enabled;
            contract.depth_write_enabled   = pipeline_contract.depth_write_enabled;
            contract.stencil_compare_equal =
                pipeline_contract.stencil_compare_equal;
            contract.stencil_ops_keep      = pipeline_contract.stencil_ops_keep;
            contract.stencil_read_mask     = pipeline_contract.stencil_read_mask;
            contract.stencil_write_mask    = pipeline_contract.stencil_write_mask;

            QRhiGraphicsPipeline* const pipeline = contract.stencil_enabled
                ? m_stencil_pipeline
                : m_pipeline;
            contract.stencil_pipeline_selected =
                contract.stencil_enabled && pipeline == m_stencil_pipeline;
            contract.stencil_ref_from_render_state = stencil_ref_from_render_state;
            command_buffer->setGraphicsPipeline(pipeline);
            command_buffer->setViewport(QRhiViewport(
                0.0f,
                0.0f,
                static_cast<float>(target_size.width()),
                static_cast<float>(target_size.height())));

            const QRect scissor_rect = contract.scissor_enabled
                ? contract.scissor_rect
                : viewport_rect;
            command_buffer->setScissor(QRhiScissor(
                scissor_rect.x(),
                scissor_rect.y(),
                scissor_rect.width(),
                scissor_rect.height()));

            if (contract.stencil_enabled) {
                command_buffer->setStencilRef(static_cast<quint32>(contract.stencil_ref));
            }
            command_buffer->setShaderResources(m_shader_resources);

            const QRhiCommandBuffer::VertexInput bindings[] = {
                {m_vertex_buffer,   0U},
                {m_instance_buffer, 0U},
            };
            command_buffer->setVertexInput(0, 2, bindings);
            command_buffer->draw(
                static_cast<quint32>(k_stage0_quad_vertices.size()),
                1U);
            drew = true;
        }

        if (m_recorder != nullptr) {
            m_recorder->record_render(
                command_buffer_non_null,
                render_target_non_null,
                viewport_rect,
                drew,
                contract);
        }
    }

    void releaseResources() override
    {
        delete_resource(m_pipeline);
        delete_resource(m_stencil_pipeline);
        delete_resource(m_shader_resources);
        delete_resource(m_uniform_buffer);
        delete_resource(m_instance_buffer);
        delete_resource(m_vertex_buffer);
        m_rhi                    = nullptr;
        m_resource_rhi           = nullptr;
        m_render_pass_descriptor = nullptr;
        m_render_pass_serialized_format.clear();
        m_render_target_samples  = 0;
        m_static_vertex_upload   = true;
        m_resources_ready        = false;
    }

private:
    QRhi* active_rhi(QRhiRenderTarget* target) const
    {
        return target != nullptr
                ? target->rhi()
                : m_rhi;
    }

    bool ensure_shaders()
    {
        if (!m_shader_packages_checked) {
            m_vertex_shader           = load_shader(k_stage0_vertex_shader_path);
            m_fragment_shader         = load_shader(k_stage0_fragment_shader_path);
            m_shader_packages_checked = true;
        }

        return m_vertex_shader.isValid() && m_fragment_shader.isValid();
    }

    bool ensure_resources(
        QRhi*             rhi,
        QRhiRenderTarget* target,
        QVector<quint32>* render_pass_serialized_format)
    {
        QRhiRenderPassDescriptor* const render_pass_descriptor =
            target->renderPassDescriptor();
        const QVector<quint32> current_render_pass_serialized_format =
            render_pass_descriptor != nullptr
                ? render_pass_descriptor->serializedFormat()
                : QVector<quint32>();
        if (render_pass_serialized_format != nullptr) {
            *render_pass_serialized_format =
                current_render_pass_serialized_format;
        }

        const int sample_count = target->sampleCount();
        if (m_resource_rhi           != rhi                    ||
            m_render_pass_serialized_format !=
                current_render_pass_serialized_format          ||
            m_render_target_samples  != sample_count)
        {
            releaseResources();
            m_rhi                    = rhi;
            m_resource_rhi           = rhi;
            m_render_pass_descriptor = render_pass_descriptor;
            m_render_pass_serialized_format =
                current_render_pass_serialized_format;
            m_render_target_samples  = sample_count;
        }
        else {
            m_render_pass_descriptor = render_pass_descriptor;
        }

        if (m_vertex_buffer != nullptr) {
            return true;
        }

        m_vertex_buffer = rhi->newBuffer(
            QRhiBuffer::Immutable,
            QRhiBuffer::VertexBuffer,
            static_cast<quint32>(
                sizeof(stage0_vertex_t) * k_stage0_quad_vertices.size()));
        m_instance_buffer = rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::VertexBuffer,
            sizeof(stage0_instance_t));
        m_uniform_buffer = rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            static_cast<quint32>(rhi->ubufAligned(sizeof(stage0_uniform_t))));
        if (!m_vertex_buffer->create()   ||
            !m_instance_buffer->create() ||
            !m_uniform_buffer->create())
        {
            releaseResources();
            return false;
        }

        m_shader_resources = rhi->newShaderResourceBindings();
        m_shader_resources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_uniform_buffer),
        });
        if (!m_shader_resources->create()) {
            releaseResources();
            return false;
        }

        m_pipeline         = create_pipeline(rhi, false, &m_pipeline_contract);
        m_stencil_pipeline = create_pipeline(
            rhi,
            true,
            &m_stencil_pipeline_contract);
        if (m_pipeline == nullptr || m_stencil_pipeline == nullptr) {
            releaseResources();
            return false;
        }

        m_static_vertex_upload = true;
        ++m_resource_rebuild_count;
        return true;
    }

    QRhiGraphicsPipeline* create_pipeline(
        QRhi*                              rhi,
        bool                               stencil_enabled,
        qsg_rhi_stage0_probe_contract_t*   pipeline_contract)
    {
        if (pipeline_contract != nullptr) {
            *pipeline_contract = stage0_pipeline_contract(stencil_enabled);
        }

        QRhiGraphicsPipeline* pipeline = rhi->newGraphicsPipeline();
        QRhiGraphicsPipeline::Flags flags = QRhiGraphicsPipeline::UsesScissor;
        if (stencil_enabled) {
            flags |= QRhiGraphicsPipeline::UsesStencilRef;
        }
        pipeline->setFlags(flags);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({stage0_blend()});
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        if (stencil_enabled) {
            const QRhiGraphicsPipeline::StencilOpState stencil_state =
                stage0_stencil_state();
            pipeline->setStencilTest(true);
            pipeline->setStencilFront(stencil_state);
            pipeline->setStencilBack(stencil_state);
            pipeline->setStencilReadMask(
                pipeline_contract != nullptr
                    ? pipeline_contract->stencil_read_mask
                    : k_stage0_stencil_mask);
            pipeline->setStencilWriteMask(
                pipeline_contract != nullptr
                    ? pipeline_contract->stencil_write_mask
                    : k_stage0_stencil_mask);
        }
        pipeline->setSampleCount(m_render_target_samples);
        pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_fragment_shader),
        });
        pipeline->setVertexInputLayout(stage0_vertex_input_layout());
        pipeline->setShaderResourceBindings(m_shader_resources);
        pipeline->setRenderPassDescriptor(m_render_pass_descriptor);
        if (!pipeline->create()) {
            delete pipeline;
            return nullptr;
        }

        return pipeline;
    }

    void fill_dynamic_payload(
        qreal              opacity,
        stage0_uniform_t&  uniform,
        stage0_instance_t& instance,
        QRectF*            model_mapped_rect = nullptr) const
    {
        const QMatrix4x4 projection =
            projectionMatrix() != nullptr ? *projectionMatrix() : QMatrix4x4();
        const QMatrix4x4 model =
            matrix() != nullptr ? *matrix() : QMatrix4x4();
        const QMatrix4x4 mvp = projection * model;

        std::copy(
            mvp.constData(),
            mvp.constData() + 16,
            std::begin(uniform.matrix));

        if (model_mapped_rect != nullptr) {
            *model_mapped_rect = model.mapRect(
                QRectF(QPointF(0.0, 0.0), m_logical_size));
        }

        const qreal bounded_opacity = std::clamp(opacity, 0.0, 1.0);
        const QColor color(34, 204, 96);
        instance.rect[0]  = 0.0f;
        instance.rect[1]  = 0.0f;
        instance.rect[2]  = static_cast<float>(m_logical_size.width());
        instance.rect[3]  = static_cast<float>(m_logical_size.height());
        instance.color[0] = static_cast<float>(color.redF());
        instance.color[1] = static_cast<float>(color.greenF());
        instance.color[2] = static_cast<float>(color.blueF());
        instance.color[3] = static_cast<float>(color.alphaF() * bounded_opacity);
    }

    bool update_buffers(
        QRhi*               rhi,
        QRhiCommandBuffer*  command_buffer,
        qreal               opacity,
        QRectF*             model_mapped_rect)
    {
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        if (m_static_vertex_upload) {
            updates->uploadStaticBuffer(
                m_vertex_buffer,
                k_stage0_quad_vertices.data());
            m_static_vertex_upload = false;
        }

        stage0_uniform_t uniform;
        stage0_instance_t instance;
        fill_dynamic_payload(opacity, uniform, instance, model_mapped_rect);
        updates->updateDynamicBuffer(
            m_uniform_buffer,
            0U,
            sizeof(uniform),
            &uniform);

        updates->updateDynamicBuffer(
            m_instance_buffer,
            0U,
            sizeof(instance),
            &instance);

        command_buffer->resourceUpdate(updates);
        m_resources_ready = true;
        return true;
    }

    std::shared_ptr<Qsg_rhi_stage0_probe_recorder>
                             m_recorder;
    QSizeF                   m_logical_size;
    QRhi*                    m_rhi                    = nullptr;
    QRhi*                    m_resource_rhi           = nullptr;
    QRhiRenderPassDescriptor*
                             m_render_pass_descriptor = nullptr;
    QVector<quint32>         m_render_pass_serialized_format;
    int                      m_render_target_samples  = 0;
    bool                     m_static_vertex_upload   = true;
    bool                     m_resources_ready        = false;
    bool                     m_shader_packages_checked = false;
    QShader                  m_vertex_shader;
    QShader                  m_fragment_shader;
    QRhiBuffer*              m_vertex_buffer          = nullptr;
    QRhiBuffer*              m_instance_buffer        = nullptr;
    QRhiBuffer*              m_uniform_buffer         = nullptr;
    QRhiShaderResourceBindings*
                             m_shader_resources       = nullptr;
    QRhiGraphicsPipeline*    m_pipeline               = nullptr;
    QRhiGraphicsPipeline*    m_stencil_pipeline       = nullptr;
    qsg_rhi_stage0_probe_contract_t
                             m_pipeline_contract;
    qsg_rhi_stage0_probe_contract_t
                             m_stencil_pipeline_contract;
    std::uint64_t            m_resource_rebuild_count = 0U;
};

}

void Qsg_rhi_stage0_probe_recorder::reset()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_frame = {};
}

void Qsg_rhi_stage0_probe_recorder::record_window_rhi(
    bool       rhi_non_null,
    QString    backend_name)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_frame.window_rhi_probe_count;
    m_frame.window_rhi_non_null = rhi_non_null;
    m_frame.rhi_backend_name    = std::move(backend_name);
}

void Qsg_rhi_stage0_probe_recorder::record_prepare(
    bool       command_buffer_non_null,
    bool       render_target_non_null,
    bool       shader_packages_valid,
    bool       resources_ready,
    bool       projection_matrix_non_null,
    bool       matrix_non_null,
    bool       used_projection_times_matrix,
    QRectF     model_mapped_rect,
    QVector<quint32>
               render_pass_serialized_format,
    std::uint64_t
               resource_rebuild_count,
    QSize      render_target_pixel_size,
    int        render_target_sample_count,
    qreal      inherited_opacity,
    qreal      pass_opacity)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_frame.prepare_count;
    m_frame.command_buffer_non_null      = command_buffer_non_null;
    m_frame.render_target_non_null       = render_target_non_null;
    m_frame.shader_packages_valid        = shader_packages_valid;
    m_frame.resources_ready              = resources_ready;
    m_frame.projection_matrix_non_null   = projection_matrix_non_null;
    m_frame.matrix_non_null              = matrix_non_null;
    m_frame.used_projection_times_matrix = used_projection_times_matrix;
    m_frame.model_mapped_rect            = model_mapped_rect;
    m_frame.render_pass_serialized_format =
        std::move(render_pass_serialized_format);
    m_frame.resource_rebuild_count       = resource_rebuild_count;
    m_frame.render_target_pixel_size     = render_target_pixel_size;
    m_frame.render_target_sample_count   = render_target_sample_count;
    m_frame.inherited_opacity            = inherited_opacity;
    m_frame.pass_count                   = 1;
    m_frame.pass_opacity                 = pass_opacity;
    m_frame.changed_states               =
        static_cast<int>(qsg_rhi_stage0_probe_changed_states());
    m_frame.rendering_flags              =
        static_cast<int>(QSGRenderNode::NoExternalRendering);
}

void Qsg_rhi_stage0_probe_recorder::record_render(
    bool                                     command_buffer_non_null,
    bool                                     render_target_non_null,
    QRect                                    viewport_rect,
    bool                                     drew,
    const qsg_rhi_stage0_probe_contract_t&   contract)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_frame.render_count;
    m_frame.command_buffer_non_null = command_buffer_non_null;
    m_frame.render_target_non_null  = render_target_non_null;
    m_frame.viewport_rect           = viewport_rect;
    m_frame.drew                    = drew;
    m_frame.contract                = contract;
}

qsg_rhi_stage0_probe_frame_t Qsg_rhi_stage0_probe_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_frame;
}

qsg_rhi_stage0_probe_contract_t qsg_rhi_stage0_probe_contract_for_state(
    const QSGRenderNode::RenderState& state)
{
    qsg_rhi_stage0_probe_contract_t contract;
    const qsg_rhi_stage0_probe_contract_t pipeline_contract =
        stage0_pipeline_contract(state.stencilEnabled());
    contract.scissor_enabled       = state.scissorEnabled();
    contract.scissor_rect          = state.scissorRect();
    contract.stencil_enabled       = state.stencilEnabled();
    contract.stencil_ref           = state.stencilValue();
    contract.stencil_ref_from_render_state = true;
    contract.stencil_compare_equal =
        pipeline_contract.stencil_compare_equal;
    contract.stencil_ops_keep      = pipeline_contract.stencil_ops_keep;
    contract.stencil_read_mask     = pipeline_contract.stencil_read_mask;
    contract.stencil_write_mask    = pipeline_contract.stencil_write_mask;
    contract.depth_test_enabled    = pipeline_contract.depth_test_enabled;
    contract.depth_write_enabled   = pipeline_contract.depth_write_enabled;
    return contract;
}

QSGRenderNode::StateFlags qsg_rhi_stage0_probe_changed_states()
{
    return QSGRenderNode::ViewportState | QSGRenderNode::ScissorState;
}

void qsg_rhi_stage0_probe_record_window_rhi(
    const std::shared_ptr<Qsg_rhi_stage0_probe_recorder>& recorder,
    QRhi*                                                 rhi)
{
    if (recorder == nullptr) {
        return;
    }

    recorder->record_window_rhi(
        rhi != nullptr,
        rhi != nullptr ? QString::fromLatin1(rhi->backendName()) : QString());
}

QSGNode* update_qsg_rhi_stage0_probe_node(
    QSGNode*                                              old_node,
    QQuickWindow*                                         window,
    QSizeF                                                logical_size,
    const std::shared_ptr<Qsg_rhi_stage0_probe_recorder>& recorder)
{
    Stage0_probe_render_node* node = dynamic_cast<Stage0_probe_render_node*>(old_node);
    if (node == nullptr) {
        delete old_node;
        node = new Stage0_probe_render_node(recorder);
    }

    node->set_frame(
        window != nullptr ? window->rhi() : nullptr,
        logical_size,
        recorder);
    return node;
}

}
