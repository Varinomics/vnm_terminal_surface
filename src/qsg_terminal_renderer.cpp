#include "vnm_terminal/internal/qsg_terminal_renderer.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/terminal_graphic_geometry.h"
#include "vnm_terminal/internal/unicode_width.h"
#include <QByteArray>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QLatin1Char>
#include <QRawFont>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

namespace {

constexpr qreal       k_no_wrap_text_line_width             = 1024.0 * 1024.0;
constexpr qreal       k_faint_foreground_alpha_factor       = 0.5;
constexpr qreal       k_ascii_advance_tolerance_floor       = 0.000001;
constexpr qreal       k_ascii_advance_tolerance_ratio       = 0.0000001;
constexpr qreal       k_text_geometry_tolerance             = k_ascii_advance_tolerance_floor;
constexpr int         k_printable_ascii_first               = 0x20;
constexpr int         k_printable_ascii_last                = 0x7e;
constexpr std::size_t k_printable_ascii_count               =
    static_cast<std::size_t>(k_printable_ascii_last - k_printable_ascii_first + 1);
constexpr int         k_utf16_classifier_block_code_units    = 32;
constexpr std::size_t k_max_coalesced_ascii_text_run_length = 384U;
constexpr qsizetype   k_max_cached_ascii_replacement_shape_length =
    static_cast<qsizetype>(k_max_coalesced_ascii_text_run_length);
constexpr std::size_t k_cached_ascii_replacement_shape_slot_count =
    static_cast<std::size_t>(k_max_cached_ascii_replacement_shape_length) + 1U;
constexpr std::size_t k_eager_render_style_attribute_limit  = 256U;

std::size_t bounded_frame_reserve(
    std::size_t cell_count,
    int         visible_rows,
    std::size_t entries_per_row,
    std::size_t minimum)
{
    const std::size_t row_count = visible_rows > 0
        ? static_cast<std::size_t>(visible_rows)
        : 1U;
    const std::size_t row_bound = std::max(minimum, row_count * entries_per_row);
    return std::min(cell_count, row_bound);
}

#if VNM_TERMINAL_PROFILING_ENABLED
constexpr std::uint64_t k_slow_text_layout_threshold_ns           = 10U * 1000U * 1000U;
constexpr std::size_t   k_max_slow_text_layout_samples            = 16U;
constexpr int           k_slow_text_layout_preview_utf16_units    = 96;
constexpr int           k_slow_text_layout_codepoint_sample_count = 24;

struct text_layout_diagnostic_context_t
{
    const QFont*                    font = nullptr;
    const Terminal_render_text_run* run = nullptr;
    bool                            clipped = false;
    bool                            force_blended_order = false;
    bool                            ascii_layout_font = false;
};

thread_local Terminal_text_layout_slow_diagnostics_recorder*
    active_slow_text_layout_recorder = nullptr;
thread_local const text_layout_diagnostic_context_t*
    active_text_layout_diagnostic_context = nullptr;

class Active_slow_text_layout_recorder_binding
{
public:
    explicit Active_slow_text_layout_recorder_binding(
        Terminal_text_layout_slow_diagnostics_recorder* recorder)
    :
        m_previous(active_slow_text_layout_recorder)
    {
        active_slow_text_layout_recorder = recorder;
    }

    ~Active_slow_text_layout_recorder_binding()
    {
        active_slow_text_layout_recorder = m_previous;
    }

    Active_slow_text_layout_recorder_binding(
        const Active_slow_text_layout_recorder_binding&) = delete;
    Active_slow_text_layout_recorder_binding& operator=(
        const Active_slow_text_layout_recorder_binding&) = delete;

private:
    Terminal_text_layout_slow_diagnostics_recorder* m_previous = nullptr;
};

class Active_text_layout_diagnostic_context_binding
{
public:
    explicit Active_text_layout_diagnostic_context_binding(
        const text_layout_diagnostic_context_t* context)
    :
        m_previous(active_text_layout_diagnostic_context)
    {
        active_text_layout_diagnostic_context = context;
    }

    ~Active_text_layout_diagnostic_context_binding()
    {
        active_text_layout_diagnostic_context = m_previous;
    }

    Active_text_layout_diagnostic_context_binding(
        const Active_text_layout_diagnostic_context_binding&) = delete;
    Active_text_layout_diagnostic_context_binding& operator=(
        const Active_text_layout_diagnostic_context_binding&) = delete;

private:
    const text_layout_diagnostic_context_t* m_previous = nullptr;
};

std::uint32_t codepoint_at(const QString& text, int& index)
{
    const QChar current = text.at(index);
    if (current.isHighSurrogate() && index + 1 < text.size()) {
        const QChar next = text.at(index + 1);
        if (next.isLowSurrogate()) {
            index += 2;
            return 0x10000U +
                (((static_cast<std::uint32_t>(current.unicode()) - 0xD800U) << 10U) |
                    (static_cast<std::uint32_t>(next.unicode()) - 0xDC00U));
        }
    }

    ++index;
    return static_cast<std::uint32_t>(current.unicode());
}

int text_codepoint_count(const QString& text)
{
    int count = 0;
    for (int index = 0; index < text.size();) {
        (void)codepoint_at(text, index);
        ++count;
    }
    return count;
}

std::uint64_t text_hash(const QString& text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const QChar character : text) {
        hash ^= static_cast<std::uint64_t>(character.unicode());
        hash *= 1099511628211ULL;
    }
    return hash;
}

QString text_codepoint_sample(const QString& text)
{
    QString sample;
    int emitted = 0;
    int index   = 0;
    for (;
        index < text.size() && emitted < k_slow_text_layout_codepoint_sample_count;)
    {
        const std::uint32_t codepoint = codepoint_at(text, index);
        if (!sample.isEmpty()) {
            sample += QLatin1Char(' ');
        }
        sample += QStringLiteral("U+%1")
            .arg(
                static_cast<qulonglong>(codepoint),
                codepoint > 0xFFFFU ? 6 : 4,
                16,
                QLatin1Char('0'))
            .toUpper();
        ++emitted;
    }
    if (index < text.size()) {
        sample += QStringLiteral(" ...");
    }
    return sample;
}

void classify_text(
    const QString& text,
    bool&          out_ascii_only,
    bool&          out_printable_ascii_only,
    bool&          out_has_control_codepoint)
{
    out_ascii_only            = true;
    out_printable_ascii_only  = true;
    out_has_control_codepoint = false;

    for (const QChar character : text) {
        const ushort value = character.unicode();
        if (value > 0x7FU) {
            out_ascii_only = false;
            out_printable_ascii_only = false;
        }
        if (value < 0x20U || value == 0x7FU) {
            out_has_control_codepoint = true;
            out_printable_ascii_only = false;
        }
    }
}

terminal_text_layout_slow_diagnostic_t make_slow_text_layout_diagnostic(
    std::uint64_t                      duration_ns,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    bool                               clipped,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    bool                               line_has_text)
{
    terminal_text_layout_slow_diagnostic_t diagnostic;
    diagnostic.duration_ns              = duration_ns;
    diagnostic.text_hash                = text_hash(run.text);
    diagnostic.text_utf16_units         = run.text.size();
    diagnostic.text_codepoints          = text_codepoint_count(run.text);
    diagnostic.row                      = run.row;
    diagnostic.logical_row              = run.logical_row;
    diagnostic.column                   = run.column;
    diagnostic.style_id                 = static_cast<int>(run.style_id);
    diagnostic.hyperlink_id             = static_cast<std::uint64_t>(run.hyperlink_id);
    diagnostic.rect_width               = run.rect.width();
    diagnostic.rect_height              = run.rect.height();
    diagnostic.font_point_size          = font.pointSizeF();
    diagnostic.font_pixel_size          = font.pixelSize();
    diagnostic.font_weight              = font.weight();
    diagnostic.clipped                  = clipped;
    diagnostic.force_blended_order      = force_blended_order;
    diagnostic.ascii_layout_font        = ascii_layout_font;
    diagnostic.line_has_text            = line_has_text;
    diagnostic.font_italic              = font.italic();
    diagnostic.font_family              = font.family();
    diagnostic.font_style_name          = font.styleName();
    const QFontInfo font_info(font);
    diagnostic.resolved_font_family     = font_info.family();
    diagnostic.resolved_font_style_name = font_info.styleName();
    diagnostic.text_preview_truncated =
        run.text.size() > k_slow_text_layout_preview_utf16_units;
    diagnostic.text_preview     = run.text.left(k_slow_text_layout_preview_utf16_units);
    diagnostic.codepoint_sample = text_codepoint_sample(run.text);
    classify_text(
        run.text,
        diagnostic.ascii_only,
        diagnostic.printable_ascii_only,
        diagnostic.has_control_codepoint);
    return diagnostic;
}
#endif

QColor qcolor_from_rgba(quint32 rgba)
{
    return QColor::fromRgba(rgba);
}

QColor resolved_color(
    const Terminal_color_ref&          color,
    const Terminal_color_state&        color_state,
    bool                               foreground)
{
    return qcolor_from_rgba(resolve_terminal_color_ref(color, color_state, foreground));
}

QRectF cell_rect(
    int                                row,
    int                                column,
    int                                column_count,
    terminal_cell_metrics_t            metrics)
{
    return
        QRectF(
            static_cast<qreal>(column) * metrics.width,
            static_cast<qreal>(row) * metrics.height,
            static_cast<qreal>(column_count) * metrics.width,
            metrics.height);
}

QColor effective_foreground(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return resolved_color(style.foreground, snapshot.color_state, true);
}

QColor effective_background(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return resolved_color(style.background, snapshot.color_state, false);
}

QColor faint_foreground_color(QColor color)
{
    color.setAlpha(std::clamp(
        static_cast<int>(std::round(
            static_cast<qreal>(color.alpha()) * k_faint_foreground_alpha_factor)),
        0,
        255));
    return color;
}

QColor default_terminal_background(
    const Terminal_render_snapshot& snapshot)
{
    const Terminal_text_style& default_style = snapshot.styles.empty()
        ? make_default_terminal_text_style()
        : snapshot.styles.front();
    return snapshot.modes.reverse_video
        ? effective_foreground(default_style, snapshot)
        : effective_background(default_style, snapshot);
}

bool style_is_inverse(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return terminal_style_has_attribute(style, Terminal_style_attribute::INVERSE) !=
        snapshot.modes.reverse_video;
}

struct render_style_attributes_t
{
    QColor foreground;
    QColor background;
    bool   underline = false;
    bool   strike    = false;
};

