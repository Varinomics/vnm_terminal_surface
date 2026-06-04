#pragma once

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QRawFont>
#include <QRect>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QSGNode;

namespace vnm_terminal::internal {

class Hierarchical_profiler;

struct Glyph_atlas_cache_key
{
    quint32 glyph_index         = 0U;
    QString fallback_face_id;
    qreal   physical_pixel_size = 0.0;
    int     subpixel_bucket     = 0;
};

bool operator==(
    const Glyph_atlas_cache_key& left,
    const Glyph_atlas_cache_key& right);

bool operator<(
    const Glyph_atlas_cache_key& left,
    const Glyph_atlas_cache_key& right);

struct Glyph_coverage_tile
{
    QSize      size;
    int        bytes_per_line = 0;
    QByteArray bytes;

    bool is_valid() const;
};

struct Glyph_atlas_slot
{
    int   page = -1;
    QRect rect;

    bool is_valid() const;
};

class Glyph_atlas_packer final
{
public:
    // The current atlas budget is one R8 texture page; page 1+ is rejected
    // until GPU multi-page addressing exists.
    explicit Glyph_atlas_packer(
        QSize page_size,
        int   gutter = 1,
        int   max_pages = 1);

    std::optional<Glyph_atlas_slot> pack(QSize tile_size);
    void reset();

    QSize page_size() const { return m_page_size; }
    int max_pages() const { return m_max_pages; }
    int page_count() const;

private:
    struct Shelf
    {
        int y      = 0;
        int height = 0;
        int x      = 0;
    };

    struct Page
    {
        std::vector<Shelf> shelves;
        int                next_y = 0;
    };

    std::optional<Glyph_atlas_slot> pack_in_page(
        int   page_index,
        QSize padded_size,
        QSize tile_size);

