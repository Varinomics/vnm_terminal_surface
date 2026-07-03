#pragma once

#if !defined(VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED)
#define VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED 0
#endif

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPoint>
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

struct QRhiDriverInfo;
class QSGNode;

namespace vnm_terminal::internal {

class Hierarchical_profiler;

constexpr bool k_qsg_atlas_msdf_text_renderer_enabled =
    VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED != 0;
constexpr bool k_qsg_atlas_msdf_text_renderer_compiled =
    k_qsg_atlas_msdf_text_renderer_enabled;
constexpr bool k_qsg_atlas_msdf_text_renderer_active = false;

bool qsg_atlas_driver_info_is_known_software_renderer(
    const QRhiDriverInfo& driver_info);

enum class Glyph_coverage_kind
{
    UNKNOWN,
    GRAYSCALE_MASK,
    LCD_RGB_MASK,
    LCD_BGR_MASK,
    COLOR_IMAGE,
    AMBIGUOUS,
    UNSUPPORTED,
};

enum class Glyph_lcd_order
{
    UNKNOWN,
    RGB,
    BGR,
};

enum class Glyph_image_presentation
{
    UNKNOWN,
    TEXT,
    COLOR,
};

struct Glyph_atlas_cache_key
{
    quint32                  glyph_index         = 0U;
    QString                  fallback_face_id;
    qreal                    physical_pixel_size = 0.0;
    Glyph_image_presentation presentation =
        Glyph_image_presentation::TEXT;
    Glyph_coverage_kind      coverage_kind       =
        Glyph_coverage_kind::GRAYSCALE_MASK;
    Glyph_lcd_order          lcd_order           = Glyph_lcd_order::UNKNOWN;
    int                      subpixel_bucket     = 0;
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

struct Glyph_rgba_tile
{
    Glyph_coverage_kind coverage_kind =
        Glyph_coverage_kind::UNKNOWN;
    Glyph_lcd_order     lcd_order = Glyph_lcd_order::UNKNOWN;
    QSize               size;
    int                 bytes_per_line = 0;
    QImage::Format      source_format = QImage::Format_Invalid;
    QByteArray          bytes;

    bool is_valid() const;
};

struct Glyph_atlas_slot
{
    int                 page          = -1;
    QRect               rect;
    QPoint              physical_offset;
    Glyph_coverage_kind coverage_kind = Glyph_coverage_kind::UNKNOWN;
    Glyph_lcd_order     lcd_order     = Glyph_lcd_order::UNKNOWN;