render_style_attributes_t render_style_attributes(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    QColor foreground = effective_foreground(style, snapshot);
    QColor background = effective_background(style, snapshot);
    if (style_is_inverse(style, snapshot)) {
        std::swap(foreground, background);
    }
    if (terminal_style_has_attribute(style, Terminal_style_attribute::FAINT)) {
        foreground = faint_foreground_color(foreground);
    }
    if (terminal_style_has_attribute(style, Terminal_style_attribute::INVISIBLE)) {
        foreground = background;
    }

    return {
        foreground,
        background,
        terminal_style_has_attribute(style, Terminal_style_attribute::UNDERLINE),
        terminal_style_has_attribute(style, Terminal_style_attribute::STRIKE),
    };
}

class Render_style_attribute_cache
{
public:
    explicit Render_style_attribute_cache(const Terminal_render_snapshot& snapshot)
    :
        m_snapshot(snapshot)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Render_style_attribute_cache::build");

        if (snapshot.styles.size() > k_eager_render_style_attribute_limit) {
            return;
        }

        m_attributes_by_index.reserve(snapshot.styles.size());
        for (const Terminal_text_style& style : snapshot.styles) {
            m_attributes_by_index.push_back(render_style_attributes(style, snapshot));
        }
    }

    const render_style_attributes_t& attributes(Terminal_style_id style_id)
    {
        if (!m_attributes_by_index.empty()) {
            return m_attributes_by_index[static_cast<std::size_t>(style_id)];
        }

        const auto found = m_attributes.find(style_id);
        if (found != m_attributes.end()) {
            return found->second;
        }

        const auto inserted = m_attributes.emplace(
            style_id,
            render_style_attributes(
                m_snapshot.styles[static_cast<std::size_t>(style_id)],
                m_snapshot));
        return inserted.first->second;
    }

private:
    const Terminal_render_snapshot&                        m_snapshot;
    std::vector<render_style_attributes_t>                 m_attributes_by_index;
    std::map<Terminal_style_id, render_style_attributes_t> m_attributes;
};

QRectF cursor_rect(
    const Terminal_render_cursor&  cursor,
    Terminal_cursor_shape          shape,
    terminal_cell_metrics_t        metrics)
{
    QRectF rect = cell_rect(cursor.position.row, cursor.position.column, 1, metrics);
    switch (shape) {
        case Terminal_cursor_shape::BLOCK:
            return rect;
        case Terminal_cursor_shape::BAR:
            rect.setWidth(std::max<qreal>(1.0, metrics.width * 0.18));
            return rect;
        case Terminal_cursor_shape::UNDERLINE:
            rect.setTop(rect.bottom() - std::max<qreal>(1.0, metrics.height * 0.16));
            return rect;
    }

    return rect;
}

QRectF decoration_rect(
    const Terminal_render_text_run&            run,
    Terminal_render_decoration_kind            kind,
    terminal_cell_metrics_t                    metrics)
{
    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    QRectF      rect      = run.rect;
    switch (kind) {
        case Terminal_render_decoration_kind::UNDERLINE:
        case Terminal_render_decoration_kind::HYPERLINK_UNDERLINE:
            rect.setTop(run.rect.top() + metrics.ascent + thickness);
            rect.setHeight(thickness);
            return rect;
        case Terminal_render_decoration_kind::STRIKE:
            rect.setTop(run.rect.top() + metrics.ascent * 0.55);
            rect.setHeight(thickness);
            return rect;
        case Terminal_render_decoration_kind::PREEDIT_CARET:
            rect.setWidth(thickness);
            return rect;
    }

    return rect;
}


int display_width_for_text(const QString& text)
{
    return std::max(0, measure_utf8_width(text.toUtf8()).cells);
}

int display_width_for_prefix(const QString& text, int code_unit_count)
{
    const int clamped_count = std::clamp(code_unit_count, 0, static_cast<int>(text.size()));
    return display_width_for_text(text.left(clamped_count));
}

bool position_inside_grid(
    terminal_grid_position_t       position,
    terminal_grid_size_t           grid_size)
{
    return
        position.row    >= 0              &&
        position.row    <  grid_size.rows &&
        position.column >= 0              &&
        position.column <  grid_size.columns;
}

int logical_row_for_viewport_row(
    const Terminal_viewport_state& viewport,
    int                            row)
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return row;
    }

    return viewport.scrollback_rows - viewport.offset_from_tail + row;
}

Terminal_render_line_provenance line_provenance_for_viewport_row(
    const Terminal_render_snapshot&    snapshot,
    int                                row,
    bool                               use_visible_line_provenance)
{
    if (!use_visible_line_provenance) {
        return {
            logical_row_for_viewport_row(snapshot.viewport, row),
            0U,
            0U,
        };
    }

    return snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
}

}

#if VNM_TERMINAL_PROFILING_ENABLED
void Terminal_text_layout_slow_diagnostics_recorder::reset()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_slow_call_count = 0U;
    m_samples.clear();
}

void Terminal_text_layout_slow_diagnostics_recorder::record_layout(
    std::uint64_t                      duration_ns,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    bool                               clipped,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    bool                               line_has_text)
{
    if (duration_ns < k_slow_text_layout_threshold_ns) {
        return;
    }

    terminal_text_layout_slow_diagnostic_t diagnostic = make_slow_text_layout_diagnostic(
        duration_ns,
        font,
        run,
        clipped,
        force_blended_order,
        ascii_layout_font,
        line_has_text);

    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_slow_call_count;
    if (m_samples.size() < k_max_slow_text_layout_samples) {
        m_samples.push_back(std::move(diagnostic));
    }
    else {
        const auto smallest = std::min_element(
            m_samples.begin(),
            m_samples.end(),
            [](const auto& left, const auto& right) {
                return left.duration_ns < right.duration_ns;
            });
        if (smallest               != m_samples.end() &&
            diagnostic.duration_ns >  smallest->duration_ns)
        {
            *smallest = std::move(diagnostic);
        }
    }

    std::sort(
        m_samples.begin(),
        m_samples.end(),
        [](const auto& left, const auto& right) {
            return left.duration_ns > right.duration_ns;
        });
}

terminal_text_layout_slow_diagnostics_t
Terminal_text_layout_slow_diagnostics_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return {
        k_slow_text_layout_threshold_ns,
        m_slow_call_count,
        m_samples,
    };
}
#endif

