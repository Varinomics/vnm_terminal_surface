#pragma once

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include <cstdint>
#include <mutex>

namespace vnm_terminal::internal {

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