    QSize             m_page_size;
    int               m_gutter = 1;
    int               m_max_pages = 1;
    std::vector<Page> m_pages;
};

struct Glyph_atlas_cache_stats
{
    std::uint64_t epoch         = 0U;
    std::uint64_t invalidations = 0U;
    std::uint64_t lookups       = 0U;
    std::uint64_t hits          = 0U;
    std::uint64_t inserts       = 0U;
    std::uint64_t failed_inserts = 0U;
    std::uint64_t page_bytes    = 0U;
    std::uint64_t allocated_bytes = 0U;
    std::uint64_t budget_bytes  = 0U;
    std::uint64_t used_bytes    = 0U;
    int           page_count    = 0;
    int           page_budget   = 0;
    QSize         page_size;
};

struct Qsg_atlas_frame_build_summary
{
    Terminal_render_snapshot_basis   snapshot_basis =
        Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose snapshot_purpose =
        Terminal_render_snapshot_purpose::CONTENT;
    Terminal_buffer_id               viewport_active_buffer =
        Terminal_buffer_id::PRIMARY;
    int                              viewport_offset_from_tail = 0;
    int                              viewport_scrollback_rows  = 0;
    int                              dirty_rows                = 0;
    int                              full_dirty_rows           = 0;
    int                              frame_background_rects    = 0;
    int                              frame_selection_rects     = 0;
    int                              frame_graphic_rects       = 0;
    int                              frame_graphic_arcs        = 0;
    int                              frame_text_runs           = 0;
    int                              frame_cursor_graphic_rects = 0;
    int                              frame_cursor_graphic_arcs = 0;
    int                              frame_overlay_rects       = 0;
    int                              packed_rows               = 0;
    int                              packed_graphic_cells      = 0;
    int                              packed_hard_block_rects   = 0;
    int                              graphic_arc_raster_rects  = 0;
    int                              cursor_graphic_arc_raster_rects = 0;
    int                              rect_instances            = 0;
    int                              glyph_instances           = 0;
    int                              distinct_glyph_faces      = 0;
    int                              fallback_glyph_faces      = 0;
    int                              emoji_presentation_runs   = 0;
    int                              color_glyph_alpha_demotions = 0;
    int                              glyph_color_alpha_failures = 0;
    int                              glyph_coverage_failures   = 0;
    int                              glyph_atlas_insert_failures = 0;
    int                              glyph_missed_instances    = 0;
    int                              first_packed_logical_row  = 0;
    Terminal_buffer_id               first_packed_active_buffer =
        Terminal_buffer_id::PRIMARY;
    int                              first_text_logical_row = 0;
    std::uint64_t                    first_text_retained_line_id = 0U;
    std::uint64_t                    first_text_content_generation = 0U;
    bool                             selection_provenance_valid = false;
    bool                             full_dirty_range          = false;
    bool                             public_projection_full_repaint = false;
    bool                             scroll_full_repaint       = false;
    bool                             full_repaint_fallback     = false;
};

constexpr int k_qsg_atlas_all_rows = -1;
constexpr int k_qsg_atlas_non_row  = -2;

struct Qsg_atlas_buffer_update_range
{
    int byte_offset = 0;
    int byte_count  = 0;
};

struct Qsg_atlas_buffer_update_summary
{
    int  rhi_frames_in_flight          = 1;
    int  rhi_frame_slot                = 0;
    int  instance_count                = 0;
    int  active_instance_count         = 0;
    int  instance_bytes                = 0;
    int  buffer_bytes                  = 0;
    int  dirty_rows                    = 0;
    int  seeded_slots                  = 0;
    int  full_uploads                  = 0;
    int  partial_uploads               = 0;
    int  uploaded_bytes                = 0;
    bool full_upload                   = false;
    bool partial_upload                = false;
    bool skipped_upload                = false;
    bool rotating_slot_seed_upload     = false;
    bool buffer_recreated_upload       = false;
    bool instance_layout_changed_upload = false;
    bool full_repaint_upload           = false;
    bool non_dirty_state_upload        = false;
    bool row_stable_layout             = false;
};

struct Qsg_atlas_buffer_update_plan
{
    Qsg_atlas_buffer_update_summary
                                  summary;
    std::vector<Qsg_atlas_buffer_update_range>
                                  ranges;
};

struct Qsg_atlas_row_stable_range
{
    int row            = k_qsg_atlas_non_row;
    int first_instance = 0;
    int instance_count = 0;
};

struct Qsg_atlas_buffer_update_input
{
    int                                      frames_in_flight = 1;
    int                                      frame_slot       = 0;
    int                                      row_count        = 0;
    int                                      instance_size    = 1;
    const char*                              bytes            = nullptr;
    int                                      byte_count       = 0;
    const std::vector<int>*                  instance_rows    = nullptr;
    QByteArray                               layout_key;
    std::vector<Terminal_render_dirty_row_range>
                                             dirty_row_ranges;
    bool                                     buffer_recreated = false;
    bool                                     force_full_reupload = false;
    bool                                     non_dirty_state_invalidation = false;
    int                                      active_instance_count = -1;
    bool                                     row_stable_layout = false;
    const std::vector<Qsg_atlas_row_stable_range>*
                                             row_stable_ranges = nullptr;
};

class Qsg_atlas_buffer_upload_planner final
{
public:
    void reset();

    Qsg_atlas_buffer_update_plan plan(
        const Qsg_atlas_buffer_update_input& input);

private:
    void resize_slots(int frames_in_flight);