namespace {

std::uint64_t count_from_bool(bool value)
{
    return value ? 1U : 0U;
}

void accumulate_simple_content_stats(
    terminal_simple_content_cumulative_stats_t&    total,
    const terminal_simple_content_stats_t&         stats);

// Field-list X-macro (see accumulate_simple_content_stats for rationale).
// simple_content is a nested struct accumulated separately below.
#define VNM_RENDER_FRAME_STATS_FIELDS(X) \
    X(visible_rows) \
    X(dirty_rows) \
    X(full_dirty_rows) \
    X(cell_pass_input_cells) \
    X(cell_pass_classification_calls) \
    X(packed_pass_input_cells) \
    X(packed_pass_cells_scanned) \
    X(packed_pass_classification_calls) \
    X(packed_text_sidecars_enabled) \
    X(packed_text_sidecars_disabled) \
    X(packed_text_disabled_cells_skipped) \
    X(packed_cells_appended) \
    X(dirty_row_lookup_count) \
    X(cells_considered) \
    X(cells_skipped_invalid) \
    X(cells_skipped_wide_continuation) \
    X(cells_rendered) \
    X(text_cells_empty) \
    X(text_cells_rendered_as_text) \
    X(text_cells_printable_ascii) \
    X(text_cells_other_ascii) \
    X(text_cells_non_ascii) \
    X(text_cells_simple_ascii) \
    X(text_cells_single_width) \
    X(text_cells_multi_width) \
    X(text_cells_with_decorations) \
    X(text_cells_with_hyperlink) \
    X(compact_ascii_cells_seen) \
    X(compact_ascii_text_direct_appends) \
    X(compact_ascii_qstring_materializations) \
    X(text_style_changes) \
    X(text_distinct_styles) \
    X(background_rects_emitted) \
    X(selection_rects_emitted) \
    X(graphic_rects_emitted) \
    X(graphic_arcs_emitted) \
    X(text_runs_emitted) \
    X(cursor_text_runs_emitted) \
    X(decoration_rects_emitted) \
    X(cursor_rects_emitted) \
    X(overlay_rects_emitted) \
    X(packed_rows) \
    X(packed_text_spans) \
    X(packed_text_cells) \
    X(packed_text_ascii_direct_cells) \
    X(packed_text_ascii_direct_bytes) \
    X(packed_text_utf8_cells) \
    X(packed_text_utf8_input_units) \
    X(packed_text_utf8_output_bytes) \
    X(packed_payload_bytes) \
    /**/

void accumulate_frame_stats(
    terminal_render_frame_cumulative_stats_t&      total,
    const terminal_render_frame_stats_t&           stats)
{
    accumulate_simple_content_stats(total.simple_content, stats.simple_content);
#define VNM_ACCUMULATE_FIELD(field) \
    total.field += static_cast<std::uint64_t>(stats.field);
    VNM_RENDER_FRAME_STATS_FIELDS(VNM_ACCUMULATE_FIELD)
#undef VNM_ACCUMULATE_FIELD
}

#undef VNM_RENDER_FRAME_STATS_FIELDS

// Field-list X-macro (see accumulate_simple_content_stats for rationale).
// Fields needing non-trivial handling are excluded and written by hand in
// the function body.
#define VNM_RENDERER_STATS_FIELDS(X) \
    X(text_content_rebuilds) \
    X(text_content_reused) \
    X(text_content_removed) \
    X(text_content_failures) \
    X(atlas_work_created) \
    X(atlas_work_reused) \
    X(text_cache_entry_child_nodes_cleared_for_replacement) \
    X(text_cache_entry_child_nodes_cleared_for_removal) \
    X(route_fast_text_cells) \
    X(route_qt_text_layout_runs) \
    X(route_fallback_cells) \
    X(qt_text_layout_calls) \
    X(text_layout_runs_single_code_unit) \
    X(text_layout_runs_multi_code_unit) \
    X(text_layout_runs_all_space) \
    X(text_layout_runs_printable_ascii) \
    X(text_layout_runs_printable_ascii_with_space) \
    X(text_layout_runs_other_ascii) \
    X(text_layout_runs_non_ascii) \
    X(text_layout_runs_clipped) \
    X(text_layout_runs_ascii_layout_font) \
    X(text_layout_runs_force_blended_order) \
    X(text_layout_runs_with_hyperlink) \
    X(text_layout_runs_with_decoration) \
    X(text_layout_runs_mixed_ascii_non_ascii) \
    X(text_layout_runs_pure_non_ascii) \
    X(text_layout_runs_plain_unclipped) \
    X(text_layout_runs_plain_unclipped_ascii_font) \
    X(text_layout_runs_all_space_plain_unclipped) \
    X(text_layout_runs_printable_ascii_plain_unclipped) \
    X(text_layout_runs_non_ascii_plain_unclipped) \
    X(text_layout_runs_mixed_ascii_non_ascii_plain_unclipped) \
    X(text_layout_runs_pure_non_ascii_plain_unclipped) \
    X(text_layout_runs_fast_space_candidate) \
    X(text_layout_runs_fast_ascii_candidate) \
    X(text_layout_runs_fast_ascii_no_space_candidate) \
    X(text_layout_runs_fast_ascii_single_candidate) \
    X(text_layout_runs_fast_ascii_multi_candidate) \
    X(text_ascii_replacement_runs_screened) \
    X(text_ascii_replacement_runs_eligible) \
    X(text_ascii_replacement_runs_attempted) \
    X(text_ascii_replacement_runs_trusted_fast_path) \
    X(text_ascii_replacement_runs_succeeded) \
    X(text_ascii_replacement_runs_all_space_succeeded) \
    X(text_ascii_replacement_add_glyphs_calls) \
    X(text_ascii_replacement_runs_fallback) \
    X(text_ascii_replacement_runs_rejected_clipped) \
    X(text_ascii_replacement_runs_rejected_force_blended_order) \
    X(text_ascii_replacement_runs_rejected_decoration) \
    X(text_ascii_replacement_runs_rejected_hyperlink) \
    X(text_ascii_replacement_runs_rejected_non_printable_ascii) \
    X(text_ascii_replacement_runs_rejected_non_ascii) \
    X(text_ascii_replacement_runs_rejected_geometry) \
    X(text_ascii_replacement_runs_rejected_unsupported_font) \
    X(text_ascii_replacement_runs_rejected_internal_node) \
    X(text_ascii_replacement_runs_rejected_glyph_mapping) \
    X(text_layout_code_units) \
    X(text_layout_space_code_units) \
    X(text_layout_printable_ascii_code_units) \
    X(text_layout_other_ascii_code_units) \
    X(text_layout_non_ascii_code_units) \
    X(text_layout_plain_unclipped_code_units) \
    X(text_layout_all_space_plain_unclipped_code_units) \
    X(text_layout_printable_ascii_plain_unclipped_code_units) \
    X(text_layout_non_ascii_plain_unclipped_code_units) \
    X(text_layout_fast_space_candidate_code_units) \
    X(text_layout_fast_ascii_candidate_code_units) \
    X(text_ascii_replacement_code_units_screened) \
    X(text_ascii_replacement_code_units_eligible) \
    X(text_ascii_replacement_code_units_attempted) \
    X(text_ascii_replacement_code_units_trusted_fast_path) \
    X(text_ascii_replacement_code_units_succeeded) \
    X(text_ascii_replacement_code_units_fallback) \
    X(qsg_nodes_created) \
    X(qsg_nodes_replaced) \
    X(qsg_nodes_destroyed) \
    X(background_qsg_nodes_created) \
    X(background_qsg_nodes_replaced) \
    X(background_qsg_nodes_destroyed) \
    X(text_groups_considered) \
    X(text_groups_dirty) \
    X(text_groups_clean) \
    X(text_clean_reuse_skips) \
    X(text_resource_descriptor_builds) \
    X(text_resource_descriptor_builds_avoided) \
    X(text_resource_descriptor_reuses) \
    X(text_key_builds) \
    X(text_key_bytes) \
    X(rect_key_builds) \
    X(rect_key_bytes) \
    X(cache_key_builds) \
    X(cache_key_bytes) \
    X(text_dirty_row_ranges) \
    X(text_dirty_rows) \
    X(text_resource_dirty_row_probes) \
    X(text_runs_considered) \
    X(text_coalescing_candidate_groups) \
    X(text_coalescing_enabled_groups) \
    X(text_resource_rows_with_runs) \
    X(text_resource_runs_before_coalescing) \
    X(text_resource_runs_after_coalescing) \
    X(text_dirty_descriptor_identical_rows) \
    X(text_key_match_reuses) \
    X(text_dirty_rows_rebuilt) \
    X(text_clean_rows_rebuilt) \
    X(rect_resource_rects_before_coalescing) \
    X(rect_resource_rects_after_coalescing) \
    X(background_row_rects_before_coalescing) \
    X(background_row_rects_after_coalescing) \
    X(background_batched_rects) \
    X(background_batched_vertices) \
    X(selection_batched_rects) \
    X(selection_batched_vertices) \
    X(graphic_batched_rects) \
    X(graphic_batched_vertices) \
    X(decoration_batched_rects) \
    X(decoration_batched_vertices) \
    X(text_cache_entries_created) \
    X(text_cache_entries_replaced) \
    X(text_cache_entries_removed) \
    X(frame_background_rects) \
    X(frame_selection_rects) \
    X(frame_graphic_rects) \
    X(frame_graphic_arcs) \
    X(frame_text_runs) \
    X(frame_cursor_text_runs) \
    X(frame_decorations) \
    X(frame_cursors) \
    X(frame_overlay_rects) \
    X(frame_dirty_row_ranges) \
    X(frame_packed_rows) \
    X(frame_packed_text_spans) \
    X(frame_packed_text_cells) \
    X(frame_packed_payload_bytes) \
    X(row_cache_hits) \
    X(row_cache_clean_skips) \
    X(background_rows_rebuilt) \
    X(background_rows_reused) \
    X(background_row_clean_reuse_skips) \
    X(background_rows_removed) \
    X(background_row_cache_fallbacks) \
    X(selection_rows_rebuilt) \
    X(selection_rows_reused) \
    X(selection_row_clean_reuse_skips) \
    X(selection_rows_removed) \
    X(selection_row_cache_fallbacks) \
    X(decoration_rows_rebuilt) \
    X(decoration_rows_reused) \
    X(decoration_row_clean_reuse_skips) \
    X(decoration_rows_removed) \
    X(decoration_row_cache_fallbacks) \
    X(graphic_rect_rows_rebuilt) \
    X(graphic_rect_rows_reused) \
    X(graphic_rect_row_clean_reuse_skips) \
    X(graphic_rect_rows_removed) \
    X(graphic_rect_row_cache_fallbacks) \
    X(graphic_arc_rows_rebuilt) \
    X(graphic_arc_rows_reused) \
    X(graphic_arc_row_clean_reuse_skips) \
    X(graphic_arc_rows_removed) \
    X(graphic_arc_row_cache_fallbacks) \
    /**/

void accumulate_renderer_stats(
    terminal_renderer_cumulative_stats_t&  total,
    const terminal_renderer_stats_t&       stats)
{
    // Fields touched specially (different src/dst name, std::max, or a
    // nested sub-struct) are written by hand; the remaining pure
    // total.<field> += static_cast<std::uint64_t>(stats.<field>) fields are
    // generated from the X-macro below. Every field is written exactly once,
    // so the relative order of these two groups does not matter.
    ++total.frames_published;
    total.paint_completed_frames += count_from_bool(stats.paint_completed);
    total.root_reused_frames     += count_from_bool(stats.root_reused);
    accumulate_frame_stats(total.frame, stats.frame);
    total.text_cache_entry_max_child_nodes_cleared = std::max(
        total.text_cache_entry_max_child_nodes_cleared,
        static_cast<std::uint64_t>(stats.text_cache_entry_max_child_nodes_cleared));
    total.text_resource_max_runs_after_coalescing_per_row = std::max(
        total.text_resource_max_runs_after_coalescing_per_row,
        static_cast<std::uint64_t>(stats.text_resource_max_runs_after_coalescing_per_row));
    total.text_wrapper_order_rebuilds   += count_from_bool(stats.text_wrapper_order_rebuilt);
    total.background_layer_rebuilds     += count_from_bool(stats.background_layer_rebuilt);
    total.selection_layer_rebuilds      += count_from_bool(stats.selection_layer_rebuilt);
    total.graphic_layer_rebuilds        += count_from_bool(stats.graphic_layer_rebuilt);
    total.decoration_layer_rebuilds     += count_from_bool(stats.decoration_layer_rebuilt);
    total.cursor_layer_rebuilds         += count_from_bool(stats.cursor_layer_rebuilt);
    total.cursor_text_layer_rebuilds    += count_from_bool(stats.cursor_text_layer_rebuilt);
    total.overlay_layer_rebuilds        += count_from_bool(stats.overlay_layer_rebuilt);
#define VNM_ACCUMULATE_FIELD(field) \
    total.field += static_cast<std::uint64_t>(stats.field);
    VNM_RENDERER_STATS_FIELDS(VNM_ACCUMULATE_FIELD)
#undef VNM_ACCUMULATE_FIELD
}

#undef VNM_RENDERER_STATS_FIELDS

}

terminal_renderer_lifecycle_stats_t Terminal_renderer_lifecycle_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void Terminal_renderer_stats_publisher::publish(const terminal_renderer_stats_t& stats)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = stats;
    accumulate_renderer_stats(m_cumulative_stats, stats);
}

terminal_renderer_stats_t Terminal_renderer_stats_publisher::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

terminal_renderer_cumulative_stats_t
Terminal_renderer_stats_publisher::cumulative_snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_cumulative_stats;
}

