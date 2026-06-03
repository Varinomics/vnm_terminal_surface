#pragma once

#include <QColor>
#include <QRect>
#include <QRectF>
#include <QSGRenderNode>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <cstdint>
#include <memory>
#include <mutex>

class QQuickWindow;
class QRhi;
class QSGNode;

namespace vnm_terminal::internal {

struct qsg_rhi_stage0_probe_contract_t
{
    bool  scissor_enabled       = false;
    QRect scissor_rect;
    bool  stencil_enabled       = false;
    bool  stencil_pipeline_selected = false;
    int   stencil_ref           = 0;
    bool  stencil_ref_from_render_state = false;
    bool  stencil_compare_equal = false;
    bool  stencil_ops_keep      = false;
    int   stencil_read_mask     = 0;
    int   stencil_write_mask    = 0;
    bool  depth_test_enabled    = false;
    bool  depth_write_enabled   = false;
};

struct qsg_rhi_stage0_probe_frame_t
{
    std::uint64_t window_rhi_probe_count       = 0U;
    std::uint64_t prepare_count                = 0U;
    std::uint64_t render_count                 = 0U;
    QString       rhi_backend_name;
    bool          window_rhi_non_null          = false;
    bool          command_buffer_non_null      = false;
    bool          render_target_non_null       = false;
    bool          shader_packages_valid        = false;
    bool          resources_ready              = false;
    bool          projection_matrix_non_null   = false;
    bool          matrix_non_null              = false;
    bool          used_projection_times_matrix = false;
    QRectF        model_mapped_rect;
    QVector<quint32>
                  render_pass_serialized_format;
    std::uint64_t resource_rebuild_count       = 0U;
    QSize         render_target_pixel_size;
    int           render_target_sample_count   = 0;
    QRect         viewport_rect;
    qreal         inherited_opacity            = 1.0;
    int           pass_count                   = 0;
    qreal         pass_opacity                 = 1.0;
    bool          drew                         = false;
    int           changed_states               = 0;
    int           rendering_flags              = 0;
    qsg_rhi_stage0_probe_contract_t
                  contract;
};

class Qsg_rhi_stage0_probe_recorder final
{
public:
    void reset();

    void record_window_rhi(
        bool       rhi_non_null,
        QString    backend_name);

    void record_prepare(
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
        qreal      pass_opacity);

    void record_render(
        bool                                     command_buffer_non_null,
        bool                                     render_target_non_null,
        QRect                                    viewport_rect,
        bool                                     drew,
        const qsg_rhi_stage0_probe_contract_t&   contract);

    qsg_rhi_stage0_probe_frame_t snapshot() const;

private:
    mutable std::mutex           m_mutex;
    qsg_rhi_stage0_probe_frame_t m_frame;
};

qsg_rhi_stage0_probe_contract_t qsg_rhi_stage0_probe_contract_for_state(
    const QSGRenderNode::RenderState& state);

QSGRenderNode::StateFlags qsg_rhi_stage0_probe_changed_states();

void qsg_rhi_stage0_probe_record_window_rhi(
    const std::shared_ptr<Qsg_rhi_stage0_probe_recorder>& recorder,
    QRhi*                                                 rhi);

QSGNode* update_qsg_rhi_stage0_probe_node(
    QSGNode*                                              old_node,
    QQuickWindow*                                         window,
    QSizeF                                                logical_size,
    const std::shared_ptr<Qsg_rhi_stage0_probe_recorder>& recorder);

}