    int                         m_frames_in_flight = 0;
    std::vector<QByteArray>     m_slot_bytes;
    std::vector<std::vector<int>>
                                m_slot_instance_rows;
    std::vector<QByteArray>     m_slot_layout_keys;
    std::vector<unsigned char>  m_seeded_slots;
};

struct Qsg_atlas_render_summary
{
    Qsg_atlas_buffer_update_summary
                  rect_buffer;
    Qsg_atlas_buffer_update_summary
                  glyph_buffer;
    int           direct_ascii_text_runs            = 0;
    int           qt_layout_text_runs                = 0;
    int           direct_ascii_glyph_instances       = 0;
    int           qt_layout_glyph_instances          = 0;
    int           glyph_buffer_instances             = 0;
    int           glyph_text_row_capacity            = 0;
    int           glyph_cursor_text_row_capacity     = 0;
    int           background_rects_before_coalescing = 0;
    int           background_rects_after_coalescing  = 0;
    int           background_rects_coalesced         = 0;
    int           rect_draw_calls                    = 0;
    int           glyph_draw_calls                   = 0;
    int           draw_calls                         = 0;
    int           atlas_page_count                   = 0;
    int           atlas_page_budget                  = 0;
    std::uint64_t atlas_page_bytes                   = 0U;
    std::uint64_t atlas_allocated_bytes              = 0U;
    std::uint64_t atlas_budget_bytes                 = 0U;
    std::uint64_t atlas_used_bytes                   = 0U;
    std::uint64_t atlas_failed_inserts               = 0U;
    bool          coverage_texture_uploaded          = false;
    bool          coverage_texture_skipped           = false;
    bool          full_dirty_range_reupload          = false;
    bool          public_projection_full_reupload    = false;
    bool          scroll_full_reupload               = false;
    bool          non_dirty_selection_invalidation   = false;
    bool          non_dirty_cursor_invalidation      = false;
    bool          non_dirty_preedit_invalidation     = false;
    bool          non_dirty_options_invalidation     = false;
    bool          non_dirty_visual_bell_invalidation = false;
    bool          font_epoch_invalidation            = false;
};

class Glyph_atlas_cache final
{
public:
    explicit Glyph_atlas_cache(QSize page_size = QSize(256, 256));

    void set_epoch(std::uint64_t epoch);
    void reset();

    const Glyph_atlas_slot* find(const Glyph_atlas_cache_key& key) const;
    Glyph_atlas_slot insert_or_get(
        const Glyph_atlas_cache_key& key,
        const Glyph_coverage_tile&   tile);

    Glyph_atlas_cache_stats stats() const;
    const QByteArray& page_bytes(int page) const;

private:
    struct Entry
    {
        Glyph_atlas_slot slot;
    };

    void ensure_page_count(int page_count);
    void copy_tile_to_slot(
        int                        page,
        const QRect&               rect,
        const Glyph_coverage_tile& tile);

    Glyph_atlas_packer                     m_packer;
    std::map<Glyph_atlas_cache_key, Entry> m_entries;
    std::vector<QByteArray>                m_pages;
    Glyph_atlas_cache_stats                m_stats;
};

struct Captured_atlas_frame
{
    std::shared_ptr<const Terminal_render_snapshot>
                                     snapshot;
    Ime_preedit_state                ime_preedit;
    Terminal_render_options          options;
    terminal_cell_metrics_t          cell_metrics;
    QSizeF                           logical_size;
    QFont                            font;
    std::shared_ptr<Hierarchical_profiler>
                                     render_profiler;
    qreal                            device_pixel_ratio  = 1.0;
    std::uint64_t                    font_epoch          = 0U;
    std::uint64_t                    capture_sequence    = 0U;
    bool                             cursor_blink_visible = true;
};

struct Qsg_atlas_frame_report
{
    std::uint64_t capture_count                   = 0U;
    std::uint64_t prepare_count                   = 0U;
    std::uint64_t render_count                    = 0U;
    std::uint64_t capture_sequence                = 0U;
    std::uint64_t captured_snapshot_sequence      = 0U;
    std::uint64_t captured_font_epoch             = 0U;
    std::uint64_t first_render_capture_sequence   = 0U;
    std::uint64_t first_captured_snapshot_sequence = 0U;
    std::uint64_t first_captured_font_epoch       = 0U;
    std::uint64_t first_render_snapshot_sequence  = 0U;
    std::uint64_t first_render_font_epoch         = 0U;
    std::uint64_t render_capture_sequence         = 0U;
    std::uint64_t render_snapshot_sequence        = 0U;
    std::uint64_t render_font_epoch               = 0U;
    QColor        captured_diagnostic_color;
    QColor        first_captured_diagnostic_color;
    QColor        first_render_diagnostic_color;
    QColor        render_diagnostic_color;
    bool          captured_light_options          = false;
    bool          first_captured_light_options    = false;
    bool          first_render_light_options      = false;
    bool          render_light_options            = false;
    bool          command_buffer_non_null         = false;
    bool          render_target_non_null          = false;
    bool          rhi_non_null                    = false;
    bool          r8_texture_created              = false;
    bool          r8_upload_recorded              = false;
    bool          raw_font_rasterized             = false;
    bool          raw_font_rasterized_in_prepare  = false;
    int           rasterized_glyphs               = 0;
    int           atlas_page_count                = 0;
    QRect         viewport_rect;
    bool          drew                            = false;
    std::uint64_t prepare_thread_id               = 0U;
    std::uint64_t raw_font_raster_thread_id       = 0U;
    Glyph_atlas_cache_stats
                  cache;
    Qsg_atlas_frame_build_summary
                  frame_build;
    Qsg_atlas_render_summary
                  render;
};

class Qsg_atlas_recorder final
{
public:
    void reset();