namespace {

Terminal_simple_content_text_category simple_content_text_category(const QString& text)
{
    if (text.isEmpty()) {
        return Terminal_simple_content_text_category::EMPTY;
    }

    unsigned int outside_printable_ascii = 0U;
    unsigned int non_ascii               = 0U;
    const qsizetype text_size            = text.size();
    const ushort* code_units             = text.utf16();
    for (qsizetype index = 0; index < text_size; ++index) {
        const unsigned int code_unit = code_units[index];
        outside_printable_ascii |= static_cast<unsigned int>(
            code_unit - k_printable_ascii_first >
                k_printable_ascii_last - k_printable_ascii_first);
        non_ascii |= code_unit;
    }

    if (outside_printable_ascii == 0U) {
        return Terminal_simple_content_text_category::PRINTABLE_ASCII;
    }

    return (non_ascii & ~0x7fU) != 0U
        ? Terminal_simple_content_text_category::NON_ASCII
        : Terminal_simple_content_text_category::OTHER_ASCII;
}

Terminal_simple_content_text_category simple_content_text_category(
    Terminal_render_cell_text_category category)
{
    static_assert(
        static_cast<int>(Terminal_render_cell_text_category::EMPTY) ==
            static_cast<int>(Terminal_simple_content_text_category::EMPTY));
    static_assert(
        static_cast<int>(Terminal_render_cell_text_category::PRINTABLE_ASCII) ==
            static_cast<int>(Terminal_simple_content_text_category::PRINTABLE_ASCII));
    static_assert(
        static_cast<int>(Terminal_render_cell_text_category::OTHER_ASCII) ==
            static_cast<int>(Terminal_simple_content_text_category::OTHER_ASCII));
    static_assert(
        static_cast<int>(Terminal_render_cell_text_category::NON_ASCII) ==
            static_cast<int>(Terminal_simple_content_text_category::NON_ASCII));

    switch (category) {
        case Terminal_render_cell_text_category::EMPTY:
        case Terminal_render_cell_text_category::PRINTABLE_ASCII:
        case Terminal_render_cell_text_category::OTHER_ASCII:
        case Terminal_render_cell_text_category::NON_ASCII:
            return static_cast<Terminal_simple_content_text_category>(category);
        case Terminal_render_cell_text_category::UNKNOWN:
            break;
    }

    return Terminal_simple_content_text_category::EMPTY;
}

Terminal_simple_content_text_category simple_content_text_category(
    const Terminal_render_cell_text& text)
{
    return simple_content_text_category(text.category());
}

struct Simple_content_text_category_state
{
    Terminal_simple_content_text_category category =
        Terminal_simple_content_text_category::EMPTY;
    bool                                  cache_sound = true;
};

Simple_content_text_category_state simple_content_text_category_state(
    const Terminal_render_cell& cell)
{
    const Terminal_render_cell_text_category actual_render_category =
        cell.text.category();
    const Terminal_simple_content_text_category actual_category =
        simple_content_text_category(actual_render_category);
    if (cell.text_category != Terminal_render_cell_text_category::UNKNOWN &&
        cell.text_category == actual_render_category)
    {
        return {
            simple_content_text_category(cell.text_category),
            true,
        };
    }

    return {
        actual_category,
        cell.text_category == Terminal_render_cell_text_category::UNKNOWN,
    };
}

Terminal_simple_content_text_category simple_content_text_category(
    const Terminal_render_cell& cell)
{
    return simple_content_text_category_state(cell).category;
}

Terminal_simple_content_rejection_reason unrepresented_simple_cell_semantics_rejection(
    const Terminal_render_cell&    cell,
    bool                           has_decoration)
{
    if (cell.hyperlink_id != 0U) {
        return Terminal_simple_content_rejection_reason::HYPERLINK;
    }

    if (has_decoration) {
        return Terminal_simple_content_rejection_reason::DECORATION;
    }

    return Terminal_simple_content_rejection_reason::NONE;
}

bool strict_printable_ascii_classifier_bypass_eligible(
    const Terminal_render_cell&           cell,
    Terminal_simple_content_text_category text_category,
    bool                                  has_decoration)
{
    return
        text_category == Terminal_simple_content_text_category::PRINTABLE_ASCII &&
        cell.text.single_printable_ascii_code_unit().has_value() &&
        cell.display_width == 1                 &&
        !cell.wide_continuation                 &&
        cell.hyperlink_id == 0U                 &&
        !has_decoration;
}

} // namespace

terminal_simple_content_classification_t classify_terminal_simple_content_cell(
    const Terminal_render_cell&        cell,
    terminal_grid_size_t               grid_size,
    std::size_t                        style_count,
    bool                               has_decoration,
    bool                               dirty_row,
    Terminal_shaped_presentation_mode  presentation_mode)
{
    terminal_simple_content_classification_t classification;
    const Simple_content_text_category_state text_category_state =
        simple_content_text_category_state(cell);
    classification.text_category = text_category_state.category;
    classification.row           = cell.position.row;
    classification.style_id      = cell.style_id;
    classification.dirty_row     = dirty_row;

    const auto reject = [&](
        Terminal_simple_content_route route,
        Terminal_simple_content_rejection_reason reason) {
        classification.route            = route;
        classification.rejection_reason = reason;
        return classification;
    };

    if (grid_size.rows <= 0 || grid_size.columns <= 0) {
        return reject(
            Terminal_simple_content_route::FALLBACK,
            Terminal_simple_content_rejection_reason::INVALID_GRID);
    }

    if (!position_inside_grid(cell.position, grid_size)) {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_POSITION);
    }

    if (static_cast<std::size_t>(cell.style_id) >= style_count) {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_STYLE_ID);
    }

    if (cell.wide_continuation) {
        return reject(
            Terminal_simple_content_route::NONE,
            Terminal_simple_content_rejection_reason::WIDE_CONTINUATION);
    }

    if (cell.display_width <= 0 ||
        cell.display_width >  grid_size.columns - cell.position.column)
    {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH);
    }

    classification.valid_terminal_cell = true;
    if (cell.text.is_empty()) {
        return reject(
            Terminal_simple_content_route::NONE,
            Terminal_simple_content_rejection_reason::EMPTY_TEXT);
    }

    if (!text_category_state.cache_sound) {
        return reject(
            Terminal_simple_content_route::FALLBACK,
            Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
    }

    if (strict_printable_ascii_classifier_bypass_eligible(
            cell,
            classification.text_category,
            has_decoration))
    {
        classification.route = Terminal_simple_content_route::FAST_TEXT;
        classification.rejection_reason =
            Terminal_simple_content_rejection_reason::NONE;
        classification.fast_text_eligible = true;
        return classification;
    }

    const Terminal_simple_content_rejection_reason semantics_rejection =
        unrepresented_simple_cell_semantics_rejection(cell, has_decoration);

    if (cell.text.single_printable_ascii_code_unit().has_value()) {
        if (cell.display_width != 1) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH);
        }
    }
    else
    if (cell.text.is_inline_single_bmp()) {
        const std::optional<ushort> inline_code_unit = cell.text.single_bmp_code_unit();
        if (!inline_code_unit.has_value()) {
            return reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
        }

        const QString inline_text(1, QChar(*inline_code_unit));
        Terminal_cell_shaping_input shaping_input;
        shaping_input.column            = cell.position.column;
        shaping_input.text              = inline_text;
        shaping_input.display_width     = cell.display_width;
        shaping_input.wide_continuation = cell.wide_continuation;
        shaping_input.style_id          = cell.style_id;
        shaping_input.hyperlink_id      = cell.hyperlink_id;
        shaping_input.presentation_mode = presentation_mode;

        const Terminal_shaped_run_status scalar_status =
            validate_cell_text_scalars(shaping_input.text);
        if (scalar_status != Terminal_shaped_run_status::OK) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
        }

        const Terminal_shaped_run_status width_status =
            validate_cell_text_width(shaping_input);
        if (width_status != Terminal_shaped_run_status::OK) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH);
        }
    }
    else {
        const QString* fallback_text = cell.text.fallback_qstring_or_null();
        if (fallback_text == nullptr) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
        }

        Terminal_cell_shaping_input shaping_input;
        shaping_input.column            = cell.position.column;
        shaping_input.text              = *fallback_text;
        shaping_input.display_width     = cell.display_width;
        shaping_input.wide_continuation = cell.wide_continuation;
        shaping_input.style_id          = cell.style_id;
        shaping_input.hyperlink_id      = cell.hyperlink_id;
        shaping_input.presentation_mode = presentation_mode;

        const Terminal_shaped_run_status scalar_status =
            validate_cell_text_scalars(shaping_input.text);
        if (scalar_status != Terminal_shaped_run_status::OK) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
        }

        const Terminal_shaped_run_status width_status =
            validate_cell_text_width(shaping_input);
        if (width_status != Terminal_shaped_run_status::OK) {
            return
                reject(
                    Terminal_simple_content_route::QT_TEXT_LAYOUT,
                    Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH);
        }
    }

    if (cell.display_width != 1) {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT);
    }

    if (semantics_rejection != Terminal_simple_content_rejection_reason::NONE) {
        return reject(
            Terminal_simple_content_route::QT_TEXT_LAYOUT,
            semantics_rejection);
    }

    if (classification.text_category ==
        Terminal_simple_content_text_category::NON_ASCII)
    {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::NON_ASCII_TEXT);
    }

    if (classification.text_category !=
        Terminal_simple_content_text_category::PRINTABLE_ASCII)
    {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII);
    }

    classification.route = Terminal_simple_content_route::FAST_TEXT;
    classification.rejection_reason =
        Terminal_simple_content_rejection_reason::NONE;
    classification.fast_text_eligible = true;
    return classification;
}