    bool is_valid() const;
};

enum class Qsg_atlas_sampler_mode
{
    UNKNOWN,
    NEAREST,
    LINEAR,
};

struct Glyph_coverage_counts
{
    int grayscale_masks    = 0;
    int lcd_rgb_masks      = 0;
    int lcd_bgr_masks      = 0;
    int color_images       = 0;
    int ambiguous_images   = 0;
    int unsupported_images = 0;
    int missed_images      = 0;
};

struct Qsg_atlas_cursor_report
{
    bool                  valid   = false;
    bool                  visible = false;
    Terminal_cursor_shape shape   = Terminal_cursor_shape::BLOCK;
    int                   row     = -1;
    int                   column  = -1;
};

struct Qsg_atlas_glyph_image_diagnostic
{
    Glyph_coverage_kind      coverage_kind =
        Glyph_coverage_kind::UNKNOWN;
    Glyph_image_presentation presentation =
        Glyph_image_presentation::UNKNOWN;
    QImage::Format           source_format        = QImage::Format_Invalid;
    QSize                    source_size;
    quint32                  glyph_index          = 0U;
    QString                  fallback_face_id;
    int                      text_run_index       = 0;
    int                      glyph_run_index      = 0;
    int                      glyph_index_in_run   = 0;
    qsizetype                source_string_start  = 0;
    qsizetype                source_string_end    = 0;
};

enum class Qsg_atlas_glyph_miss_cause
{
    NONE,
    UNSUPPORTED_IMAGE,
    ATLAS_INSERT_FAILED,
    REJECTED_RASTER_CACHE,
};

struct Qsg_atlas_glyph_miss_diagnostic
{
    bool                            valid = false;
    Qsg_atlas_glyph_miss_cause      cause =
        Qsg_atlas_glyph_miss_cause::NONE;
    Qsg_atlas_glyph_image_diagnostic image;
    QSize                           tile_size;
    int                             tile_bytes_per_line = 0;
    int                             atlas_page_count    = 0;
    int                             atlas_page_budget   = 0;
    QSize                           atlas_page_size;
};

class Glyph_atlas_packer final
{
public:
    // Texture-array page addressing is part of the RGBA atlas contract.
    explicit Glyph_atlas_packer(
        QSize page_size,
        int   gutter = 1,
        int   max_pages = 4);

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

struct Glyph_rgba_cache_accounting
{
    std::uint64_t page_bytes      = 0U;
    std::uint64_t allocated_bytes = 0U;
    std::uint64_t budget_bytes    = 0U;
    std::uint64_t used_bytes      = 0U;
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
    int                              frame_overlay_rects       = 0;
    int                              frame_row_descriptors     = 0;
    int                              frame_layer_descriptors   = 0;
    int                              qsg_layer_descriptors     = 0;
    int                              rect_instances            = 0;
    int                              glyph_instances           = 0;
    int                              max_glyph_instance_page   = -1;
    int                              distinct_glyph_faces      = 0;
    int                              fallback_glyph_faces      = 0;
    int                              emoji_presentation_runs   = 0;
    int                              glyph_coverage_failures   = 0;
    int                              glyph_atlas_insert_failures = 0;
    int                              glyph_missed_instances    = 0;
    Glyph_coverage_counts            glyph_coverage;
    Qsg_atlas_glyph_miss_diagnostic  first_glyph_miss;
    int                              snapped_origin_failures   = 0;
    int                              first_text_logical_row = 0;
    std::uint64_t                    first_text_retained_line_id = 0U;
    std::uint64_t                    first_text_content_generation = 0U;
    Qsg_atlas_cursor_report          emitted_cursor;
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

enum class Qsg_atlas_buffer_update_result
{
    NOT_RUN,
    READY,
    RECT_BUFFER_FAILED,
    GLYPH_BUFFER_FAILED,
    MSDF_TEXT_BUFFER_FAILED,
    FULL_UPLOAD_REQUIRES_POPULATED_FRAME,
};

bool qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
    bool                            already_retried,
    bool                            gpu_resources_ready,
    bool                            has_msdf_text_draw_passes,
    Qsg_atlas_buffer_update_result  buffer_update_result);

bool qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
    bool already_retried,
    bool has_msdf_text_draw_passes,
    bool msdf_prepare_resource_attempted,
    bool msdf_prepare_resource_failed);

void qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(
    std::uint64_t sequence);

void qsg_atlas_clear_resource_prepare_failure_for_testing();

void qsg_atlas_fail_msdf_resource_prepare_for_snapshot_sequence_for_testing(
    std::uint64_t sequence);

void qsg_atlas_fail_msdf_text_buffer_update_for_snapshot_sequence_for_testing(
    std::uint64_t sequence);

void qsg_atlas_clear_msdf_resource_failures_for_testing();

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
    bool full_upload_requires_populated_frame = false;
    bool rotating_slot_seed_upload     = false;
    bool buffer_recreated_upload       = false;
    bool instance_layout_changed_upload = false;
    bool full_repaint_upload           = false;
    bool non_dirty_state_upload        = false;
    bool row_stable_layout             = false;
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

struct Qsg_atlas_buffer_upload_commit
{
    int                         frames_in_flight = 0;
    std::vector<QByteArray>     slot_bytes;
    std::vector<QByteArray>     slot_layout_keys;
    std::vector<unsigned char>  seeded_slots;
    bool                        valid = false;
};

struct Qsg_atlas_buffer_update_plan
{
    Qsg_atlas_buffer_update_summary
                                  summary;
    std::vector<Qsg_atlas_buffer_update_range>
                                  ranges;
    Qsg_atlas_buffer_upload_commit
                                  commit;
};

struct Qsg_atlas_shaped_glyph_record
{
    int                 text_run_index      = 0;
    bool                cursor_text_run     = false;
    int                 glyph_run_index     = 0;
    int                 glyph_index_in_run  = 0;
    int                 row                 = 0;
    int                 logical_row         = 0;
    std::uint64_t       retained_line_id    = 0U;
    std::uint64_t       content_generation  = 0U;
    int                 run_column          = 0;
    int                 owner_column        = 0;
    int                 owner_cell_span     = 1;
    qsizetype           source_string_start = 0;
    qsizetype           source_string_end   = 0;
    quint32             glyph_index         = 0U;
    QRawFont            raw_font;
    QString             fallback_face_id;
    qreal               physical_pixel_size = 0.0;
    QPointF             glyph_origin;
    QRectF              glyph_bounds;
};

struct Qsg_atlas_shaped_text_run_result
{
    std::vector<Qsg_atlas_shaped_glyph_record> records;
    int                 missing_string_indexes = 0;
    int                 invalid_string_indexes = 0;
};

class Qsg_atlas_buffer_upload_planner final
{
public:
    void reset();

