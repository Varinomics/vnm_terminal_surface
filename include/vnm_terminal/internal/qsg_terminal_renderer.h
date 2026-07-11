#pragma once

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/terminal_hyperlink.h"
#include <QFont>
#include <QString>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if !defined(VNM_TERMINAL_PROFILING_ENABLED)
#define VNM_TERMINAL_PROFILING_ENABLED 0
#endif

namespace vnm_terminal::internal {

struct Terminal_render_text_run;

#if VNM_TERMINAL_PROFILING_ENABLED
struct terminal_text_layout_slow_diagnostic_t
{
    std::uint64_t  duration_ns                = 0U;
    std::uint64_t  text_hash                  = 0U;
    int            text_utf16_units           = 0;
    int            text_codepoints            = 0;
    int            row                        = 0;
    int            logical_row                = 0;
    int            column                     = 0;
    int            style_id                   = 0;
    Terminal_hyperlink_id hyperlink_id = k_no_terminal_hyperlink_id;
    qreal          rect_width                 = 0.0;
    qreal          rect_height                = 0.0;
    qreal          font_point_size            = 0.0;
    int            font_pixel_size            = 0;
    int            font_weight                = 0;
    bool           ascii_only                 = false;
    bool           printable_ascii_only       = false;
    bool           has_control_codepoint      = false;
    bool           clipped                    = false;
    bool           force_blended_order        = false;
    bool           ascii_layout_font          = false;
    bool           line_has_text              = false;
    bool           text_preview_truncated     = false;
    bool           font_italic                = false;
    QString        font_family;
    QString        font_style_name;
    QString        resolved_font_family;
    QString        resolved_font_style_name;
    QString        text_preview;
    QString        codepoint_sample;
};

struct terminal_text_layout_slow_diagnostics_t
{
    std::uint64_t  threshold_ns    = 0U;
    std::uint64_t  slow_call_count = 0U;
    std::vector<terminal_text_layout_slow_diagnostic_t>
                   samples;
};

class Terminal_text_layout_slow_diagnostics_recorder final
{
public:
    void reset();

    void record_layout(
        std::uint64_t                      duration_ns,
        const QFont&                       font,
        const Terminal_render_text_run&    run,
        bool                               clipped,
        bool                               force_blended_order,
        bool                               ascii_layout_font,
        bool                               line_has_text);

    terminal_text_layout_slow_diagnostics_t snapshot() const;

private:
    mutable std::mutex                                  m_mutex;
    std::uint64_t                                       m_slow_call_count = 0U;
    std::vector<terminal_text_layout_slow_diagnostic_t> m_samples;
};
#endif

struct terminal_renderer_lifecycle_stats_t
{
    std::uint64_t release_resources_calls              = 0U;
    std::uint64_t item_scene_changes                   = 0U;
    std::uint64_t item_scene_detaches                  = 0U;
    std::uint64_t item_destructions                    = 0U;
    std::uint64_t scene_graph_invalidated_calls        = 0U;
    std::uint64_t render_node_deletions_in_paint       = 0U;
    std::uint64_t render_root_nodes_created            = 0U;
    std::uint64_t render_root_nodes_destroyed          = 0U;
    std::uint64_t render_text_resources_created        = 0U;
    std::uint64_t render_text_resources_destroyed      = 0U;
    std::uint64_t render_rect_resources_created        = 0U;
    std::uint64_t render_rect_resources_destroyed      = 0U;
};

class Terminal_renderer_lifecycle_recorder final
{
public:
    void record_release_resources()
    {
        increment(&terminal_renderer_lifecycle_stats_t::release_resources_calls);
    }

    void record_item_scene_change()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_scene_changes);
    }

    void record_item_scene_detach()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_scene_detaches);
    }

    void record_item_destruction()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_destructions);
    }

    void record_scene_graph_invalidated()
    {
        increment(&terminal_renderer_lifecycle_stats_t::scene_graph_invalidated_calls);
    }

    void record_render_node_deleted()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_node_deletions_in_paint);
    }

    void record_root_node_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_root_nodes_created);
    }

    void record_root_node_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_root_nodes_destroyed);
    }

    void record_text_resource_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_text_resources_created);
    }

    void record_text_resource_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_text_resources_destroyed);
    }

    void record_rect_resource_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_rect_resources_created);
    }

    void record_rect_resource_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_rect_resources_destroyed);
    }

    terminal_renderer_lifecycle_stats_t snapshot() const;

private:
    void increment(std::uint64_t terminal_renderer_lifecycle_stats_t::* counter)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++(m_stats.*counter);
    }

    mutable std::mutex                    m_mutex;
    terminal_renderer_lifecycle_stats_t   m_stats;
};

}