namespace {

// Per-field accumulation is pure boilerplate: total.<field> +=
// static_cast<std::uint64_t>(stats.<field>) for every field. The X-macro
// below lists the field names ONCE so the accumulate body cannot drift from
// the struct. Field names are deliberately unchanged (a benchmark and tests
// read them). static_cast is a no-op for fields that are already uint64_t.
#define VNM_SIMPLE_CONTENT_STATS_FIELDS(X) \
    X(cells_considered) \
    X(eligible_cells) \
    X(eligible_after_all_gates_cells) \
    X(rows_with_eligible_cells) \
    X(styles_with_eligible_cells) \
    X(dirty_eligible_cells) \
    X(clean_eligible_cells) \
    X(text_category_empty_cells) \
    X(text_category_printable_ascii_cells) \
    X(text_category_other_ascii_cells) \
    X(text_category_non_ascii_cells) \
    X(route_none_cells) \
    X(route_fast_text_cells) \
    X(route_qt_text_layout_cells) \
    X(route_fallback_cells) \
    X(rejection_none_cells) \
    X(rejection_empty_text_cells) \
    X(rejection_invalid_grid_cells) \
    X(rejection_invalid_position_cells) \
    X(rejection_invalid_style_id_cells) \
    X(rejection_wide_continuation_cells) \
    X(rejection_invalid_display_width_cells) \
    X(rejection_invalid_text_encoding_cells) \
    X(rejection_invalid_text_width_cells) \
    X(rejection_multi_cell_text_cells) \
    X(rejection_non_printable_ascii_cells) \
    X(rejection_non_ascii_text_cells) \
    X(rejection_decoration_cells) \
    X(rejection_hyperlink_cells) \
    /**/

void accumulate_simple_content_stats(
    terminal_simple_content_cumulative_stats_t&    total,
    const terminal_simple_content_stats_t&         stats)
{
#define VNM_ACCUMULATE_FIELD(field) \
    total.field += static_cast<std::uint64_t>(stats.field);
    VNM_SIMPLE_CONTENT_STATS_FIELDS(VNM_ACCUMULATE_FIELD)
#undef VNM_ACCUMULATE_FIELD
}

#undef VNM_SIMPLE_CONTENT_STATS_FIELDS

void record_simple_content_text_category(
    terminal_simple_content_stats_t&       stats,
    Terminal_simple_content_text_category  category)
{
    switch (category) {
        case Terminal_simple_content_text_category::EMPTY:
            ++stats.text_category_empty_cells;
            return;
        case Terminal_simple_content_text_category::PRINTABLE_ASCII:
            ++stats.text_category_printable_ascii_cells;
            return;
        case Terminal_simple_content_text_category::OTHER_ASCII:
            ++stats.text_category_other_ascii_cells;
            return;
        case Terminal_simple_content_text_category::NON_ASCII:
            ++stats.text_category_non_ascii_cells;
            return;
    }
}

void record_simple_content_route(
    terminal_simple_content_stats_t&       stats,
    Terminal_simple_content_route          route)
{
    switch (route) {
        case Terminal_simple_content_route::NONE:
            ++stats.route_none_cells;
            return;
        case Terminal_simple_content_route::FAST_TEXT:
            ++stats.route_fast_text_cells;
            return;
        case Terminal_simple_content_route::QT_TEXT_LAYOUT:
            ++stats.route_qt_text_layout_cells;
            return;
        case Terminal_simple_content_route::FALLBACK:
            ++stats.route_fallback_cells;
            return;
    }
}

void record_simple_content_rejection(
    terminal_simple_content_stats_t&           stats,
    Terminal_simple_content_rejection_reason   reason)
{
    switch (reason) {
        case Terminal_simple_content_rejection_reason::NONE:
            ++stats.rejection_none_cells;
            return;
        case Terminal_simple_content_rejection_reason::EMPTY_TEXT:
            ++stats.rejection_empty_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_GRID:
            ++stats.rejection_invalid_grid_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_POSITION:
            ++stats.rejection_invalid_position_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_STYLE_ID:
            ++stats.rejection_invalid_style_id_cells;
            return;
        case Terminal_simple_content_rejection_reason::WIDE_CONTINUATION:
            ++stats.rejection_wide_continuation_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH:
            ++stats.rejection_invalid_display_width_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING:
            ++stats.rejection_invalid_text_encoding_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH:
            ++stats.rejection_invalid_text_width_cells;
            return;
        case Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT:
            ++stats.rejection_multi_cell_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII:
            ++stats.rejection_non_printable_ascii_cells;
            return;
        case Terminal_simple_content_rejection_reason::NON_ASCII_TEXT:
            ++stats.rejection_non_ascii_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::DECORATION:
            ++stats.rejection_decoration_cells;
            return;
        case Terminal_simple_content_rejection_reason::HYPERLINK:
            ++stats.rejection_hyperlink_cells;
            return;
    }
}

struct Simple_content_eligibility_flags
{
    std::vector<unsigned char> rows;
    std::vector<unsigned char> styles;
};

void record_simple_content_eligible_row_and_style(
    terminal_simple_content_stats_t&                stats,
    const terminal_simple_content_classification_t& classification,
    Simple_content_eligibility_flags&               flags)
{
    const std::size_t row_index   = static_cast<std::size_t>(classification.row);
    const std::size_t style_index = static_cast<std::size_t>(classification.style_id);
    if (row_index   >= flags.rows.size() ||
        style_index >= flags.styles.size())
    {
        Q_ASSERT(false);
        return;
    }

    if (flags.rows[row_index] == 0U) {
        flags.rows[row_index] = 1U;
        ++stats.rows_with_eligible_cells;
    }
    if (flags.styles[style_index] == 0U) {
        flags.styles[style_index] = 1U;
        ++stats.styles_with_eligible_cells;
    }
}

void record_simple_content_classification(
    terminal_simple_content_stats_t&                stats,
    const terminal_simple_content_classification_t& classification,
    bool                                            eligible_after_all_gates,
    Simple_content_eligibility_flags&               eligibility_flags)
{
    ++stats.cells_considered;
    record_simple_content_text_category(stats, classification.text_category);
    record_simple_content_route(stats, classification.route);
    record_simple_content_rejection(stats, classification.rejection_reason);

    if (!classification.fast_text_eligible) {
        return;
    }

    ++stats.eligible_cells;
    if (eligible_after_all_gates) {
        ++stats.eligible_after_all_gates_cells;
    }
    if (classification.dirty_row) {
        ++stats.dirty_eligible_cells;
    }
    else {
        ++stats.clean_eligible_cells;
    }

    record_simple_content_eligible_row_and_style(
        stats,
        classification,
        eligibility_flags);
}

bool cell_text_decoration_enabled(
    const Terminal_render_cell&    cell,
    std::size_t                    style_count,
    Render_style_attribute_cache&  style_attributes)
{
    if (cell.wide_continuation ||
        static_cast<std::size_t>(cell.style_id) >= style_count)
    {
        return false;
    }

    const render_style_attributes_t& style = style_attributes.attributes(cell.style_id);
    return style.underline || style.strike;
}

bool cell_intersects_grid_columns(
    const Terminal_render_cell&    cell,
    int                            first_column,
    int                            column_count)
{
    const int cell_end = cell.position.column + cell.display_width;
    const int span_end = first_column + column_count;
    return
        column_count         > 0        &&
        cell.position.column < span_end &&
        first_column         < cell_end;
}

bool block_cursor_covers_cell(
    const Terminal_render_cell&    cell,
    const Terminal_render_cursor&  cursor,
    bool                           block_cursor_visible)
{
    return
        block_cursor_visible                     &&
        cell.position.row == cursor.position.row &&
        cell_intersects_grid_columns(cell, cursor.position.column, 1);
}

bool ime_preedit_covers_cell(
    const Terminal_render_cell&    cell,
    bool                           ime_preedit_visible,
    int                            ime_preedit_row,
    int                            ime_preedit_column,
    int                            ime_preedit_columns)
{
    return
        ime_preedit_visible                  &&
        cell.position.row == ime_preedit_row &&
        cell_intersects_grid_columns(cell, ime_preedit_column, ime_preedit_columns);
}

struct Snapshot_dirty_row_flags
{
    int                        row_count = 0;
    bool                       all_rows_dirty = false;
    std::vector<unsigned char> row_flags;
};

Snapshot_dirty_row_flags build_snapshot_dirty_row_flags(
    const std::vector<Terminal_render_dirty_row_range>& dirty_row_ranges,
    int                                                row_count)
{
    if (row_count <= 0) {
        return {};
    }

    if (dirty_row_ranges.size() == 1U) {
        const Terminal_render_dirty_row_range& range = dirty_row_ranges.front();
        const std::int64_t range_first_row =
            static_cast<std::int64_t>(range.first_row);
        const std::int64_t range_end_row =
            range_first_row + static_cast<std::int64_t>(range.row_count);
        if (range.row_count > 0 &&
            range_first_row <= 0 &&
            range_end_row >= static_cast<std::int64_t>(row_count))
        {
            return {row_count, true, {}};
        }
    }

    Snapshot_dirty_row_flags dirty_row_flags;
    dirty_row_flags.row_count = row_count;
    dirty_row_flags.row_flags.resize(static_cast<std::size_t>(row_count), 0U);
    for (const Terminal_render_dirty_row_range& range : dirty_row_ranges) {
        if (range.row_count <= 0) {
            continue;
        }

        const std::int64_t first_row_value =
            std::max<std::int64_t>(static_cast<std::int64_t>(range.first_row), 0);
        const std::int64_t end_row_value =
            std::min<std::int64_t>(
                static_cast<std::int64_t>(range.first_row) +
                    static_cast<std::int64_t>(range.row_count),
                static_cast<std::int64_t>(row_count));
        if (end_row_value <= first_row_value) {
            continue;
        }

        const int first_row = static_cast<int>(first_row_value);
        const int end_row = static_cast<int>(end_row_value);
        for (int row = first_row; row < end_row; ++row) {
            dirty_row_flags.row_flags[static_cast<std::size_t>(row)] = 1U;
        }
    }
    return dirty_row_flags;
}

bool snapshot_row_is_dirty(
    const Snapshot_dirty_row_flags&    dirty_row_flags,
    int                                row)
{
    if (row < 0) {
        return false;
    }

    const std::size_t row_index = static_cast<std::size_t>(row);
    if (row_index >= static_cast<std::size_t>(dirty_row_flags.row_count)) {
        return false;
    }
    if (dirty_row_flags.all_rows_dirty) {
        return true;
    }

    return row_index < dirty_row_flags.row_flags.size() &&
        dirty_row_flags.row_flags[row_index] != 0U;
}

bool terminal_render_cell_column_less(
    const Terminal_render_cell*    left,
    const Terminal_render_cell*    right)
{
    return left->position.column < right->position.column;
}

void stable_sort_terminal_render_cell_row_by_column(
    std::vector<const Terminal_render_cell*>& row_cells)
{
    if (row_cells.size() <= 1U) {
        return;
    }

    std::stable_sort(
        row_cells.begin(),
        row_cells.end(),
        terminal_render_cell_column_less);
}

std::vector<std::vector<const Terminal_render_cell*>> build_explicit_snapshot_row_table(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0) {
        return {};
    }

    std::vector<std::vector<const Terminal_render_cell*>> row_table(
        static_cast<std::size_t>(snapshot.grid_size.rows));
    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row < 0 || cell.position.row >= snapshot.grid_size.rows) {
            continue;
        }
        row_table[static_cast<std::size_t>(cell.position.row)].push_back(&cell);
    }

    for (std::vector<const Terminal_render_cell*>& row_cells : row_table) {
        stable_sort_terminal_render_cell_row_by_column(row_cells);
    }

    return row_table;
}

std::uint32_t packed_color_rgba(const QColor& color)
{
    return static_cast<std::uint32_t>(color.rgba());
}