    Qsg_atlas_buffer_update_plan plan(
        const Qsg_atlas_buffer_update_input& input);
    void commit(const Qsg_atlas_buffer_update_plan& plan);

private:
    Qsg_atlas_buffer_upload_commit commit_for_frames(int frames_in_flight) const;

    int                         m_frames_in_flight = 0;
    std::vector<QByteArray>     m_slot_bytes;
    std::vector<QByteArray>     m_slot_layout_keys;
    std::vector<unsigned char>  m_seeded_slots;
};

struct Qsg_atlas_render_summary
{
    Qsg_atlas_buffer_update_summary
                  rect_buffer;
    Qsg_atlas_buffer_update_summary
                  glyph_buffer;
    Qsg_atlas_buffer_update_summary
                  msdf_text_buffer;
    int           shaped_text_runs                  = 0;
    int           shaped_glyph_records              = 0;
    int           shaped_missing_string_indexes     = 0;
    int           shaped_invalid_string_indexes     = 0;
    int           glyph_buffer_instances             = 0;
    int           rect_row_capacity                  = 0;
    int           glyph_text_row_capacity            = 0;
    int           glyph_cursor_text_row_capacity     = 0;
    int           background_rects_before_coalescing = 0;
    int           background_rects_after_coalescing  = 0;
    int           background_rects_coalesced         = 0;
    int           rect_draw_calls                    = 0;
    int           glyph_draw_calls                   = 0;
    int           msdf_text_draw_calls               = 0;
    int           draw_calls                         = 0;
    Terminal_text_renderer_policy
                  text_renderer_policy               =
                      Terminal_text_renderer_policy::AUTO;
    Terminal_text_renderer_kind
                  effective_text_renderer            =
                      Terminal_text_renderer_kind::NONE;
    bool          text_renderer_fallback_allowed     = true;
    bool          text_renderer_fallback_used        = false;
    Terminal_lcd_subpixel_order
                  msdf_lcd_subpixel_order            =
                      Terminal_lcd_subpixel_order::NONE;
    bool          msdf_lcd_text_enabled              = false;
    int           msdf_text_supported_runs           = 0;
    int           msdf_text_runs                     = 0;
    int           msdf_text_glyph_instances          = 0;
    int           msdf_text_missed_supported_runs    = 0;
    int           msdf_text_missed_supported_glyphs  = 0;
    int           msdf_text_font_data_bytes          = 0;
    int           msdf_text_pixel_height             = 0;
    int           msdf_text_atlas_size               = 0;
    float         msdf_text_px_range                 = 0.0f;
    // Zoom-validation instrumentation (Batch 1). In Batch 1 the baked pixel
    // height equals the draw pixel height; baked/draw separation arrives in
    // Batch 3. Cross-size atlas reuse is not yet enabled, so
    // msdf_text_baked_atlas_reused is diagnostic-only until then.
    int           msdf_text_baked_pixel_height       = 0;
    std::uint64_t msdf_text_atlas_generation         = 0U;
    bool          msdf_text_cache_hit                = false;
    bool          msdf_text_cache_miss               = false;
    bool          msdf_text_baked_atlas_reused       = false;
    bool          msdf_text_atlas_build_attempted    = false;
    bool          msdf_text_atlas_build_succeeded    = false;
    std::uint64_t msdf_text_atlas_build_attempts_total  = 0U;
    std::uint64_t msdf_text_atlas_build_successes_total = 0U;
    std::uint64_t msdf_text_atlas_texture_uploads_total = 0U;
    std::uint64_t msdf_text_baked_cache_hits_total      = 0U;
    std::uint64_t msdf_text_baked_cache_misses_total    = 0U;
    int           atlas_page_count                   = 0;
    int           atlas_page_budget                  = 0;
    std::uint64_t atlas_page_bytes                   = 0U;
    std::uint64_t atlas_allocated_bytes              = 0U;
    std::uint64_t atlas_budget_bytes                 = 0U;
    std::uint64_t atlas_used_bytes                   = 0U;
    std::uint64_t atlas_failed_inserts               = 0U;
    Qsg_atlas_sampler_mode
                  glyph_sampler_mode                 =
                      Qsg_atlas_sampler_mode::UNKNOWN;
    Qsg_atlas_sampler_mode
                  msdf_text_sampler_mode             =
                      Qsg_atlas_sampler_mode::UNKNOWN;
    bool          coverage_texture_uploaded          = false;
    bool          coverage_texture_skipped           = false;
    bool          msdf_text_texture_uploaded         = false;
    bool          msdf_text_texture_ready            = false;
    bool          atlas_page_pressure                = false;
    bool          glyph_shader_package_available     = false;
    bool          msdf_text_shader_package_available = false;
    bool          msdf_text_atlas_built              = false;
    bool          msdf_text_atlas_ready              = false;
    bool          dual_source_probe_shader_package_available = false;
    bool          dual_source_blend_factors_available = false;
    bool          dual_source_blend_factors_runtime_probe = false;
    bool          msdf_text_renderer_enabled =
        k_qsg_atlas_msdf_text_renderer_enabled;
    bool          msdf_text_renderer_compiled =
        k_qsg_atlas_msdf_text_renderer_compiled;
    bool          msdf_text_renderer_active =
        k_qsg_atlas_msdf_text_renderer_active;
    bool          msdf_text_resources_ready          = false;
    QString       msdf_text_message;
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

void qsg_atlas_merge_msdf_text_failure_diagnostics(
    const Qsg_atlas_render_summary& failed_msdf_render,
    Qsg_atlas_render_summary&       fallback_render);

struct Qsg_atlas_producer_summary
{
    int text_runs_considered          = 0;
    int text_runs_empty               = 0;
    int shape_cache_lookups           = 0;
    int shape_cache_hits              = 0;
    int shape_cache_misses            = 0;
    int shape_cache_inserts           = 0;
    int shape_cache_pruned            = 0;
    int shape_cache_entries           = 0;
    int shaped_runs_built             = 0;
    int shaped_runs_reused            = 0;
    int shaped_glyph_records_built    = 0;
    int shaped_glyph_records_reused   = 0;
    int presentation_run_scans        = 0;
    int presentation_source_scans     = 0;
    int presentation_fast_text_runs   = 0;
    int presentation_emoji_runs       = 0;
    int slot_resolutions_built        = 0;
    int slot_resolutions_reused       = 0;
    int simple_path_attempts          = 0;
    int simple_path_used              = 0;
    int simple_path_fallbacks         = 0;
};

struct Qsg_atlas_warm_lazy_summary
{
    bool          warm_completed           = false;
    bool          warm_broad_seed_skipped  = false;
    std::uint64_t warm_epoch               = 0U;
    int           warm_seed_strings        = 0;
    int           warm_shaped_glyph_records = 0;
    int           warm_covered_glyph_records = 0;
    int           warm_skipped_glyph_records = 0;
    int           warm_environment_skipped_glyph_records = 0;
    int           warm_failed_glyph_records = 0;
    int           warm_missing_string_indexes = 0;
    int           warm_invalid_string_indexes = 0;
    int           warm_unsupported_images  = 0;
    int           warm_cache_hits          = 0;
    int           warm_insert_attempts     = 0;
    int           warm_inserts             = 0;
    int           warm_failed_inserts      = 0;
    double        warm_elapsed_ms          = 0.0;
    bool          warm_page_pressure       = false;
    int           lazy_insert_attempts     = 0;
    int           lazy_inserts             = 0;
    int           lazy_failed_inserts      = 0;
    double        lazy_elapsed_ms          = 0.0;
    int           lazy_max_insert_us       = 0;
    int           lazy_frames              = 0;
    int           incomplete_frames        = 0;
};

class Glyph_atlas_cache final
{
public:
    explicit Glyph_atlas_cache(QSize page_size = QSize(256, 256));

