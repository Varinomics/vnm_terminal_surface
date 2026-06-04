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
    // Stage 1 uploads one R8 atlas texture; page 1+ is rejected until GPU
    // multi-page addressing exists.
    explicit Glyph_atlas_packer(
        QSize page_size,
        int   gutter = 1,
        int   max_pages = 1);

    std::optional<Glyph_atlas_slot> pack(QSize tile_size);
    void reset();

    QSize page_size() const { return m_page_size; }
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
    int           page_count    = 0;
    QSize         page_size;
};

struct Qsg_atlas_stage3_frame_summary
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
    qreal                            device_pixel_ratio  = 1.0;
    std::uint64_t                    font_epoch          = 0U;
    std::uint64_t                    capture_sequence    = 0U;
    bool                             cursor_blink_visible = true;
};

struct Qsg_atlas_stage1_frame_report
{
    std::uint64_t capture_count                   = 0U;
    std::uint64_t prepare_count                   = 0U;
    std::uint64_t render_count                    = 0U;
    std::uint64_t capture_sequence                = 0U;
    std::uint64_t captured_snapshot_sequence      = 0U;
    std::uint64_t captured_font_epoch             = 0U;
    std::uint64_t first_captured_snapshot_sequence = 0U;
    std::uint64_t first_captured_font_epoch       = 0U;
    std::uint64_t first_render_snapshot_sequence  = 0U;
    std::uint64_t first_render_font_epoch         = 0U;
    QColor        captured_probe_color;
    QColor        first_captured_probe_color;
    QColor        first_render_probe_color;
    bool          captured_light_options          = false;
    bool          first_captured_light_options    = false;
    bool          first_render_light_options      = false;
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
    Qsg_atlas_stage3_frame_summary
                  stage3;
};

class Qsg_atlas_stage1_recorder final
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
        const Qsg_atlas_stage3_frame_summary& stage3);
    void record_render(
        const Captured_atlas_frame& frame,
        QRect                       viewport_rect,
        bool                        drew);

    Qsg_atlas_stage1_frame_report snapshot() const;

private:
    mutable std::mutex            m_mutex;
    Qsg_atlas_stage1_frame_report m_report;
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
    bool                          cursor_blink_visible);

QColor qsg_atlas_stage1_probe_color(const Captured_atlas_frame& frame);

QSGNode* update_qsg_atlas_stage1_node(
    QSGNode*                                      old_node,
    Captured_atlas_frame                         frame,
    const std::shared_ptr<Qsg_atlas_stage1_recorder>&
                                                  recorder);

}