void append_packed_text_bytes(
    Terminal_render_frame&                   frame,
    terminal_packed_text_span_t&             span,
    const Terminal_render_cell_text&         text,
    Terminal_simple_content_text_category    text_category)
{
    if (text_category == Terminal_simple_content_text_category::PRINTABLE_ASCII) {
        const std::size_t byte_offset = frame.packed_text_bytes.size();
        const qsizetype text_size     = text.code_unit_count();
        frame.packed_text_bytes.resize(byte_offset + static_cast<std::size_t>(text_size));

        ++frame.stats.packed_text_ascii_direct_cells;
        frame.stats.packed_text_ascii_direct_bytes += static_cast<std::uint64_t>(text_size);
        char* bytes = frame.packed_text_bytes.data() + byte_offset;
        const std::optional<ushort> inline_code_unit =
            text.single_printable_ascii_code_unit();
        if (inline_code_unit.has_value()) {
            bytes[0] = static_cast<char>(*inline_code_unit);
        }
        else {
            const QString* fallback_text = text.fallback_qstring_or_null();
            Q_ASSERT(fallback_text != nullptr);
            const ushort* code_units = fallback_text->utf16();
            for (qsizetype index = 0; index < text_size; ++index) {
                bytes[index] = static_cast<char>(code_units[index]);
            }
        }
        span.text_length += static_cast<std::uint32_t>(text_size);
        return;
    }

    const QString* fallback_text = text.fallback_qstring_or_null();
    Q_ASSERT(fallback_text != nullptr);
    const QByteArray bytes = fallback_text->toUtf8();
    ++frame.stats.packed_text_utf8_cells;
    frame.stats.packed_text_utf8_input_units  +=
        static_cast<std::uint64_t>(fallback_text->size());
    frame.stats.packed_text_utf8_output_bytes += static_cast<std::uint64_t>(bytes.size());
    frame.packed_text_bytes.insert(
        frame.packed_text_bytes.end(),
        bytes.constData(),
        bytes.constData() + bytes.size());
    span.text_length += static_cast<std::uint32_t>(bytes.size());
}

void append_packed_text_cell(
    Terminal_render_frame&                   frame,
    terminal_packed_render_row_t&            row,
    const Terminal_render_cell&              cell,
    const render_style_attributes_t&         style,
    Terminal_simple_content_text_category    text_category)
{
    const std::uint32_t foreground_rgba = packed_color_rgba(style.foreground);
    const std::uint32_t background_rgba = packed_color_rgba(style.background);
    if (row.text_span_count > 0U) {
        terminal_packed_text_span_t& span =
            frame.packed_text_spans[static_cast<std::size_t>(
                row.first_text_span + row.text_span_count - 1U)];
        if (span.first_column + span.column_count == cell.position.column &&
            span.style_id                         == cell.style_id        &&
            span.foreground_rgba                  == foreground_rgba      &&
            span.background_rgba                  == background_rgba)
        {
            span.column_count += cell.display_width;
            append_packed_text_bytes(frame, span, cell.text, text_category);
            ++frame.stats.packed_text_cells;
            ++frame.stats.packed_cells_appended;
            return;
        }
    }

    if (row.text_span_count == 0U) {
        row.first_text_span =
            static_cast<std::uint32_t>(frame.packed_text_spans.size());
    }

    terminal_packed_text_span_t span;
    span.first_column    = cell.position.column;
    span.column_count    = cell.display_width;
    span.style_id        = cell.style_id;
    span.foreground_rgba = foreground_rgba;
    span.background_rgba = background_rgba;
    span.text_offset     = static_cast<std::uint32_t>(frame.packed_text_bytes.size());
    append_packed_text_bytes(frame, span, cell.text, text_category);
    frame.packed_text_spans.push_back(span);
    ++row.text_span_count;
    ++frame.stats.packed_text_cells;
    ++frame.stats.packed_cells_appended;
}

std::uint64_t packed_payload_byte_count(const Terminal_render_frame& frame)
{
    return
        static_cast<std::uint64_t>(frame.packed_rows.size()) * sizeof(terminal_packed_render_row_t)            +
        static_cast<std::uint64_t>(frame.packed_text_spans.size()) * sizeof(terminal_packed_text_span_t)       +
        static_cast<std::uint64_t>(frame.packed_text_bytes.size()) * sizeof(char);
}

void finalize_packed_render_frame_stats(Terminal_render_frame& frame)
{
    frame.stats.packed_rows          = static_cast<int>(frame.packed_rows.size());
    frame.stats.packed_text_spans    = static_cast<int>(frame.packed_text_spans.size());
    frame.stats.packed_payload_bytes = packed_payload_byte_count(frame);
}

void initialize_packed_render_frame_stats(
    Terminal_render_frame&             frame,
    const Terminal_render_snapshot&    snapshot,
    bool                               packed_text_sidecars_enabled)
{
    frame.stats.packed_pass_input_cells = packed_text_sidecars_enabled
        ? static_cast<int>(snapshot.cells.size())
        : 0;
    frame.stats.packed_text_sidecars_enabled =
        packed_text_sidecars_enabled ? 1 : 0;
    frame.stats.packed_text_sidecars_disabled =
        packed_text_sidecars_enabled ? 0 : 1;
}

terminal_packed_render_row_t& append_packed_render_row(
    Terminal_render_frame&             frame,
    const Terminal_render_snapshot&    snapshot,
    int                                row_index)
{
    terminal_packed_render_row_t row;
    row.active_buffer = snapshot.viewport.active_buffer;
    row.viewport_row  = row_index;
    row.logical_row   = logical_row_for_viewport_row(snapshot.viewport, row_index);
    // Span offsets are placeholders until the first span append for each kind.
    row.first_text_span =
        static_cast<std::uint32_t>(frame.packed_text_spans.size());
    frame.packed_rows.push_back(row);
    return frame.packed_rows.back();
}

void initialize_disabled_sidecar_packed_rows(
    Terminal_render_frame&             frame,
    const Terminal_render_snapshot&    snapshot)
{
    VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::disabled_sidecar_packed_rows");

    frame.packed_rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
    for (int row_index = 0; row_index < snapshot.grid_size.rows; ++row_index) {
        append_packed_render_row(frame, snapshot, row_index);
    }
}

void append_cell_text_to_text_run(
    Terminal_render_frame&           frame,
    Terminal_render_text_run&        run,
    const Terminal_render_cell_text& text)
{
    const std::optional<ushort> inline_code_unit =
        text.is_inline_printable_ascii()
            ? text.single_printable_ascii_code_unit()
            : std::nullopt;
    if (inline_code_unit.has_value()) {
        run.text += QChar(*inline_code_unit);
        ++frame.stats.compact_ascii_text_direct_appends;
        return;
    }

    text.append_to(run.text);
}

void build_terminal_render_frame_packed_data(
    Terminal_render_frame&             frame,
    const Terminal_render_snapshot&    snapshot,
    const Snapshot_dirty_row_flags&    dirty_row_flags,
    Render_style_attribute_cache&      style_attributes,
    bool                               block_cursor_visible,
    bool                               ime_preedit_visible,
    int                                ime_preedit_row,
    int                                ime_preedit_column,
    int                                ime_preedit_columns)
{
    VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data");

    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0) {
        return;
    }

    std::vector<std::vector<const Terminal_render_cell*>> row_table;
    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data::row_table");

        row_table = build_explicit_snapshot_row_table(snapshot);
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data::reserve");

        frame.packed_rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
        frame.packed_text_spans.reserve(snapshot.cells.size() / 2U);
        frame.packed_text_bytes.reserve(snapshot.cells.size());
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data::scan");

        for (int row_index = 0; row_index < snapshot.grid_size.rows; ++row_index) {
            terminal_packed_render_row_t& packed_row =
                append_packed_render_row(frame, snapshot, row_index);
            for (const Terminal_render_cell* cell : row_table[static_cast<std::size_t>(row_index)]) {
                ++frame.stats.packed_pass_cells_scanned;
                if (block_cursor_covers_cell(*cell, snapshot.cursor, block_cursor_visible) ||
                    ime_preedit_covers_cell(
                        *cell,
                        ime_preedit_visible,
                        ime_preedit_row,
                        ime_preedit_column,
                        ime_preedit_columns))
                {
                    continue;
                }

                ++frame.stats.dirty_row_lookup_count;
                const bool dirty_row = snapshot_row_is_dirty(
                    dirty_row_flags,
                    cell->position.row);
                const bool has_decoration = cell_text_decoration_enabled(
                    *cell,
                    snapshot.styles.size(),
                    style_attributes);
                ++frame.stats.packed_pass_classification_calls;
                const terminal_simple_content_classification_t classification =
                    classify_terminal_simple_content_cell(
                        *cell,
                        snapshot.grid_size,
                        snapshot.styles.size(),
                        has_decoration,
                        dirty_row);

                if (!classification.fast_text_eligible) {
                    continue;
                }

                append_packed_text_cell(
                    frame,
                    packed_row,
                    *cell,
                    style_attributes.attributes(cell->style_id),
                    classification.text_category);
            }
        }
    }
}

} // namespace