    void set_epoch(std::uint64_t epoch);
    void reset();

    const Glyph_atlas_slot* find(const Glyph_atlas_cache_key& key);
    Glyph_atlas_slot insert_or_get(
        const Glyph_atlas_cache_key& key,
        const Glyph_rgba_tile&       tile,
        QPoint                       physical_offset = {});

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
        const Glyph_rgba_tile&      tile);

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
    std::uint64_t                    publication_generation = 0U;
    bool                             cursor_blink_visible = true;
};

struct Qsg_atlas_frame_report
{
    std::uint64_t capture_count                   = 0U;
    std::uint64_t prepare_count                   = 0U;
    std::uint64_t render_count                    = 0U;
    std::uint64_t capture_sequence                = 0U;
    std::uint64_t captured_snapshot_sequence      = 0U;
    std::uint64_t captured_publication_generation = 0U;
    std::uint64_t captured_font_epoch             = 0U;
    std::uint64_t first_render_capture_sequence   = 0U;
    std::uint64_t first_captured_snapshot_sequence = 0U;
    std::uint64_t first_captured_publication_generation = 0U;
    std::uint64_t first_captured_font_epoch       = 0U;
    std::uint64_t first_render_snapshot_sequence  = 0U;
    std::uint64_t first_render_publication_generation = 0U;
    std::uint64_t first_render_font_epoch         = 0U;
    std::uint64_t render_capture_sequence         = 0U;
    std::uint64_t render_snapshot_sequence        = 0U;
    std::uint64_t render_publication_generation   = 0U;
    std::uint64_t render_font_epoch               = 0U;
    QColor        captured_diagnostic_color;
    QColor        first_captured_diagnostic_color;
    QColor        first_render_diagnostic_color;
    QColor        render_diagnostic_color;
    bool          captured_light_options          = false;
    bool          first_captured_light_options    = false;
    bool          first_render_light_options      = false;
    bool          render_light_options            = false;
    Qsg_atlas_cursor_report
                  captured_snapshot_cursor;
    Qsg_atlas_cursor_report
                  captured_render_cursor;
    bool          command_buffer_non_null         = false;
    bool          render_target_non_null          = false;
    bool          rhi_non_null                    = false;
    Terminal_text_renderer_policy
                  text_renderer_policy               =
                      Terminal_text_renderer_policy::AUTO;
    Terminal_text_renderer_kind
                  effective_text_renderer            =
                      Terminal_text_renderer_kind::NONE;
    bool          text_renderer_fallback_allowed     = true;
    bool          text_renderer_fallback_used        = false;
    Terminal_lcd_subpixel_order
                  msdf_lcd_subpixel_order            =
                      Terminal_lcd_subpixel_order::NONE;
    bool          msdf_lcd_text_enabled              = false;
    bool          msdf_text_renderer_enabled =
        k_qsg_atlas_msdf_text_renderer_enabled;
    bool          msdf_text_renderer_compiled =
        k_qsg_atlas_msdf_text_renderer_compiled;
    bool          msdf_text_renderer_active =
        k_qsg_atlas_msdf_text_renderer_active;
    bool          msdf_text_shader_package_available = false;
    bool          msdf_text_atlas_built              = false;
    bool          msdf_text_atlas_ready              = false;
    bool          msdf_text_texture_ready            = false;
    bool          msdf_text_resources_ready          = false;
    int           msdf_text_supported_runs           = 0;
    int           msdf_text_runs                     = 0;
    int           msdf_text_glyph_instances          = 0;
    int           msdf_text_draw_calls               = 0;
    int           msdf_text_missed_supported_runs    = 0;
    int           msdf_text_missed_supported_glyphs  = 0;
    int           msdf_text_font_data_bytes          = 0;
    int           msdf_text_pixel_height             = 0;
    int           msdf_text_atlas_size               = 0;
    float         msdf_text_px_range                 = 0.0f;
    QString       msdf_text_message;
    bool          coverage_texture_created        = false;
    bool          coverage_upload_recorded        = false;
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
    Qsg_atlas_producer_summary
                  producer;
    Qsg_atlas_warm_lazy_summary
                  warm_lazy;
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
        bool                          coverage_texture_created,
        bool                          coverage_upload_recorded,
        bool                          raw_font_rasterized,
        bool                          raw_font_rasterized_in_prepare,
        int                           rasterized_glyphs,
        std::uint64_t                 prepare_thread_id,
        std::uint64_t                 raw_font_raster_thread_id,
        const Glyph_atlas_cache_stats& cache,
        const Qsg_atlas_frame_build_summary& frame_build,
        const Qsg_atlas_render_summary& render_summary,
        const Qsg_atlas_producer_summary& producer_summary,
        const Qsg_atlas_warm_lazy_summary& warm_lazy_summary);
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

Glyph_rgba_tile qsg_atlas_rgba_tile_from_image(
    const QImage&             image,
    Glyph_image_presentation  presentation =
        Glyph_image_presentation::UNKNOWN,
    Glyph_coverage_kind       requested_coverage_kind =
        Glyph_coverage_kind::UNKNOWN);

std::uint64_t qsg_atlas_rgba_tile_byte_count(QSize size);

Glyph_rgba_cache_accounting qsg_atlas_rgba_cache_accounting(
    const Glyph_atlas_cache_stats& cache);

Glyph_coverage_kind qsg_atlas_classify_glyph_image_candidate(
    const QImage&             image,
    Glyph_image_presentation  presentation = Glyph_image_presentation::UNKNOWN);

Qsg_atlas_glyph_image_diagnostic qsg_atlas_glyph_image_diagnostic_from_record(
    const Qsg_atlas_shaped_glyph_record& record,
    const QImage&                        image,
    Glyph_image_presentation             presentation);

const char* qsg_atlas_glyph_coverage_kind_name(Glyph_coverage_kind kind);

const char* qsg_atlas_glyph_miss_cause_name(
    Qsg_atlas_glyph_miss_cause cause);

const char* qsg_atlas_glyph_image_presentation_name(
    Glyph_image_presentation presentation);

const char* qsg_atlas_sampler_mode_name(Qsg_atlas_sampler_mode mode);

const char* qsg_atlas_text_renderer_policy_name(
    Terminal_text_renderer_policy policy);

const char* qsg_atlas_text_renderer_kind_name(
    Terminal_text_renderer_kind kind);

const char* qsg_atlas_lcd_subpixel_order_name(
    Terminal_lcd_subpixel_order order);

QFont qsg_atlas_cell_stable_ascii_layout_font(const QFont& font);

// True when the MSDF text renderer can actually render the given font (its bytes
// resolve and a ready atlas bakes). Thread-safe; used to gate the MSDF option in
// the UI so it is never offered for a font MSDF cannot render.
bool qsg_atlas_msdf_text_available_for_font(const QFont& font);

QString qsg_atlas_face_id_for_raw_font(const QRawFont& raw_font);

qreal qsg_atlas_physical_pixel_size(
    const QFont& font,
    qreal        device_pixel_ratio);

qreal qsg_atlas_physical_pixel_size(
    const QRawFont& raw_font,
    qreal           device_pixel_ratio);

QPoint qsg_atlas_glyph_physical_offset_for_raster_font(
    const QRawFont&           raster_font,
    quint32                   glyph_index,
    Glyph_image_presentation  presentation,
    bool                      lcd_text_path_enabled = true);

QPointF qsg_atlas_snapped_physical_point(
    QPointF point,
    qreal   device_pixel_ratio);

QRectF qsg_atlas_snapped_glyph_draw_rect(
    QPointF glyph_origin,
    QPoint  glyph_physical_offset,
    QSize   glyph_physical_size,
    qreal   device_pixel_ratio);

Glyph_atlas_cache_key qsg_atlas_cache_key(
    quint32              glyph_index,
    QString              fallback_face_id,
    qreal                physical_pixel_size,
    int                  subpixel_bucket,
    Glyph_coverage_kind  coverage_kind =
        Glyph_coverage_kind::GRAYSCALE_MASK,
    Glyph_image_presentation presentation =
        Glyph_image_presentation::TEXT);

Qsg_atlas_shaped_text_run_result qsg_atlas_shape_text_run(
    const Terminal_render_text_run& run,
    const QFont&                    font,
    terminal_cell_metrics_t         cell_metrics,
    qreal                           device_pixel_ratio,
    int                             text_run_index = 0,
    bool                            cursor_text_run = false);

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

Qsg_atlas_frame_report capture_qsg_atlas_frame_for_testing(
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