    void record_capture(const Captured_atlas_frame& frame);
    void record_prepare(
        const Captured_atlas_frame&   frame,
        bool                          command_buffer_non_null,
        bool                          render_target_non_null,
        bool                          rhi_non_null,
        bool                          r8_texture_created,
        bool                          r8_upload_recorded,
        bool                          raw_font_rasterized,
        bool                          raw_font_rasterized_in_prepare,
        int                           rasterized_glyphs,
        std::uint64_t                 prepare_thread_id,
        std::uint64_t                 raw_font_raster_thread_id,
        const Glyph_atlas_cache_stats& cache,
        const Qsg_atlas_frame_build_summary& frame_build,
        const Qsg_atlas_render_summary& render_summary);
    void record_render(
        const Captured_atlas_frame& frame,
        QRect                       viewport_rect,
        bool                        drew);

    Qsg_atlas_frame_report snapshot() const;

private:
    mutable std::mutex            m_mutex;
    Qsg_atlas_frame_report m_report;
};

Glyph_coverage_tile qsg_atlas_coverage_tile_from_image(const QImage& image);

QString qsg_atlas_face_id_for_raw_font(const QRawFont& raw_font);

qreal qsg_atlas_physical_pixel_size(
    const QFont& font,
    qreal        device_pixel_ratio);

qreal qsg_atlas_physical_pixel_size(
    const QRawFont& raw_font,
    qreal           device_pixel_ratio);

Glyph_atlas_cache_key qsg_atlas_cache_key(
    quint32 glyph_index,
    QString fallback_face_id,
    qreal   physical_pixel_size,
    int     subpixel_bucket);

Captured_atlas_frame capture_qsg_atlas_frame(
    std::shared_ptr<const Terminal_render_snapshot>
                                  snapshot,
    Ime_preedit_state             ime_preedit,
    Terminal_render_options       options,
    terminal_cell_metrics_t       cell_metrics,
    QSizeF                        logical_size,
    QFont                         font,
    std::shared_ptr<Hierarchical_profiler>
                                  render_profiler,
    qreal                         device_pixel_ratio,
    std::uint64_t                 font_epoch,
    std::uint64_t                 capture_sequence,
    bool                          cursor_blink_visible);

QColor qsg_atlas_diagnostic_color(const Captured_atlas_frame& frame);

QSGNode* update_qsg_atlas_node(
    QSGNode*                                      old_node,
    Captured_atlas_frame                         frame,
    const std::shared_ptr<Qsg_atlas_recorder>&
                                                  recorder);

}