Terminal_render_frame build_terminal_render_frame(
    const Terminal_render_snapshot*    snapshot,
    QSizeF                             logical_size,
    terminal_cell_metrics_t            cell_metrics,
    const Terminal_render_options&     options,
    bool                               cursor_blink_visible,
    const Ime_preedit_state*           ime_preedit_override)
{
    VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame");

    const bool metrics_valid = is_valid_cell_metrics(cell_metrics);
    Terminal_render_frame frame;
    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::base_frame");

        frame.logical_size = logical_size;
        frame.cell_metrics = cell_metrics;
        frame.background_rects.reserve(
            snapshot != nullptr && metrics_valid
                ? bounded_frame_reserve(
                    snapshot->cells.size() + 1U,
                    snapshot->grid_size.rows,
                    8U,
                    64U)
                : 1U);
        frame.background_rects.push_back({
            QRectF(QPointF(0.0, 0.0), logical_size),
            options.default_background,
        });
        frame.stats.background_rects_emitted =
            static_cast<int>(frame.background_rects.size());
    }

    if (snapshot == nullptr || !metrics_valid) {
        return frame;
    }

    QColor grid_background;
    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::snapshot_setup");

        grid_background = default_terminal_background(*snapshot);
        frame.background_rects.front().color = grid_background;
        frame.grid_size        = snapshot->grid_size;
        frame.viewport         = snapshot->viewport;
        frame.dirty_row_ranges = snapshot->dirty_row_ranges;
        frame.stats.visible_rows          = snapshot->grid_size.rows;
        frame.stats.cell_pass_input_cells = static_cast<int>(snapshot->cells.size());
        for (const Terminal_render_dirty_row_range& range : snapshot->dirty_row_ranges) {
            frame.stats.dirty_rows += range.row_count;
        }
        if (frame.stats.dirty_rows == snapshot->grid_size.rows) {
            frame.stats.full_dirty_rows = frame.stats.dirty_rows;
        }
        initialize_packed_render_frame_stats(
            frame,
            *snapshot,
            options.packed_text_sidecars_enabled);
    }
    const bool valid_grid = snapshot->grid_size.rows > 0 && snapshot->grid_size.columns > 0;
    Snapshot_dirty_row_flags dirty_row_flags;
    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::dirty_row_flags");

        if (valid_grid) {
            dirty_row_flags = build_snapshot_dirty_row_flags(
                snapshot->dirty_row_ranges,
                snapshot->grid_size.rows);
        }
    }
    const Ime_preedit_state& ime_preedit =
        ime_preedit_override != nullptr ? *ime_preedit_override : snapshot->ime_preedit;
    const bool cursor_in_grid = valid_grid &&
        position_inside_grid(snapshot->cursor.position, snapshot->grid_size);
    const bool cursor_blink_enabled =
        options.cursor_blink_enabled_override.value_or(snapshot->cursor.blink_enabled);
    const bool cursor_visible =
        cursor_in_grid && snapshot->cursor.visible &&
        (!cursor_blink_enabled || cursor_blink_visible);
    const bool use_visible_line_provenance =
        render_snapshot_visible_line_provenance_is_valid(*snapshot);
    const Terminal_cursor_shape cursor_shape =
        options.cursor_shape_override.value_or(snapshot->cursor.shape);
    const bool block_cursor_visible =
        cursor_visible && cursor_shape == Terminal_cursor_shape::BLOCK;
    const QRectF block_cursor_rect = block_cursor_visible
        ? cursor_rect(snapshot->cursor, cursor_shape, cell_metrics)
        : QRectF();
    const bool ime_preedit_visible =
        valid_grid && ime_preedit.active && !ime_preedit.text.isEmpty();
    const int ime_preedit_row = ime_preedit_visible
        ? std::clamp(snapshot->cursor.position.row, 0, snapshot->grid_size.rows - 1)
        : -1;
    const int ime_preedit_column = ime_preedit_visible
        ? std::clamp(snapshot->cursor.position.column, 0, snapshot->grid_size.columns - 1)
        : -1;
    const int ime_preedit_columns = ime_preedit_visible
        ? std::clamp(
            display_width_for_text(ime_preedit.text),
            1,
            snapshot->grid_size.columns - ime_preedit_column)
        : 0;
    Render_style_attribute_cache style_attributes(*snapshot);

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::reserve_outputs");

        const std::size_t cell_count = snapshot->cells.size();
        frame.selection_rects.reserve(snapshot->selection_spans.size() + 1U);
        frame.graphic_rects.reserve(
            bounded_frame_reserve(cell_count, snapshot->grid_size.rows, 8U, 64U));
        frame.graphic_arcs.reserve(
            bounded_frame_reserve(cell_count, snapshot->grid_size.rows, 4U, 32U));
        frame.text_runs.reserve(
            bounded_frame_reserve(cell_count, snapshot->grid_size.rows, 64U, 512U));
        frame.decorations.reserve(
            bounded_frame_reserve(cell_count, snapshot->grid_size.rows, 4U, 64U));
        frame.cursors.reserve(2U);
        frame.overlay_rects.reserve(1U);
        if (!options.packed_text_sidecars_enabled) {
            if (valid_grid) {
                initialize_disabled_sidecar_packed_rows(frame, *snapshot);
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::cells");

        std::vector<unsigned char> seen_style_ids;
        Terminal_style_id          previous_style_id     = k_default_terminal_style_id;
        bool                       has_previous_style_id = false;

        Simple_content_eligibility_flags simple_eligibility_flags;
        {
            VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::cells::setup");

            seen_style_ids.resize(snapshot->styles.size(), 0U);
            simple_eligibility_flags.rows.resize(
                valid_grid ? static_cast<std::size_t>(snapshot->grid_size.rows) : 0U,
                0U);
            simple_eligibility_flags.styles.resize(
                valid_grid ? snapshot->styles.size() : 0U,
                0U);
        }

        int                             cached_row              = std::numeric_limits<int>::min();
        bool                            cached_valid_render_row = false;
        bool                            cached_dirty_row        = false;
        bool                            cached_block_cursor_row = false;
        bool                            cached_ime_preedit_row  = false;
        Terminal_render_line_provenance cached_line_provenance;
        auto update_cached_row_bookkeeping = [&](const Terminal_render_cell& cell) {
            cached_row = cell.position.row;
            cached_valid_render_row =
                valid_grid             &&
                cell.position.row >= 0 &&
                cell.position.row < snapshot->grid_size.rows;
            cached_dirty_row = snapshot_row_is_dirty(dirty_row_flags, cell.position.row);
            cached_block_cursor_row =
                block_cursor_visible &&
                cell.position.row == snapshot->cursor.position.row;
            cached_ime_preedit_row =
                ime_preedit_visible &&
                cell.position.row == ime_preedit_row;
            cached_line_provenance = cached_valid_render_row
                ? line_provenance_for_viewport_row(
                    *snapshot,
                    cell.position.row,
                    use_visible_line_provenance)
                : Terminal_render_line_provenance{};
        };
        // Open span for direct printable-ASCII text-run coalescing (see the text
        // emit branch below). Tracks the run currently being grown in
        // frame.text_runs so contiguous, single-width, plain printable-ASCII cells
        // with equivalent metadata are merged at emit time instead of one run per
        // cell. Gaps (empty/graphic cells, row changes) break the span implicitly
        // through the row/end_column contiguity test, so no explicit reset is
        // needed outside the emit branch.
        struct Open_ascii_coalesce_run
        {
            bool              valid              = false;
            std::size_t       index              = 0U;
            int               row                = 0;
            int               logical_row        = 0;
            std::uint64_t     retained_line_id   = 0U;
            std::uint64_t     content_generation = 0U;
            int               end_column         = 0;
            QRgb              foreground_rgba    = 0U;
            QRgb              background_rgba    = 0U;
            Terminal_style_id style_id           = k_default_terminal_style_id;
            int               length             = 0;
        };
        Open_ascii_coalesce_run open_ascii_run;
        for (const Terminal_render_cell& cell : snapshot->cells) {
            ++frame.stats.cells_considered;
            if (cell.position.row != cached_row) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "build_terminal_render_frame::cells::row_bookkeeping");

                update_cached_row_bookkeeping(cell);
            }

            const bool valid_style_id =
                static_cast<std::size_t>(cell.style_id) < snapshot->styles.size();
            const bool valid_render_row = cached_valid_render_row;
            const bool valid_render_cell =
                !cell.wide_continuation                                                  &&
                valid_render_row                                                          &&
                cell.position.column                    >= 0                              &&
                cell.position.column                    <  snapshot->grid_size.columns    &&
                cell.display_width                      >  0                              &&
                cell.display_width                      <= snapshot->grid_size.columns -
                                                           cell.position.column            &&
                valid_style_id;
            const render_style_attributes_t* style_or_null = valid_render_cell
                ? &style_attributes.attributes(cell.style_id)
                : nullptr;
            const bool classification_has_decoration =
                style_or_null != nullptr &&
                (style_or_null->underline || style_or_null->strike);
            // This legacy counter is intentionally per-cell for profile
            // comparability; the actual dirty-row lookup is cached per row.
            ++frame.stats.dirty_row_lookup_count;
            ++frame.stats.cell_pass_classification_calls;
            const terminal_simple_content_classification_t classification = classify_terminal_simple_content_cell(
                cell,
                snapshot->grid_size,
                snapshot->styles.size(),
                classification_has_decoration,
                cached_dirty_row);
            const bool block_cursor_cell =
                cached_block_cursor_row &&
                cell_intersects_grid_columns(cell, snapshot->cursor.position.column, 1);
            const bool ime_preedit_cell =
                cached_ime_preedit_row &&
                cell_intersects_grid_columns(cell, ime_preedit_column, ime_preedit_columns);
            const bool eligible_after_all_gates =
                classification.fast_text_eligible &&
                !block_cursor_cell                &&
                !ime_preedit_cell;
            record_simple_content_classification(
                frame.stats.simple_content,
                classification,
                eligible_after_all_gates,
                simple_eligibility_flags);

            if (cell.wide_continuation) {
                ++frame.stats.cells_skipped_wide_continuation;
                continue;
            }

            if (!valid_render_cell) {
                ++frame.stats.cells_skipped_invalid;
                continue;
            }

            ++frame.stats.cells_rendered;
            if (has_previous_style_id && previous_style_id != cell.style_id) {
                ++frame.stats.text_style_changes;
            }
            previous_style_id = cell.style_id;
            has_previous_style_id = true;

            const std::size_t style_index = static_cast<std::size_t>(cell.style_id);
            if (seen_style_ids[style_index] == 0U) {
                seen_style_ids[style_index] = 1U;
                ++frame.stats.text_distinct_styles;
            }

            const render_style_attributes_t& style = *style_or_null;
            const QColor foreground = style.foreground;
            const QColor background = style.background;

            const QRectF rect = cell_rect(
                cell.position.row,
                cell.position.column,
                cell.display_width,
                cell_metrics);
            if (background != grid_background) {
                frame.background_rects.push_back({rect, background});
            }

            const Terminal_render_line_provenance& line_provenance = cached_line_provenance;
            const bool text_is_empty = cell.text.is_empty();
            if (text_is_empty) {
                ++frame.stats.text_cells_empty;
            }
            else {
                if (cell.text.is_inline_printable_ascii()) {
                    ++frame.stats.compact_ascii_cells_seen;
                }

                if (cell.display_width == 1) {
                    ++frame.stats.text_cells_single_width;
                }
                else {
                    ++frame.stats.text_cells_multi_width;
                }

                if (classification.text_category ==
                    Terminal_simple_content_text_category::PRINTABLE_ASCII)
                {
                    ++frame.stats.text_cells_printable_ascii;
                    if (cell.display_width == 1) {
                        ++frame.stats.text_cells_simple_ascii;
                    }
                }
                else
                if (classification.text_category ==
                    Terminal_simple_content_text_category::NON_ASCII)
                {
                    ++frame.stats.text_cells_non_ascii;
                }
                else {
                    ++frame.stats.text_cells_other_ascii;
                }

                if (cell.hyperlink_id != 0U) {
                    ++frame.stats.text_cells_with_hyperlink;
                }
            }

            Terminal_render_text_run run;
            run.row                = cell.position.row;
            run.logical_row        = static_cast<int>(line_provenance.logical_row);
            run.retained_line_id   = line_provenance.retained_line_id;
            run.content_generation = line_provenance.content_generation;
            run.column             = cell.position.column;
            run.rect               = rect;
            run.baseline_origin    = QPointF(rect.left(), rect.top() + cell_metrics.ascent);
            run.foreground         = foreground;
            run.background         = background;
            run.style_id           = cell.style_id;
            run.hyperlink_id       = cell.hyperlink_id;
            run.underline          = style.underline;
            run.strike             = style.strike;
            const bool render_as_terminal_graphic =
                !text_is_empty &&
                !block_cursor_cell &&
                !ime_preedit_cell &&
                classification.rejection_reason ==
                    Terminal_simple_content_rejection_reason::NON_ASCII_TEXT &&
                is_terminal_graphic_text(cell.text) &&
                append_terminal_graphic_rects(
                    frame.graphic_rects,
                    frame.graphic_arcs,
                    rect,
                    cell.text,
                    foreground,
                    cell_metrics);
            if (render_as_terminal_graphic) {
                open_ascii_run.valid = false;
            }
            else
            if (!text_is_empty) {
                // Direct coalescing of printable-ASCII text runs. Merge this cell
                // into the open run when it is a contiguous, single-width, plain
                // (no decoration/hyperlink) printable-ASCII cell that shares the
                // open run's paint, style, and line provenance, capped at the
                // same maximum length the downstream coalescer uses. Cursor/IME
                // cells are kept standalone so block-cursor inverted text and
                // preedit overlays still copy a single protected cell.
                const std::optional<ushort> printable_ascii_code_unit =
                    cell.text.single_printable_ascii_code_unit();
                const bool coalescable_ascii_cell =
                    cell.display_width == 1                                    &&
                    classification.text_category ==
                        Terminal_simple_content_text_category::PRINTABLE_ASCII &&
                    printable_ascii_code_unit.has_value()                      &&
                    !run.underline                                            &&
                    !run.strike                                               &&
                    run.hyperlink_id == 0U                                    &&
                    !block_cursor_cell                                        &&
                    !ime_preedit_cell;
                const QRgb foreground_rgba = foreground.rgba();
                const QRgb background_rgba = background.rgba();
                if (coalescable_ascii_cell                                    &&
                    open_ascii_run.valid                                     &&
                    open_ascii_run.row                == cell.position.row    &&
                    open_ascii_run.logical_row        == run.logical_row      &&
                    open_ascii_run.retained_line_id   == run.retained_line_id &&
                    open_ascii_run.content_generation == run.content_generation &&
                    open_ascii_run.end_column         == cell.position.column &&
                    open_ascii_run.foreground_rgba    == foreground_rgba      &&
                    open_ascii_run.background_rgba    == background_rgba      &&
                    open_ascii_run.style_id           == cell.style_id        &&
                    open_ascii_run.length             <
                        static_cast<int>(k_max_coalesced_ascii_text_run_length))
                {
                    Terminal_render_text_run& open_run =
                        frame.text_runs[open_ascii_run.index];
                    open_run.text += QChar(*printable_ascii_code_unit);
                    if (cell.text.is_inline_printable_ascii()) {
                        ++frame.stats.compact_ascii_text_direct_appends;
                    }
                    open_run.rect.setWidth(
                        rect.left() + rect.width() - open_run.rect.left());
                    open_ascii_run.end_column += 1;
                    ++open_ascii_run.length;
                }
                else {
                    Terminal_render_text_run emitted_run = run;
                    emitted_run.text.reserve(
                        static_cast<qsizetype>(cell.text.code_unit_count()));
                    append_cell_text_to_text_run(frame, emitted_run, cell.text);
                    frame.text_runs.push_back(std::move(emitted_run));
                    if (coalescable_ascii_cell) {
                        open_ascii_run.valid              = true;
                        open_ascii_run.index              = frame.text_runs.size() - 1U;
                        open_ascii_run.row                = cell.position.row;
                        open_ascii_run.logical_row        = run.logical_row;
                        open_ascii_run.retained_line_id   = run.retained_line_id;
                        open_ascii_run.content_generation = run.content_generation;
                        open_ascii_run.end_column         = cell.position.column + 1;
                        open_ascii_run.foreground_rgba    = foreground_rgba;
                        open_ascii_run.background_rgba    = background_rgba;
                        open_ascii_run.style_id           = cell.style_id;
                        open_ascii_run.length             = 1;
                    }
                    else {
                        open_ascii_run.valid = false;
                    }
                }
                ++frame.stats.text_cells_rendered_as_text;
            }

            const bool has_decoration =
                run.underline                                           ||
                run.strike                                              ||
                (options.underline_hyperlinks && run.hyperlink_id != 0U);
            if (has_decoration) {
                ++frame.stats.text_cells_with_decorations;
            }
            if (run.underline) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::UNDERLINE,
                    decoration_rect(run, Terminal_render_decoration_kind::UNDERLINE, cell_metrics),
                    foreground,
                });
            }
            if (run.strike) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::STRIKE,
                    decoration_rect(run, Terminal_render_decoration_kind::STRIKE, cell_metrics),
                    foreground,
                });
            }
            if (options.underline_hyperlinks && run.hyperlink_id != 0U) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::HYPERLINK_UNDERLINE,
                    decoration_rect(
                        run,
                        Terminal_render_decoration_kind::HYPERLINK_UNDERLINE,
                        cell_metrics),
                    foreground,
                });
            }
        }
        if (!options.packed_text_sidecars_enabled) {
            VNM_TERMINAL_PROFILE_SCOPE(
                "build_terminal_render_frame::cells::finalize_disabled_sidecars");

            frame.stats.packed_text_disabled_cells_skipped =
                frame.stats.simple_content.eligible_after_all_gates_cells;
        }
    }

    if (options.packed_text_sidecars_enabled) {
        build_terminal_render_frame_packed_data(
            frame,
            *snapshot,
            dirty_row_flags,
            style_attributes,
            block_cursor_visible,
            ime_preedit_visible,
            ime_preedit_row,
            ime_preedit_column,
            ime_preedit_columns);
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data::finalize");

        finalize_packed_render_frame_stats(frame);
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::selection");

        for (const Terminal_render_selection_span& span : snapshot->selection_spans) {
            frame.selection_rects.push_back({
                cell_rect(span.row, span.first_column, span.column_count, cell_metrics),
                options.selection_background,
            });
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::cursor");

        if (cursor_visible) {
            const QRectF rendered_cursor_rect =
                cursor_rect(snapshot->cursor, cursor_shape, cell_metrics);
            frame.cursors.push_back({
                cursor_shape,
                rendered_cursor_rect,
                options.cursor_color,
            });
            if (cursor_shape == Terminal_cursor_shape::BLOCK) {
                const auto text_run = std::find_if(
                    frame.text_runs.begin(),
                    frame.text_runs.end(),
                    [&](const Terminal_render_text_run& run) {
                        return
                            run.row == snapshot->cursor.position.row  &&
                            run.rect.intersects(rendered_cursor_rect) &&
                            !run.text.isEmpty();
                    });
                if (text_run != frame.text_runs.end()) {
                    Terminal_render_text_run cursor_run = *text_run;
                    cursor_run.foreground = cursor_run.background;
                    cursor_run.clip_rect  = rendered_cursor_rect;
                    frame.cursor_text_runs.push_back(std::move(cursor_run));
                }
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::ime");

        if (valid_grid && ime_preedit.active && !ime_preedit.text.isEmpty()) {
            const int row =
                std::clamp(snapshot->cursor.position.row, 0, snapshot->grid_size.rows - 1);
            const int column =
                std::clamp(snapshot->cursor.position.column, 0, snapshot->grid_size.columns - 1);
            const int column_count = std::clamp(
                display_width_for_text(ime_preedit.text),
                1,
                snapshot->grid_size.columns - column);
            const QRectF rect = cell_rect(row, column, column_count, cell_metrics);
            frame.selection_rects.push_back({rect, options.preedit_background});

            const Terminal_render_line_provenance line_provenance =
                line_provenance_for_viewport_row(
                    *snapshot,
                    row,
                    use_visible_line_provenance);
            Terminal_render_text_run run;
            run.row                = row;
            run.logical_row        = static_cast<int>(line_provenance.logical_row);
            run.retained_line_id   = line_provenance.retained_line_id;
            run.content_generation = line_provenance.content_generation;
            run.column             = column;
            run.rect               = rect;
            run.baseline_origin    = QPointF(rect.left(), rect.top() + cell_metrics.ascent);
            run.text               = ime_preedit.text;
            run.foreground         = options.default_foreground;
            run.background         = options.preedit_background;
            frame.text_runs.push_back(run);

            const int caret_column = std::clamp(
                display_width_for_prefix(ime_preedit.text, ime_preedit.cursor_position),
                0,
                column_count);
            const int caret_grid_column =
                std::clamp(column + caret_column, 0, snapshot->grid_size.columns - 1);
            QRectF caret_rect = cell_rect(row, caret_grid_column, 1, cell_metrics);
            caret_rect.setWidth(std::max<qreal>(1.0, cell_metrics.width * 0.12));
            frame.decorations.push_back({
                Terminal_render_decoration_kind::PREEDIT_CARET,
                caret_rect,
                options.default_foreground,
            });
        }
    }

    if (snapshot->metadata.visual_bell_active && options.visual_bell_enabled) {
        frame.overlay_rects.push_back({
            QRectF(QPointF(0.0, 0.0), logical_size),
            options.visual_bell_color,
        });
    }

    frame.stats.background_rects_emitted = static_cast<int>(frame.background_rects.size());
    frame.stats.selection_rects_emitted  = static_cast<int>(frame.selection_rects.size());
    frame.stats.graphic_rects_emitted    = static_cast<int>(frame.graphic_rects.size());
    frame.stats.graphic_arcs_emitted     = static_cast<int>(frame.graphic_arcs.size());
    frame.stats.text_runs_emitted        = static_cast<int>(frame.text_runs.size());
    frame.stats.cursor_text_runs_emitted = static_cast<int>(frame.cursor_text_runs.size());
    frame.stats.decoration_rects_emitted = static_cast<int>(frame.decorations.size());
    frame.stats.cursor_rects_emitted     = static_cast<int>(frame.cursors.size());
    frame.stats.overlay_rects_emitted        = static_cast<int>(frame.overlay_rects.size());
    return frame;
}

}
