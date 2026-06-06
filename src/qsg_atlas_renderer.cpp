#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_atlas_warm_set.h"
#include "vnm_terminal/internal/terminal_graphic_geometry.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
#include <vnm_msdf_text/msdf_text.h>
#endif

#include <QElapsedTimer>
#include <QFile>
#include <QGlyphRun>
#include <QMatrix4x4>
#include <QSGRenderNode>
#include <QTextLayout>
#include <QTextOption>
#include <QThread>
#include <QTransform>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qrawfont_p.h>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr const char* k_atlas_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_quad.vert.qsb";
constexpr const char* k_atlas_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_quad.frag.qsb";
constexpr const char* k_atlas_glyph_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_glyph.vert.qsb";
constexpr const char* k_atlas_glyph_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_glyph.frag.qsb";
constexpr const char* k_atlas_dual_source_probe_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_dual_source_probe.frag.qsb";
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
constexpr const char* k_atlas_msdf_text_vertex_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_msdf_text.vert.qsb";
constexpr const char* k_atlas_msdf_text_fragment_shader_path =
    ":/vnm_terminal_surface/shaders/atlas_msdf_text.frag.qsb";
constexpr const char* k_atlas_msdf_terminal_font_resource =
    ":/vnm_terminal_surface/fonts/vnm_framework_monospace.ttf";
constexpr int k_atlas_msdf_text_texture_size = 2048;
constexpr double k_atlas_msdf_text_min_atlas_font_size = 48.0;
constexpr float k_atlas_msdf_text_atlas_px_range = 10.0f;
constexpr float k_atlas_msdf_text_sharpness_bias = 2.5f;
#endif
constexpr qreal k_no_wrap_text_line_width = 1024.0 * 1024.0;
constexpr int k_atlas_stencil_mask = 0xff;
constexpr qreal k_atlas_physical_origin_snap_epsilon = 0.001;
constexpr int k_atlas_neutral_channel_spread_tolerance = 3;
constexpr float k_atlas_gpu_kind_grayscale = 0.0f;
constexpr float k_atlas_gpu_kind_lcd_rgb   = 1.0f;
constexpr float k_atlas_gpu_kind_lcd_bgr   = 2.0f;
constexpr float k_atlas_gpu_kind_color     = 3.0f;
constexpr ushort k_atlas_printable_ascii_first = 0x20U;
constexpr ushort k_atlas_printable_ascii_last  = 0x7eU;
constexpr int k_atlas_printable_ascii_count =
    static_cast<int>(k_atlas_printable_ascii_last -
        k_atlas_printable_ascii_first + 1U);

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
namespace msdf_text = vnm::msdf_text;
using msdf_text_atlas_t  = msdf_text::atlas_t;
using msdf_text_glyph_t  = msdf_text::glyph_t;
using msdf_text_vertex_t = msdf_text::text_vertex_t;

static_assert(sizeof(msdf_text_atlas_t) > 0);
static_assert(sizeof(msdf_text_glyph_t) > 0);
static_assert(sizeof(msdf_text_vertex_t) > 0);
#endif

struct atlas_vertex_t
{
    float x = 0.0f;
    float y = 0.0f;
};

struct atlas_instance_t
{
    float rect[4]  = {};
    float color[4] = {};
};

struct atlas_glyph_instance_t
{
    float rect[4]       = {};
    float uv_rect[4]    = {};
    float color[4]      = {};
    float atlas_info[4] = {};
};

struct atlas_msdf_instance_t
{
    float rect[4]      = {};
    float uv_rect[4]   = {};
    float color[4]     = {};
    float uv_bounds[4] = {};
    float frame_rect[4] = {};
};

struct atlas_uniform_t
{
    float matrix[16] = {};
};

struct atlas_msdf_uniform_t
{
    float matrix[16]    = {};
    float px_range      = 0.0f;
    float target_width  = 1.0f;
    float target_height = 1.0f;
    float reserved0     = 0.0f;
};

static_assert(offsetof(atlas_msdf_uniform_t, matrix)        == 0);
static_assert(offsetof(atlas_msdf_uniform_t, px_range)      == 64);
static_assert(offsetof(atlas_msdf_uniform_t, target_width)  == 68);
static_assert(offsetof(atlas_msdf_uniform_t, target_height) == 72);
static_assert(sizeof(atlas_msdf_uniform_t)                  == 80);

struct atlas_pass_range_t
{
    quint32 first = 0U;
    quint32 count = 0U;

    bool has_instances() const { return count > 0U; }
};

struct atlas_text_pass_ranges_t
{
    atlas_pass_range_t glyph;
    atlas_pass_range_t msdf;
};

struct Atlas_prepare_result
{
    bool                            raw_font_rasterized = false;
    std::uint64_t                   raster_thread       = 0U;
    int                             rasterized_glyphs   = 0;
    QString                         base_face_id;
    std::set<QString>               glyph_face_ids;
    std::set<QString>               fallback_face_ids;
    Qsg_atlas_frame_build_summary frame_build;
    Qsg_atlas_render_summary      render;
    Qsg_atlas_producer_summary    producer;
    Qsg_atlas_warm_lazy_summary   warm_lazy;
};

enum class Atlas_cache_insert_source
{
    WARM,
    VISIBLE_LAZY,
};

struct Atlas_frame_state_keys
{
    QByteArray selection;
    QByteArray cursor;
    QByteArray preedit;
    QByteArray options;
    QByteArray visual_bell;
};

struct Prepared_atlas_glyph
{
    Qsg_atlas_shaped_glyph_record record;
    Glyph_image_presentation      presentation = Glyph_image_presentation::UNKNOWN;
    Glyph_atlas_slot              slot;
};

struct Prepared_atlas_text_run
{
    QPointF                       baseline_origin;
    std::vector<Prepared_atlas_glyph>
                                  glyphs;
    std::uint64_t                 last_seen_frame = 0U;
    bool                          emoji_presentation_run = false;
};

struct Simple_atlas_glyph_template
{
    Qsg_atlas_shaped_glyph_record record;
    Glyph_atlas_slot              slot;
    bool                          drawable = false;
};

struct Simple_atlas_text_cache
{
    std::uint64_t font_epoch = 0U;
    QString       font_key;
    qreal         device_pixel_ratio = 1.0;
    terminal_cell_metrics_t
                  cell_metrics;
    bool          initialized = false;
    bool          usable      = false;
    std::array<Simple_atlas_glyph_template, k_atlas_printable_ascii_count>
                  glyphs;
};

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
struct Msdf_terminal_text_cache
{
    std::uint64_t font_epoch = 0U;
    std::uint64_t generation = 0U;
    qreal         device_pixel_ratio = 1.0;
    terminal_cell_metrics_t
                  cell_metrics;
    int           pixel_height = 0;
    int           font_data_bytes = 0;
    bool          initialized = false;
    bool          atlas_built = false;
    bool          ready       = false;
    QString       message;
    msdf_text_atlas_t atlas;
};
#endif

struct Atlas_warm_key
{
    std::uint64_t           font_epoch = 0U;
    QString                 font_key;
    qreal                   device_pixel_ratio = 1.0;
    terminal_cell_metrics_t cell_metrics;
    bool                    valid = false;
};

const std::array<atlas_vertex_t, 6> k_atlas_quad_vertices = {{
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

QRhiGraphicsPipeline::TargetBlend atlas_blend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

QRhiGraphicsPipeline::TargetBlend atlas_msdf_text_blend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

QRhiGraphicsPipeline::StencilOpState atlas_stencil_state()
{
    QRhiGraphicsPipeline::StencilOpState state;
    state.failOp      = QRhiGraphicsPipeline::Keep;
    state.depthFailOp = QRhiGraphicsPipeline::Keep;
    state.passOp      = QRhiGraphicsPipeline::Keep;
    state.compareOp   = QRhiGraphicsPipeline::Equal;
    return state;
}

QRhiVertexInputLayout atlas_vertex_t_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(atlas_vertex_t)),
        QRhiVertexInputBinding(
            sizeof(atlas_instance_t),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(atlas_vertex_t, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_instance_t, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_instance_t, color)),
    });
    return layout;
}

QRhiVertexInputLayout atlas_glyph_vertex_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(atlas_vertex_t)),
        QRhiVertexInputBinding(
            sizeof(atlas_glyph_instance_t),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(atlas_vertex_t, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_glyph_instance_t, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_glyph_instance_t, uv_rect)),
        QRhiVertexInputAttribute(
            1,
            3,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_glyph_instance_t, color)),
        QRhiVertexInputAttribute(
            1,
            4,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_glyph_instance_t, atlas_info)),
    });
    return layout;
}

QRhiVertexInputLayout atlas_msdf_text_vertex_input_layout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({
        QRhiVertexInputBinding(sizeof(atlas_vertex_t)),
        QRhiVertexInputBinding(
            sizeof(atlas_msdf_instance_t),
            QRhiVertexInputBinding::PerInstance),
    });
    layout.setAttributes({
        QRhiVertexInputAttribute(
            0,
            0,
            QRhiVertexInputAttribute::Float2,
            offsetof(atlas_vertex_t, x)),
        QRhiVertexInputAttribute(
            1,
            1,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_msdf_instance_t, rect)),
        QRhiVertexInputAttribute(
            1,
            2,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_msdf_instance_t, uv_rect)),
        QRhiVertexInputAttribute(
            1,
            3,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_msdf_instance_t, color)),
        QRhiVertexInputAttribute(
            1,
            4,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_msdf_instance_t, uv_bounds)),
        QRhiVertexInputAttribute(
            1,
            5,
            QRhiVertexInputAttribute::Float4,
            offsetof(atlas_msdf_instance_t, frame_rect)),
    });
    return layout;
}

std::array<float, 4> atlas_color_components(QColor color, qreal opacity)
{
    const qreal alpha_ratio = std::clamp(color.alphaF() * opacity, 0.0, 1.0);
    return {
        static_cast<float>(
            std::round(static_cast<qreal>(color.red())   * alpha_ratio) / 255.0),
        static_cast<float>(
            std::round(static_cast<qreal>(color.green()) * alpha_ratio) / 255.0),
        static_cast<float>(
            std::round(static_cast<qreal>(color.blue())  * alpha_ratio) / 255.0),
        static_cast<float>(std::round(alpha_ratio * 255.0) / 255.0),
    };
}

std::array<float, 4> atlas_glyph_color_components(QColor color, qreal opacity)
{
    const qreal alpha_ratio = std::clamp(color.alphaF() * opacity, 0.0, 1.0);
    return {
        static_cast<float>(color.redF()),
        static_cast<float>(color.greenF()),
        static_cast<float>(color.blueF()),
        static_cast<float>(std::round(alpha_ratio * 255.0) / 255.0),
    };
}

void store_color(float* target, const std::array<float, 4>& color)
{
    std::copy(color.begin(), color.end(), target);
}

void store_color(float* target, QColor color, qreal opacity)
{
    store_color(target, atlas_color_components(color, opacity));
}

qreal atlas_logical_pixel_size(qreal device_pixel_ratio)
{
    return 1.0 / std::max<qreal>(1.0, device_pixel_ratio);
}

qreal atlas_antialiased_rect_pixel_coverage(
    const Terminal_render_rect& rect,
    QPointF                     point)
{
    const QRectF& shape = rect.rect;
    if (shape.width() >= shape.height()) {
        if (point.x() < shape.left() || point.x() > shape.right()) {
            return 0.0;
        }

        const qreal distance = std::abs(point.y() - shape.center().y());
        return std::clamp(
            shape.height() * 0.5 + k_terminal_graphic_antialias_feather - distance,
            0.0,
            1.0);
    }

    if (point.y() < shape.top() || point.y() > shape.bottom()) {
        return 0.0;
    }

    const qreal distance = std::abs(point.x() - shape.center().x());
    return std::clamp(
        shape.width() * 0.5 + k_terminal_graphic_antialias_feather - distance,
        0.0,
        1.0);
}

bool dirty_range_covers_full_grid(const Terminal_render_frame& frame)
{
    return
        frame.grid_size.rows > 0 &&
        frame.dirty_row_ranges.size() == 1U &&
        frame.dirty_row_ranges.front().first_row == 0 &&
        frame.dirty_row_ranges.front().row_count >= frame.grid_size.rows;
}

struct atlas_dirty_row_summary_t
{
    std::vector<unsigned char> rows;
    int                        dirty_rows = 0;
    bool                       full_grid  = false;
};

atlas_dirty_row_summary_t atlas_dirty_rows(
    const std::vector<Terminal_render_dirty_row_range>& ranges,
    int                                                 row_count)
{
    atlas_dirty_row_summary_t summary;
    if (row_count <= 0) {
        return summary;
    }

    summary.rows.assign(static_cast<std::size_t>(row_count), 0U);
    for (const Terminal_render_dirty_row_range& range : ranges) {
        const int first = std::clamp(range.first_row, 0, row_count);
        const int last = std::clamp(
            range.first_row + range.row_count,
            0,
            row_count);
        for (int row = first; row < last; ++row) {
            unsigned char& dirty = summary.rows[static_cast<std::size_t>(row)];
            if (dirty == 0U) {
                dirty = 1U;
                ++summary.dirty_rows;
            }
        }
    }
    summary.full_grid = summary.dirty_rows == row_count && row_count > 0;
    return summary;
}

bool atlas_row_is_dirty(const atlas_dirty_row_summary_t& dirty_rows, int row)
{
    if (row == k_qsg_atlas_all_rows) {
        return dirty_rows.full_grid;
    }
    if (row < 0 || row >= static_cast<int>(dirty_rows.rows.size())) {
        return false;
    }
    return dirty_rows.rows[static_cast<std::size_t>(row)] != 0U;
}

void append_key_bytes(QByteArray& key, const char* bytes, int byte_count)
{
    key.append(bytes, byte_count);
}

void append_key_int(QByteArray& key, int value)
{
    append_key_bytes(
        key,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
}

void append_key_uint64(QByteArray& key, std::uint64_t value)
{
    append_key_bytes(
        key,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
}

void append_key_qreal(QByteArray& key, qreal value)
{
    const double stored = static_cast<double>(value);
    append_key_bytes(
        key,
        reinterpret_cast<const char*>(&stored),
        static_cast<int>(sizeof(stored)));
}

void append_key_bool(QByteArray& key, bool value)
{
    append_key_int(key, value ? 1 : 0);
}

void append_key_color(QByteArray& key, const QColor& color)
{
    append_key_uint64(key, static_cast<std::uint64_t>(color.rgba()));
}

void append_key_string(QByteArray& key, const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    append_key_int(key, utf8.size());
    key.append(utf8);
}

void append_key_rect(QByteArray& key, const QRectF& rect)
{
    append_key_qreal(key, rect.x());
    append_key_qreal(key, rect.y());
    append_key_qreal(key, rect.width());
    append_key_qreal(key, rect.height());
}

void append_key_render_rect(
    QByteArray&                 key,
    const Terminal_render_rect& rect)
{
    append_key_rect(key, rect.rect);
    append_key_color(key, rect.color);
    append_key_bool(key, rect.antialias);
}

void append_key_arc(QByteArray& key, const Terminal_render_arc& arc)
{
    append_key_int(key, static_cast<int>(arc.kind));
    append_key_rect(key, arc.rect);
    append_key_color(key, arc.color);
    append_key_qreal(key, arc.stroke);
}

void append_key_text_run(QByteArray& key, const Terminal_render_text_run& run)
{
    append_key_int(key, run.row);
    append_key_int(key, run.logical_row);
    append_key_uint64(key, run.retained_line_id);
    append_key_uint64(key, run.content_generation);
    append_key_int(key, run.column);
    append_key_rect(key, run.rect);
    append_key_rect(key, run.clip_rect);
    append_key_qreal(key, run.baseline_origin.x());
    append_key_qreal(key, run.baseline_origin.y());
    append_key_string(key, run.text);
    append_key_color(key, run.foreground);
    append_key_color(key, run.background);
    append_key_uint64(key, run.style_id);
    append_key_uint64(key, run.hyperlink_id);
    append_key_bool(key, run.underline);
    append_key_bool(key, run.strike);
}

void append_key_cell_metrics(
    QByteArray&                key,
    terminal_cell_metrics_t    metrics)
{
    append_key_qreal(key, metrics.width);
    append_key_qreal(key, metrics.height);
    append_key_qreal(key, metrics.ascent);
    append_key_qreal(key, metrics.descent);
}

bool qsg_atlas_text_is_ascii(const QString& text)
{
    for (const QChar ch : text) {
        if (ch.unicode() > 0x7fU) {
            return false;
        }
    }
    return true;
}

bool qsg_atlas_text_is_printable_ascii(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }

    for (QChar code_unit : text) {
        const ushort value = code_unit.unicode();
        if (value < k_atlas_printable_ascii_first ||
            value > k_atlas_printable_ascii_last)
        {
            return false;
        }
    }

    return true;
}

int qsg_atlas_printable_ascii_index(QChar code_unit)
{
    const ushort value = code_unit.unicode();
    if (value < k_atlas_printable_ascii_first ||
        value > k_atlas_printable_ascii_last)
    {
        return -1;
    }

    return static_cast<int>(value - k_atlas_printable_ascii_first);
}

QString qsg_atlas_printable_ascii_probe_text()
{
    QString text;
    text.reserve(k_atlas_printable_ascii_count);
    for (ushort value = k_atlas_printable_ascii_first;
        value <= k_atlas_printable_ascii_last;
        ++value)
    {
        text.append(QChar(value));
    }
    return text;
}

QString qsg_atlas_printable_ascii_stability_probe_text()
{
    QString text = qsg_atlas_printable_ascii_probe_text();
    text.reserve(
        text.size() +
        QStringLiteral("==!=->=><=>=::///www").size());
    text.append(QStringLiteral("==!=->=><=>=::///www"));
    return text;
}

int qsg_atlas_printable_ascii_drawable_glyph_count(const QString& text)
{
    int count = 0;
    for (QChar code_unit : text) {
        if (code_unit.unicode() != 0x20U) {
            ++count;
        }
    }
    return count;
}

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
msdf_text::options_t atlas_msdf_text_options()
{
    msdf_text::options_t options;
    options.atlas_size           = k_atlas_msdf_text_texture_size;
    options.min_atlas_font_size  = k_atlas_msdf_text_min_atlas_font_size;
    options.atlas_px_range       = k_atlas_msdf_text_atlas_px_range;
    options.sharpness_bias       = k_atlas_msdf_text_sharpness_bias;
    options.build_kerning_table  = false;
    return options;
}

const std::vector<char32_t>& atlas_msdf_text_codepoints()
{
    static const std::vector<char32_t> codepoints = [] {
        std::vector<char32_t> values;
        values.reserve(k_atlas_printable_ascii_count);
        for (ushort value = k_atlas_printable_ascii_first;
            value <= k_atlas_printable_ascii_last;
            ++value)
        {
            values.push_back(static_cast<char32_t>(value));
        }
        return values;
    }();
    return codepoints;
}

bool atlas_msdf_text_uv_is_valid(float value)
{
    constexpr float k_uv_slop = 0.001f;
    return std::isfinite(value) &&
        value >= -k_uv_slop &&
        value <= 1.0f + k_uv_slop;
}

bool atlas_msdf_text_glyph_is_drawable(const msdf_text_glyph_t& glyph)
{
    return
        std::isfinite(glyph.plane_left)   &&
        std::isfinite(glyph.plane_right)  &&
        std::isfinite(glyph.plane_top)    &&
        std::isfinite(glyph.plane_bottom) &&
        glyph.plane_right > glyph.plane_left &&
        glyph.plane_top > glyph.plane_bottom &&
        atlas_msdf_text_uv_is_valid(glyph.uv_left)   &&
        atlas_msdf_text_uv_is_valid(glyph.uv_right)  &&
        atlas_msdf_text_uv_is_valid(glyph.uv_top)    &&
        atlas_msdf_text_uv_is_valid(glyph.uv_bottom) &&
        glyph.uv_right >= glyph.uv_left &&
        glyph.uv_bottom >= glyph.uv_top;
}

bool atlas_msdf_text_atlas_has_printable_ascii(
    const msdf_text_atlas_t& atlas,
    QString*                 out_message)
{
    for (ushort value = k_atlas_printable_ascii_first;
        value <= k_atlas_printable_ascii_last;
        ++value)
    {
        const char32_t codepoint = static_cast<char32_t>(value);
        const auto glyph = atlas.glyphs.find(codepoint);
        if (glyph == atlas.glyphs.end()) {
            if (out_message != nullptr) {
                *out_message = QStringLiteral(
                    "MSDF atlas is missing printable ASCII codepoint %1")
                    .arg(static_cast<int>(value));
            }
            return false;
        }

        if (value != 0x20U &&
            !atlas_msdf_text_glyph_is_drawable(glyph->second))
        {
            if (out_message != nullptr) {
                *out_message = QStringLiteral(
                    "MSDF atlas has an invalid drawable glyph for codepoint %1")
                    .arg(static_cast<int>(value));
            }
            return false;
        }
    }

    return true;
}

#endif

bool qsg_atlas_cell_metric_equal(qreal left, qreal right)
{
    return std::abs(left - right) <= 0.001;
}

bool qsg_atlas_cell_metrics_equal(
    terminal_cell_metrics_t left,
    terminal_cell_metrics_t right)
{
    return
        qsg_atlas_cell_metric_equal(left.width, right.width)     &&
        qsg_atlas_cell_metric_equal(left.height, right.height)   &&
        qsg_atlas_cell_metric_equal(left.ascent, right.ascent)   &&
        qsg_atlas_cell_metric_equal(left.descent, right.descent);
}

QString qsg_atlas_warm_seed_qstring(const qsg_atlas_warm_seed_string_t& seed)
{
    return QString::fromUtf16(
        seed.text.data(),
        static_cast<qsizetype>(seed.text.size()));
}

bool qsg_atlas_warm_seed_code_unit_is_non_rendering(QChar ch)
{
    if (ch.isSpace()) {
        return true;
    }

    switch (ch.category()) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
        case QChar::Other_Format:
        case QChar::Other_Control:
            return true;
        default:
            break;
    }
    return false;
}

bool qsg_atlas_warm_seed_source_range_is_non_rendering(
    const QString& text,
    qsizetype      source_start,
    qsizetype      source_end)
{
    if (source_start < 0 || source_start >= text.size()) {
        return false;
    }

    const qsizetype bounded_end =
        std::clamp(source_end, source_start + 1, text.size());
    for (qsizetype index = source_start; index < bounded_end; ++index) {
        if (!qsg_atlas_warm_seed_code_unit_is_non_rendering(text.at(index))) {
            return false;
        }
    }
    return true;
}

bool atlas_warm_key_matches(
    const Atlas_warm_key&      key,
    std::uint64_t              font_epoch,
    const QString&             font_key,
    qreal                      device_pixel_ratio,
    terminal_cell_metrics_t    cell_metrics)
{
    return
        key.valid &&
        key.font_epoch == font_epoch &&
        key.font_key == font_key &&
        qsg_atlas_cell_metric_equal(
            key.device_pixel_ratio,
            device_pixel_ratio) &&
        qsg_atlas_cell_metrics_equal(key.cell_metrics, cell_metrics);
}

bool qsg_atlas_simple_text_run_candidate(
    const Terminal_render_text_run& run,
    terminal_cell_metrics_t         cell_metrics)
{
    if (!qsg_atlas_text_is_printable_ascii(run.text) ||
        run.clip_rect.isValid()                      ||
        !run.rect.isValid()                          ||
        !std::isfinite(cell_metrics.width)           ||
        cell_metrics.width <= 0.0                    ||
        !std::isfinite(cell_metrics.height)          ||
        cell_metrics.height <= 0.0                   ||
        !std::isfinite(cell_metrics.ascent)          ||
        run.hyperlink_id != 0U                       ||
        run.underline                                ||
        run.strike                                   ||
        !std::isfinite(run.baseline_origin.x())      ||
        !std::isfinite(run.baseline_origin.y()))
    {
        return false;
    }

    const int rect_cell_span = static_cast<int>(
        std::round(run.rect.width() / cell_metrics.width));
    if (rect_cell_span <= 0 ||
        run.text.size() != static_cast<qsizetype>(rect_cell_span))
    {
        return false;
    }

    return
        std::abs(
            run.rect.width() -
            static_cast<qreal>(run.text.size()) * cell_metrics.width) <=
            0.001 &&
        std::abs(run.baseline_origin.x() - run.rect.left()) <= 0.001;
}

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
bool qsg_atlas_msdf_text_run_candidate(
    const Terminal_render_text_run& run,
    terminal_cell_metrics_t         cell_metrics)
{
    if (!qsg_atlas_text_is_printable_ascii(run.text) ||
        !run.rect.isValid()                          ||
        !std::isfinite(cell_metrics.width)           ||
        cell_metrics.width <= 0.0                    ||
        !std::isfinite(cell_metrics.height)          ||
        cell_metrics.height <= 0.0                   ||
        !std::isfinite(cell_metrics.ascent)          ||
        run.hyperlink_id != 0U                       ||
        run.underline                                ||
        run.strike                                   ||
        !std::isfinite(run.baseline_origin.x())      ||
        !std::isfinite(run.baseline_origin.y()))
    {
        return false;
    }

    const int rect_cell_span = static_cast<int>(
        std::round(run.rect.width() / cell_metrics.width));
    if (rect_cell_span <= 0 ||
        run.text.size() != static_cast<qsizetype>(rect_cell_span))
    {
        return false;
    }

    return
        std::abs(
            run.rect.width() -
            static_cast<qreal>(run.text.size()) * cell_metrics.width) <=
            0.001 &&
        std::abs(run.baseline_origin.x() - run.rect.left()) <= 0.001;
}

bool qsg_atlas_msdf_text_font_is_supported(const QFont& font)
{
    return font.family() == vnm_terminal_default_monospace_font_family();
}
#endif

bool text_has_emoji_presentation(const QString& text);

bool qsg_atlas_text_has_emoji_presentation(
    const QString&                text,
    Qsg_atlas_producer_summary&   producer)
{
    if (qsg_atlas_text_is_ascii(text)) {
        ++producer.presentation_fast_text_runs;
        return false;
    }

    ++producer.presentation_run_scans;
    return text_has_emoji_presentation(text);
}

QByteArray prepared_text_cache_key(
    const Terminal_render_text_run& run,
    const QFont&                    font,
    terminal_cell_metrics_t         cell_metrics,
    qreal                           device_pixel_ratio,
    std::uint64_t                   font_epoch,
    bool                            cursor_text_run)
{
    QByteArray key;
    append_key_uint64(key, run.retained_line_id);
    append_key_uint64(key, run.content_generation);
    append_key_string(key, run.text);
    append_key_int(key, run.column);
    append_key_bool(key, cursor_text_run);
    append_key_uint64(key, font_epoch);
    append_key_string(key, font.toString());
    append_key_cell_metrics(key, cell_metrics);
    const qreal normalized_device_pixel_ratio =
        (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0)
            ? 1.0
            : std::max<qreal>(1.0, device_pixel_ratio);
    append_key_qreal(key, normalized_device_pixel_ratio);
    return key;
}

bool prepared_text_cache_key_is_usable(const Terminal_render_text_run& run)
{
    return run.retained_line_id != 0U && !run.text.isEmpty();
}

template <typename T, typename Append_fn>
void append_key_vector(
    QByteArray&             key,
    const std::vector<T>&   values,
    Append_fn               append_value)
{
    append_key_int(key, static_cast<int>(values.size()));
    for (const T& value : values) {
        append_value(key, value);
    }
}

void append_key_int_vector(QByteArray& key, const std::vector<int>& values)
{
    append_key_int(key, static_cast<int>(values.size()));
    for (const int value : values) {
        append_key_int(key, value);
    }
}

int atlas_rect_row(
    const QRectF&            rect,
    terminal_cell_metrics_t  cell_metrics,
    int                      row_count)
{
    if (rect.height() <= 0.0 || row_count <= 0 || cell_metrics.height <= 0.0) {
        return k_qsg_atlas_non_row;
    }

    const qreal full_height = static_cast<qreal>(row_count) * cell_metrics.height;
    if (rect.y() <= 0.0 && rect.height() >= full_height) {
        return k_qsg_atlas_all_rows;
    }

    const int first_row =
        static_cast<int>(std::floor(rect.top() / cell_metrics.height));
    const int last_row = static_cast<int>(std::floor(
        std::max<qreal>(0.0, rect.bottom() - 0.001) / cell_metrics.height));
    if (first_row < 0 || first_row >= row_count || first_row != last_row) {
        return k_qsg_atlas_non_row;
    }
    return first_row;
}

bool atlas_same_coalescing_band(const QRectF& left, const QRectF& right)
{
    constexpr qreal k_epsilon = 0.001;
    return
        std::abs(left.y()      - right.y())      < k_epsilon &&
        std::abs(left.height() - right.height()) < k_epsilon &&
        std::abs(left.right()  - right.left())   < k_epsilon;
}

std::vector<Terminal_render_rect> coalesced_atlas_background_rects(
    const std::vector<Terminal_render_rect>& rects)
{
    std::vector<Terminal_render_rect> coalesced;
    coalesced.reserve(rects.size());
    for (const Terminal_render_rect& rect : rects) {
        if (!coalesced.empty()) {
            Terminal_render_rect& previous = coalesced.back();
            if (!previous.antialias                         &&
                !rect.antialias                             &&
                previous.color.rgba() == rect.color.rgba()  &&
                atlas_same_coalescing_band(previous.rect, rect.rect))
            {
                previous.rect.setRight(rect.rect.right());
                continue;
            }
        }
        coalesced.push_back(rect);
    }
    return coalesced;
}

void append_pass_key(QByteArray& key, const atlas_pass_range_t& pass)
{
    append_key_int(key, static_cast<int>(pass.first));
    append_key_int(key, static_cast<int>(pass.count));
}

int atlas_pass_draw_count(const atlas_pass_range_t& pass)
{
    return pass.has_instances() ? 1 : 0;
}

int render_glyph_row_capacity_bucket(int required_capacity)
{
    if (required_capacity <= 0) {
        return 0;
    }
    if (required_capacity <= 8) {
        return 8;
    }

    constexpr int k_capacity_bucket = 32;
    return ((required_capacity + k_capacity_bucket - 1) / k_capacity_bucket) *
        k_capacity_bucket;
}

int render_glyph_row_capacity_sum(const std::vector<int>& capacities)
{
    return std::accumulate(capacities.begin(), capacities.end(), 0);
}

int render_glyph_row_capacity_max(const std::vector<int>& capacities)
{
    return capacities.empty()
        ? 0
        : *std::max_element(capacities.begin(), capacities.end());
}

qreal atlas_normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return std::max<qreal>(1.0, device_pixel_ratio);
}

bool text_has_emoji_presentation(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    const Terminal_utf8_width_result width =
        measure_utf8_width(QByteArrayView(utf8.constData(), utf8.size()));
    return std::any_of(
        width.codepoints.begin(),
        width.codepoints.end(),
        [](const Terminal_codepoint_width& codepoint) {
            return codepoint.width_class == Terminal_unicode_width_class::EMOJI_PRESENTATION ||
                codepoint.presentation == Terminal_unicode_presentation::EMOJI;
        });
}

Glyph_image_presentation glyph_image_presentation_for_source_range(
    const QString& text,
    qsizetype      source_start,
    qsizetype      source_end)
{
    if (source_start < 0 ||
        source_end <= source_start ||
        source_start >= text.size())
    {
        return Glyph_image_presentation::TEXT;
    }

    const qsizetype bounded_end = std::min(source_end, text.size());
    return text_has_emoji_presentation(text.mid(source_start, bounded_end - source_start))
        ? Glyph_image_presentation::COLOR
        : Glyph_image_presentation::TEXT;
}

struct Glyph_coverage_kind_candidates
{
    std::array<Glyph_coverage_kind, 4> kinds = {
        Glyph_coverage_kind::UNKNOWN,
        Glyph_coverage_kind::UNKNOWN,
        Glyph_coverage_kind::UNKNOWN,
        Glyph_coverage_kind::UNKNOWN,
    };
    int count = 0;
};

Glyph_coverage_kind_candidates qsg_atlas_cache_lookup_candidates(
    Glyph_image_presentation presentation)
{
    if (presentation == Glyph_image_presentation::COLOR) {
        return {{
                Glyph_coverage_kind::COLOR_IMAGE,
                Glyph_coverage_kind::GRAYSCALE_MASK,
                Glyph_coverage_kind::UNKNOWN,
            },
            2};
    }

    return {{
            Glyph_coverage_kind::LCD_RGB_MASK,
            Glyph_coverage_kind::LCD_BGR_MASK,
            Glyph_coverage_kind::GRAYSCALE_MASK,
            Glyph_coverage_kind::COLOR_IMAGE,
        },
        4};
}

bool qsg_atlas_image_format_is_color_alpha(QImage::Format format)
{
    switch (format) {
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
            return true;
        default:
            return false;
    }
}

bool qsg_atlas_image_format_is_single_channel_coverage(QImage::Format format)
{
    switch (format) {
        case QImage::Format_Indexed8:
        case QImage::Format_Grayscale8:
        case QImage::Format_Alpha8:
            return true;
        default:
            return false;
    }
}

template <typename Pipeline, typename = void>
struct Qsg_atlas_dual_source_blend_api
{
    static constexpr bool available = false;

    static QRhiGraphicsPipeline::TargetBlend target_blend()
    {
        return atlas_blend();
    }
};

template <typename Pipeline>
struct Qsg_atlas_dual_source_blend_api<
    Pipeline,
    std::void_t<
        decltype(Pipeline::Src1Color),
        decltype(Pipeline::OneMinusSrc1Color),
        decltype(Pipeline::Src1Alpha),
        decltype(Pipeline::OneMinusSrc1Alpha)>>
{
    static constexpr bool available = true;

    static QRhiGraphicsPipeline::TargetBlend target_blend()
    {
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable   = true;
        blend.srcColor = Pipeline::Src1Color;
        blend.dstColor = Pipeline::OneMinusSrc1Color;
        blend.srcAlpha = Pipeline::Src1Alpha;
        blend.dstAlpha = Pipeline::OneMinusSrc1Alpha;
        return blend;
    }
};

void record_rejected_glyph_image(
    Glyph_coverage_counts& counts,
    Glyph_coverage_kind    kind)
{
    switch (kind) {
        case Glyph_coverage_kind::LCD_RGB_MASK:
            ++counts.lcd_rgb_masks;
            break;
        case Glyph_coverage_kind::LCD_BGR_MASK:
            ++counts.lcd_bgr_masks;
            break;
        case Glyph_coverage_kind::COLOR_IMAGE:
            ++counts.color_images;
            break;
        case Glyph_coverage_kind::AMBIGUOUS:
            ++counts.ambiguous_images;
            break;
        case Glyph_coverage_kind::UNSUPPORTED:
            ++counts.unsupported_images;
            break;
        case Glyph_coverage_kind::UNKNOWN:
            ++counts.missed_images;
            break;
        case Glyph_coverage_kind::GRAYSCALE_MASK:
            break;
    }
}

void record_accepted_glyph_image(
    Glyph_coverage_counts& counts,
    Glyph_coverage_kind    kind)
{
    switch (kind) {
        case Glyph_coverage_kind::GRAYSCALE_MASK:
            ++counts.grayscale_masks;
            break;
        case Glyph_coverage_kind::LCD_RGB_MASK:
            ++counts.lcd_rgb_masks;
            break;
        case Glyph_coverage_kind::LCD_BGR_MASK:
            ++counts.lcd_bgr_masks;
            break;
        case Glyph_coverage_kind::COLOR_IMAGE:
            ++counts.color_images;
            break;
        case Glyph_coverage_kind::AMBIGUOUS:
        case Glyph_coverage_kind::UNSUPPORTED:
        case Glyph_coverage_kind::UNKNOWN:
            record_rejected_glyph_image(counts, kind);
            break;
    }
}

int qsg_atlas_color_channel_spread(const QColor& color)
{
    const int min_channel = std::min({color.red(), color.green(), color.blue()});
    const int max_channel = std::max({color.red(), color.green(), color.blue()});
    return max_channel - min_channel;
}

bool qsg_atlas_rgb_image_has_channel_variation(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 0) {
                continue;
            }

            // Keep near-neutral RGB masks on the grayscale path; larger
            // channel spread indicates LCD subpixel or source-color data.
            if (qsg_atlas_color_channel_spread(pixel) >
                k_atlas_neutral_channel_spread_tolerance)
            {
                return true;
            }
        }
    }

    return false;
}

bool atlas_physical_coordinate_is_snapped(qreal coordinate)
{
    return
        std::isfinite(coordinate) &&
        std::abs(coordinate - std::round(coordinate)) <=
            k_atlas_physical_origin_snap_epsilon;
}

bool atlas_physical_origin_is_snapped(
    QPointF glyph_origin,
    qreal   device_pixel_ratio)
{
    const qreal normalized_device_pixel_ratio =
        atlas_normalized_device_pixel_ratio(device_pixel_ratio);
    return
        atlas_physical_coordinate_is_snapped(
            glyph_origin.x() * normalized_device_pixel_ratio) &&
        atlas_physical_coordinate_is_snapped(
            glyph_origin.y() * normalized_device_pixel_ratio);
}

qreal atlas_snapped_physical_coordinate(
    qreal coordinate,
    qreal device_pixel_ratio)
{
    if (!std::isfinite(coordinate)) {
        return coordinate;
    }

    const qreal normalized_device_pixel_ratio =
        atlas_normalized_device_pixel_ratio(device_pixel_ratio);
    return std::round(coordinate * normalized_device_pixel_ratio) /
        normalized_device_pixel_ratio;
}

int atlas_snapped_physical_int(
    qreal coordinate,
    qreal device_pixel_ratio)
{
    if (!std::isfinite(coordinate)) {
        return 0;
    }

    const qreal normalized_device_pixel_ratio =
        atlas_normalized_device_pixel_ratio(device_pixel_ratio);
    return static_cast<int>(
        std::lround(coordinate * normalized_device_pixel_ratio));
}

QFontEngine::GlyphFormat atlas_glyph_format_for_presentation(
    const QFontEngine&        font_engine,
    Glyph_image_presentation  presentation)
{
    if (font_engine.isColorFont() ||
        presentation == Glyph_image_presentation::COLOR)
    {
        return QFontEngine::Format_ARGB;
    }

    return presentation == Glyph_image_presentation::UNKNOWN
        ? QFontEngine::Format_A8
        : QFontEngine::Format_A32;
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

void store_uv_bounds(
    float* target,
    float  left,
    float  top,
    float  right,
    float  bottom,
    int    atlas_size)
{
    const float min_left   = std::min(left, right);
    const float min_top    = std::min(top, bottom);
    const float max_right  = std::max(left, right);
    const float max_bottom = std::max(top, bottom);
    const float half_texel = atlas_size > 0
        ? 0.5f / static_cast<float>(atlas_size)
        : 0.0f;
    const float margin_x = std::min(
        half_texel,
        std::max(0.0f, max_right - min_left) * 0.499f);
    const float margin_y = std::min(
        half_texel,
        std::max(0.0f, max_bottom - min_top) * 0.499f);
    target[0] = min_left + margin_x;
    target[1] = min_top + margin_y;
    target[2] = max_right - margin_x;
    target[3] = max_bottom - margin_y;
}

float qsg_atlas_gpu_coverage_kind(Glyph_coverage_kind kind)
{
    switch (kind) {
        case Glyph_coverage_kind::GRAYSCALE_MASK:
            return k_atlas_gpu_kind_grayscale;
        case Glyph_coverage_kind::LCD_RGB_MASK:
            return k_atlas_gpu_kind_lcd_rgb;
        case Glyph_coverage_kind::LCD_BGR_MASK:
            return k_atlas_gpu_kind_lcd_bgr;
        case Glyph_coverage_kind::COLOR_IMAGE:
            return k_atlas_gpu_kind_color;
        case Glyph_coverage_kind::UNKNOWN:
        case Glyph_coverage_kind::AMBIGUOUS:
        case Glyph_coverage_kind::UNSUPPORTED:
            break;
    }
    return k_atlas_gpu_kind_grayscale;
}

void store_atlas_info(
    float*              target,
    Glyph_coverage_kind coverage_kind,
    int                 page)
{
    target[0] = qsg_atlas_gpu_coverage_kind(coverage_kind);
    target[1] = static_cast<float>(std::max(0, page));
    target[2] = 0.0f;
    target[3] = 0.0f;
}

class Qsg_atlas_render_node final : public QSGRenderNode
{
public:
    explicit Qsg_atlas_render_node(
        std::shared_ptr<Qsg_atlas_recorder> recorder)
    :
        m_recorder(std::move(recorder))
    {}

    ~Qsg_atlas_render_node() override
    {
        releaseResources();
    }

    void set_frame(
        Captured_atlas_frame                    frame,
        std::shared_ptr<Qsg_atlas_recorder>
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
        Active_profiler_binding profiler_binding(m_frame.render_profiler.get());
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::prepare");

        QRhiCommandBuffer* const command_buffer = commandBuffer();
        QRhiRenderTarget* const  target         = renderTarget();
        QRhi* const              rhi            = target != nullptr ? target->rhi() : nullptr;
        const bool command_buffer_non_null      = command_buffer != nullptr;
        const bool render_target_non_null       = target != nullptr;
        const bool rhi_non_null                 = rhi != nullptr;
        const std::uint64_t prepare_thread      = current_thread_id();

        bool coverage_texture_created = false;
        bool coverage_upload_recorded = false;
        bool raw_font_rasterized = false;
        std::uint64_t raster_thread = 0U;
        int rasterized_glyphs       = 0;

        Atlas_prepare_result prepare_result = prepare_atlas_instances();
        raw_font_rasterized = prepare_result.raw_font_rasterized;
        raster_thread       = prepare_result.raster_thread;
        rasterized_glyphs   = prepare_result.rasterized_glyphs;

        if (rhi != nullptr && command_buffer != nullptr && target != nullptr) {
            const bool rect_ready = ensure_rect_resources(rhi, target);
            const bool atlas_ready = rect_ready && upload_coverage_texture(
                rhi,
                command_buffer,
                prepare_result.rasterized_glyphs > 0,
                &coverage_texture_created,
                &coverage_upload_recorded);
            const bool glyph_ready =
                rect_ready &&
                (!has_glyph_draw_passes() || ensure_glyph_resources(rhi, target));
            const bool msdf_atlas_ready =
                rect_ready &&
                upload_msdf_text_atlas_texture(
                    rhi,
                    command_buffer,
                    prepare_result.render);
            const bool msdf_ready =
                rect_ready &&
                msdf_atlas_ready &&
                ensure_msdf_text_resources(rhi, target);
            if (has_msdf_text_draw_passes() && !msdf_ready) {
                prepare_result.render.msdf_text_missed_supported_runs +=
                    prepare_result.render.msdf_text_runs;
                prepare_result.render.msdf_text_missed_supported_glyphs +=
                prepare_result.render.msdf_text_glyph_instances;
            }
            m_resources_ready = rect_ready &&
                atlas_ready &&
                glyph_ready &&
                msdf_ready &&
                update_atlas_buffers(
                    rhi,
                    command_buffer,
                    target->pixelSize(),
                    &prepare_result.render);
            prepare_result.render.coverage_texture_uploaded =
                coverage_upload_recorded;
            prepare_result.render.coverage_texture_skipped =
                atlas_ready &&
                !coverage_upload_recorded &&
                m_cache.stats().page_count > 0;
        }
        else {
            m_resources_ready = false;
        }
        prepare_result.render.glyph_sampler_mode = m_glyph_sampler_mode;
        prepare_result.render.msdf_text_sampler_mode = m_msdf_text_sampler_mode;
        prepare_result.render.glyph_shader_package_available =
            glyph_shader_package_available();
        prepare_result.render.msdf_text_shader_package_available =
            msdf_text_shader_package_available();
        prepare_result.render.msdf_text_texture_ready =
            m_msdf_text_atlas_texture != nullptr;
        prepare_result.render.msdf_text_resources_ready =
            msdf_text_resources_ready();
        prepare_result.render.msdf_text_renderer_active =
            k_qsg_atlas_msdf_text_renderer_compiled &&
            prepare_result.render.msdf_text_supported_runs > 0 &&
            prepare_result.render.msdf_text_missed_supported_runs == 0 &&
            prepare_result.render.msdf_text_atlas_ready &&
            prepare_result.render.msdf_text_resources_ready;
        prepare_result.render.dual_source_probe_shader_package_available =
            dual_source_probe_shader_package_available();
        prepare_result.render.dual_source_blend_factors_runtime_probe =
            m_dual_source_blend_factors_probe_completed;
        prepare_result.render.dual_source_blend_factors_available =
            m_dual_source_blend_factors_available;

        if (m_recorder != nullptr) {
            m_recorder->record_prepare(
                m_frame,
                command_buffer_non_null,
                render_target_non_null,
                rhi_non_null,
                coverage_texture_created,
                coverage_upload_recorded,
                raw_font_rasterized,
                raw_font_rasterized && raster_thread == prepare_thread,
                rasterized_glyphs,
                prepare_thread,
                raster_thread,
                m_cache.stats(),
                prepare_result.frame_build,
                prepare_result.render,
                prepare_result.producer,
                prepare_result.warm_lazy);
        }
    }

    void render(const RenderState* state) override
    {
        QRect viewport_rect;
        bool  drew = false;
        {
            Active_profiler_binding profiler_binding(m_frame.render_profiler.get());
            VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::render");

            QRhiCommandBuffer* const command_buffer = commandBuffer();
            QRhiRenderTarget* const  target         = renderTarget();

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

                const bool stencil_enabled =
                    state != nullptr && state->stencilEnabled();
                if (stencil_enabled) {
                    command_buffer->setStencilRef(
                        static_cast<quint32>(state->stencilValue()));
                }

                draw_rect_pass(command_buffer, m_background_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_selection_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_graphic_pass, stencil_enabled);
                draw_glyph_pass(command_buffer, m_text_pass, stencil_enabled);
                draw_msdf_text_pass(command_buffer, m_msdf_text_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_decoration_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_cursor_pass, stencil_enabled);
                draw_glyph_pass(command_buffer, m_cursor_text_pass, stencil_enabled);
                draw_msdf_text_pass(
                    command_buffer,
                    m_msdf_cursor_text_pass,
                    stencil_enabled);
                draw_rect_pass(command_buffer, m_overlay_pass, stencil_enabled);
                drew = total_instance_count() > 0U;
            }
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
        delete_resource(m_stencil_msdf_text_pipeline);
        delete_resource(m_msdf_text_pipeline);
        delete_resource(m_stencil_glyph_pipeline);
        delete_resource(m_glyph_pipeline);
        delete_resource(m_stencil_rect_pipeline);
        delete_resource(m_rect_pipeline);
        delete_resource(m_msdf_text_shader_resources);
        delete_resource(m_glyph_shader_resources);
        delete_resource(m_rect_shader_resources);
        delete_resource(m_msdf_text_sampler);
        delete_resource(m_coverage_sampler);
        delete_resource(m_msdf_text_uniform_buffer);
        delete_resource(m_uniform_buffer);
        delete_resource(m_msdf_text_instance_buffer);
        delete_resource(m_glyph_instance_buffer);
        delete_resource(m_rect_instance_buffer);
        delete_resource(m_vertex_buffer);
        delete_resource(m_msdf_text_atlas_texture);
        delete_resource(m_coverage_texture);
        m_resource_rhi                  = nullptr;
        m_render_pass_serialized_format.clear();
        m_render_target_samples         = 0;
        m_rect_instance_buffer_size      = 0U;
        m_glyph_instance_buffer_size     = 0U;
        m_msdf_text_instance_buffer_size = 0U;
        m_static_vertex_upload_needed    = true;
        m_resources_ready                = false;
        m_dual_source_blend_factors_probe_completed = false;
        m_dual_source_blend_factors_available       = false;
        m_rect_upload_planner.reset();
        m_glyph_upload_planner.reset();
        m_msdf_text_upload_planner.reset();
        m_glyph_sampler_mode     = Qsg_atlas_sampler_mode::UNKNOWN;
        m_msdf_text_sampler_mode = Qsg_atlas_sampler_mode::UNKNOWN;
        m_render_glyph_text_row_capacities.clear();
        m_render_glyph_cursor_text_row_capacities.clear();
        m_glyph_buffer_row_stable_ranges.clear();
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        m_msdf_text_uploaded_generation = 0U;
        m_msdf_text_resources_ready     = false;
#endif
    }

private:
    bool ensure_shaders()
    {
        if (!m_shader_packages_checked) {
            m_vertex_shader              = load_shader(k_atlas_vertex_shader_path);
            m_fragment_shader            = load_shader(k_atlas_fragment_shader_path);
            m_glyph_vertex_shader        = load_shader(k_atlas_glyph_vertex_shader_path);
            m_glyph_fragment_shader      = load_shader(k_atlas_glyph_fragment_shader_path);
            m_dual_source_probe_fragment_shader =
                load_shader(k_atlas_dual_source_probe_fragment_shader_path);
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
            m_msdf_text_vertex_shader =
                load_shader(k_atlas_msdf_text_vertex_shader_path);
            m_msdf_text_fragment_shader =
                load_shader(k_atlas_msdf_text_fragment_shader_path);
#endif
            m_shader_packages_checked = true;
        }

        return
            m_vertex_shader.isValid()         &&
            m_fragment_shader.isValid()       &&
            m_glyph_vertex_shader.isValid()   &&
            m_glyph_fragment_shader.isValid();
    }

    bool glyph_shader_package_available() const
    {
        return m_glyph_vertex_shader.isValid() && m_glyph_fragment_shader.isValid();
    }

    bool dual_source_probe_shader_package_available() const
    {
        return m_dual_source_probe_fragment_shader.isValid();
    }

    bool msdf_text_shader_package_available() const
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        return
            m_msdf_text_vertex_shader.isValid() &&
            m_msdf_text_fragment_shader.isValid();
#else
        return false;
#endif
    }

    bool msdf_text_resources_ready() const
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        return m_msdf_text_resources_ready;
#else
        return false;
#endif
    }

    bool msdf_text_atlas_built() const
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        return m_msdf_text_cache.atlas_built;
#else
        return false;
#endif
    }

    bool msdf_text_atlas_ready() const
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        return m_msdf_text_cache.ready;
#else
        return false;
#endif
    }

    void record_msdf_text_cache_summary(
        Qsg_atlas_render_summary& summary) const
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        summary.msdf_text_font_data_bytes = m_msdf_text_cache.font_data_bytes;
        summary.msdf_text_pixel_height    = m_msdf_text_cache.pixel_height;
        summary.msdf_text_atlas_size      =
            m_msdf_text_cache.atlas.atlas_size;
        summary.msdf_text_px_range        = m_msdf_text_cache.atlas.px_range;
        summary.msdf_text_message         = m_msdf_text_cache.message;
#else
        (void)summary;
#endif
    }

    void configure_stencil_state(
        QRhiGraphicsPipeline* pipeline,
        bool                  stencil_enabled) const
    {
        if (!stencil_enabled) {
            return;
        }

        const QRhiGraphicsPipeline::StencilOpState stencil_state =
            atlas_stencil_state();
        pipeline->setStencilTest(true);
        pipeline->setStencilFront(stencil_state);
        pipeline->setStencilBack(stencil_state);
        pipeline->setStencilReadMask(k_atlas_stencil_mask);
        pipeline->setStencilWriteMask(k_atlas_stencil_mask);
    }

    QRhiGraphicsPipeline* create_rect_pipeline(
        QRhi*                     rhi,
        QRhiRenderPassDescriptor* render_pass_descriptor,
        bool                      stencil_enabled)
    {
        QRhiGraphicsPipeline* pipeline = rhi->newGraphicsPipeline();
        if (pipeline == nullptr) {
            return nullptr;
        }

        QRhiGraphicsPipeline::Flags flags = QRhiGraphicsPipeline::UsesScissor;
        if (stencil_enabled) {
            flags |= QRhiGraphicsPipeline::UsesStencilRef;
        }
        pipeline->setFlags(flags);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({atlas_blend()});
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        configure_stencil_state(pipeline, stencil_enabled);
        pipeline->setSampleCount(m_render_target_samples);
        pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_fragment_shader),
        });
        pipeline->setVertexInputLayout(atlas_vertex_t_input_layout());
        pipeline->setShaderResourceBindings(m_rect_shader_resources);
        pipeline->setRenderPassDescriptor(render_pass_descriptor);
        if (!pipeline->create()) {
            delete pipeline;
            return nullptr;
        }

        return pipeline;
    }

    bool probe_dual_source_blend_factors(
        QRhi*                     rhi,
        QRhiRenderPassDescriptor* render_pass_descriptor)
    {
        if (m_dual_source_blend_factors_probe_completed) {
            return m_dual_source_blend_factors_available;
        }

        m_dual_source_blend_factors_available = false;
        if (!Qsg_atlas_dual_source_blend_api<QRhiGraphicsPipeline>::available ||
            rhi == nullptr                                             ||
            render_pass_descriptor == nullptr                          ||
            m_rect_shader_resources == nullptr                         ||
            !m_dual_source_probe_fragment_shader.isValid())
        {
            return false;
        }

        QRhiGraphicsPipeline* pipeline = rhi->newGraphicsPipeline();
        if (pipeline == nullptr) {
            return false;
        }

        pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({
            Qsg_atlas_dual_source_blend_api<QRhiGraphicsPipeline>::target_blend(),
        });
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        configure_stencil_state(pipeline, false);
        pipeline->setSampleCount(m_render_target_samples);
        pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_vertex_shader),
            QRhiShaderStage(
                QRhiShaderStage::Fragment,
                m_dual_source_probe_fragment_shader),
        });
        pipeline->setVertexInputLayout(atlas_vertex_t_input_layout());
        pipeline->setShaderResourceBindings(m_rect_shader_resources);
        pipeline->setRenderPassDescriptor(render_pass_descriptor);
        m_dual_source_blend_factors_probe_completed = true;
        m_dual_source_blend_factors_available = pipeline->create();
        delete pipeline;
        return m_dual_source_blend_factors_available;
    }

    QRhiGraphicsPipeline* create_glyph_pipeline(
        QRhi*                         rhi,
        QRhiRenderPassDescriptor*     render_pass_descriptor,
        QRhiShaderResourceBindings*   shader_resources,
        bool                          stencil_enabled)
    {
        QRhiGraphicsPipeline* pipeline = rhi->newGraphicsPipeline();
        if (pipeline == nullptr) {
            return nullptr;
        }

        QRhiGraphicsPipeline::Flags flags = QRhiGraphicsPipeline::UsesScissor;
        if (stencil_enabled) {
            flags |= QRhiGraphicsPipeline::UsesStencilRef;
        }
        pipeline->setFlags(flags);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({
            Qsg_atlas_dual_source_blend_api<QRhiGraphicsPipeline>::target_blend(),
        });
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        configure_stencil_state(pipeline, stencil_enabled);
        pipeline->setSampleCount(m_render_target_samples);
        pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_glyph_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_glyph_fragment_shader),
        });
        pipeline->setVertexInputLayout(atlas_glyph_vertex_input_layout());
        pipeline->setShaderResourceBindings(shader_resources);
        pipeline->setRenderPassDescriptor(render_pass_descriptor);
        if (!pipeline->create()) {
            delete pipeline;
            return nullptr;
        }

        return pipeline;
    }

    QRhiGraphicsPipeline* create_msdf_text_pipeline(
        QRhi*                         rhi,
        QRhiRenderPassDescriptor*     render_pass_descriptor,
        QRhiShaderResourceBindings*   shader_resources,
        bool                          stencil_enabled)
    {
        QRhiGraphicsPipeline* pipeline = rhi->newGraphicsPipeline();
        if (pipeline == nullptr) {
            return nullptr;
        }

        QRhiGraphicsPipeline::Flags flags = QRhiGraphicsPipeline::UsesScissor;
        if (stencil_enabled) {
            flags |= QRhiGraphicsPipeline::UsesStencilRef;
        }
        pipeline->setFlags(flags);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({atlas_msdf_text_blend()});
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        configure_stencil_state(pipeline, stencil_enabled);
        pipeline->setSampleCount(m_render_target_samples);
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex,   m_msdf_text_vertex_shader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_msdf_text_fragment_shader),
        });
#endif
        pipeline->setVertexInputLayout(atlas_msdf_text_vertex_input_layout());
        pipeline->setShaderResourceBindings(shader_resources);
        pipeline->setRenderPassDescriptor(render_pass_descriptor);
        if (!pipeline->create()) {
            delete pipeline;
            return nullptr;
        }

        return pipeline;
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
                sizeof(atlas_vertex_t) * k_atlas_quad_vertices.size()));
        m_uniform_buffer = rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            static_cast<quint32>(rhi->ubufAligned(sizeof(atlas_uniform_t))));
        if (m_vertex_buffer == nullptr ||
            m_uniform_buffer == nullptr)
        {
            releaseResources();
            return false;
        }
        if (!m_vertex_buffer->create() ||
            !m_uniform_buffer->create())
        {
            releaseResources();
            return false;
        }

        m_rect_shader_resources = rhi->newShaderResourceBindings();
        if (m_rect_shader_resources == nullptr) {
            releaseResources();
            return false;
        }
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

        m_rect_pipeline = create_rect_pipeline(
            rhi,
            render_pass_descriptor,
            false);
        m_stencil_rect_pipeline = create_rect_pipeline(
            rhi,
            render_pass_descriptor,
            true);
        if (m_rect_pipeline == nullptr || m_stencil_rect_pipeline == nullptr) {
            releaseResources();
            return false;
        }

        (void)probe_dual_source_blend_factors(rhi, render_pass_descriptor);
        m_static_vertex_upload_needed = true;
        return true;
    }

    bool ensure_coverage_texture(QRhi* rhi, bool* out_created = nullptr)
    {
        if (out_created != nullptr) {
            *out_created = false;
        }

        const QSize page_size = m_cache.stats().page_size;
        const int page_budget = std::max(1, m_cache.stats().page_budget);
        if (m_coverage_texture != nullptr &&
            m_coverage_texture->format()    == QRhiTexture::RGBA8 &&
            m_coverage_texture->pixelSize() == page_size          &&
            m_coverage_texture->arraySize() == page_budget)
        {
            return true;
        }

        if (rhi == nullptr || !rhi->isFeatureSupported(QRhi::TextureArrays)) {
            return false;
        }

        delete_resource(m_stencil_glyph_pipeline);
        delete_resource(m_glyph_pipeline);
        delete_resource(m_glyph_shader_resources);
        delete_resource(m_coverage_texture);

        QRhiTexture* texture =
            rhi->newTextureArray(QRhiTexture::RGBA8, page_budget, page_size);
        if (texture == nullptr || !texture->create()) {
            delete_resource(texture);
            return false;
        }

        m_coverage_texture = texture;
        if (out_created != nullptr) {
            *out_created = true;
        }
        return true;
    }

    bool ensure_glyph_resources(QRhi* rhi, QRhiRenderTarget* target)
    {
        if (m_glyph_pipeline != nullptr && m_stencil_glyph_pipeline != nullptr) {
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
            m_glyph_sampler_mode = Qsg_atlas_sampler_mode::NEAREST;
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
        QRhiGraphicsPipeline* glyph_pipeline = create_glyph_pipeline(
            rhi,
            render_pass_descriptor,
            shader_resources,
            false);
        QRhiGraphicsPipeline* stencil_glyph_pipeline = create_glyph_pipeline(
            rhi,
            render_pass_descriptor,
            shader_resources,
            true);
        if (glyph_pipeline == nullptr || stencil_glyph_pipeline == nullptr) {
            delete_resource(glyph_pipeline);
            delete_resource(stencil_glyph_pipeline);
            delete_resource(shader_resources);
            return false;
        }

        m_glyph_shader_resources  = shader_resources;
        m_glyph_pipeline          = glyph_pipeline;
        m_stencil_glyph_pipeline  = stencil_glyph_pipeline;
        return true;
    }

    bool ensure_msdf_text_atlas_texture(QRhi* rhi, bool* out_created = nullptr)
    {
        if (out_created != nullptr) {
            *out_created = false;
        }

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (!m_msdf_text_cache.ready || m_msdf_text_cache.atlas.atlas_size <= 0) {
            return false;
        }

        const QSize atlas_size(
            m_msdf_text_cache.atlas.atlas_size,
            m_msdf_text_cache.atlas.atlas_size);
        if (m_msdf_text_atlas_texture != nullptr &&
            m_msdf_text_atlas_texture->format()    == QRhiTexture::RGBA8 &&
            m_msdf_text_atlas_texture->pixelSize() == atlas_size)
        {
            return true;
        }

        delete_resource(m_stencil_msdf_text_pipeline);
        delete_resource(m_msdf_text_pipeline);
        delete_resource(m_msdf_text_shader_resources);
        delete_resource(m_msdf_text_atlas_texture);

        QRhiTexture* texture =
            rhi->newTexture(QRhiTexture::RGBA8, atlas_size);
        if (texture == nullptr || !texture->create()) {
            delete_resource(texture);
            return false;
        }

        m_msdf_text_atlas_texture = texture;
        if (out_created != nullptr) {
            *out_created = true;
        }
        return true;
#else
        (void)rhi;
        return false;
#endif
    }

    bool upload_msdf_text_atlas_texture(
        QRhi*                       rhi,
        QRhiCommandBuffer*          command_buffer,
        Qsg_atlas_render_summary&   summary)
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (!has_msdf_text_draw_passes()) {
            return true;
        }

        bool texture_created = false;
        const bool texture_ready =
            ensure_msdf_text_atlas_texture(rhi, &texture_created);
        summary.msdf_text_texture_ready =
            texture_ready && m_msdf_text_atlas_texture != nullptr;
        if (!texture_ready) {
            return false;
        }

        if (!texture_created &&
            m_msdf_text_uploaded_generation == m_msdf_text_cache.generation)
        {
            return true;
        }

        QImage image(
            m_msdf_text_cache.atlas.rgba.data(),
            m_msdf_text_cache.atlas.atlas_size,
            m_msdf_text_cache.atlas.atlas_size,
            m_msdf_text_cache.atlas.atlas_size * 4,
            QImage::Format_RGBA8888);
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        updates->uploadTexture(m_msdf_text_atlas_texture, image);
        command_buffer->resourceUpdate(updates);
        m_msdf_text_uploaded_generation = m_msdf_text_cache.generation;
        summary.msdf_text_texture_uploaded = true;
        return true;
#else
        (void)rhi;
        (void)command_buffer;
        (void)summary;
        return true;
#endif
    }

    bool ensure_msdf_text_resources(QRhi* rhi, QRhiRenderTarget* target)
    {
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (!has_msdf_text_draw_passes()) {
            m_msdf_text_resources_ready = false;
            return true;
        }

        m_msdf_text_resources_ready = false;
        if (!ensure_shaders()                         ||
            !msdf_text_shader_package_available()     ||
            !m_msdf_text_cache.ready                  ||
            m_msdf_text_atlas_texture == nullptr)
        {
            return false;
        }

        if (m_msdf_text_uniform_buffer == nullptr) {
            m_msdf_text_uniform_buffer = rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                static_cast<quint32>(
                    rhi->ubufAligned(sizeof(atlas_msdf_uniform_t))));
            if (m_msdf_text_uniform_buffer == nullptr ||
                !m_msdf_text_uniform_buffer->create())
            {
                delete_resource(m_msdf_text_uniform_buffer);
                return false;
            }
            delete_resource(m_stencil_msdf_text_pipeline);
            delete_resource(m_msdf_text_pipeline);
            delete_resource(m_msdf_text_shader_resources);
        }

        if (m_msdf_text_sampler == nullptr) {
            QRhiSampler* sampler = rhi->newSampler(
                QRhiSampler::Linear,
                QRhiSampler::Linear,
                QRhiSampler::None,
                QRhiSampler::ClampToEdge,
                QRhiSampler::ClampToEdge);
            if (sampler == nullptr || !sampler->create()) {
                delete_resource(sampler);
                return false;
            }

            m_msdf_text_sampler = sampler;
            m_msdf_text_sampler_mode = Qsg_atlas_sampler_mode::LINEAR;
            delete_resource(m_stencil_msdf_text_pipeline);
            delete_resource(m_msdf_text_pipeline);
            delete_resource(m_msdf_text_shader_resources);
        }

        if (m_msdf_text_pipeline != nullptr &&
            m_stencil_msdf_text_pipeline != nullptr)
        {
            m_msdf_text_resources_ready = true;
            return true;
        }

        delete_resource(m_msdf_text_shader_resources);
        QRhiShaderResourceBindings* shader_resources =
            rhi->newShaderResourceBindings();
        if (shader_resources == nullptr) {
            delete_resource(shader_resources);
            return false;
        }

        shader_resources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                m_msdf_text_uniform_buffer),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_msdf_text_atlas_texture,
                m_msdf_text_sampler),
        });
        if (!shader_resources->create()) {
            delete_resource(shader_resources);
            return false;
        }

        QRhiRenderPassDescriptor* const render_pass_descriptor =
            target->renderPassDescriptor();
        QRhiGraphicsPipeline* msdf_text_pipeline = create_msdf_text_pipeline(
            rhi,
            render_pass_descriptor,
            shader_resources,
            false);
        QRhiGraphicsPipeline* stencil_msdf_text_pipeline = create_msdf_text_pipeline(
            rhi,
            render_pass_descriptor,
            shader_resources,
            true);
        if (msdf_text_pipeline == nullptr ||
            stencil_msdf_text_pipeline == nullptr)
        {
            delete_resource(msdf_text_pipeline);
            delete_resource(stencil_msdf_text_pipeline);
            delete_resource(shader_resources);
            return false;
        }

        m_msdf_text_shader_resources         = shader_resources;
        m_msdf_text_pipeline                 = msdf_text_pipeline;
        m_stencil_msdf_text_pipeline         = stencil_msdf_text_pipeline;
        m_msdf_text_resources_ready          = true;
        return true;
#else
        (void)rhi;
        (void)target;
        return true;
#endif
    }

    atlas_pass_range_t append_rect_pass(
        const std::vector<Terminal_render_rect>& rects,
        qreal                                    opacity)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : rects) {
            append_rect_instance(rect.rect, rect.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_pass_range_t append_decoration_pass(
        const std::vector<Terminal_render_decoration>& decorations,
        qreal                                          opacity)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_decoration& decoration : decorations) {
            append_rect_instance(decoration.rect, decoration.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_pass_range_t append_cursor_pass(
        const std::vector<Terminal_render_cursor_primitive>& cursors,
        qreal                                                opacity)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_cursor_primitive& cursor : cursors) {
            append_rect_instance(cursor.rect, cursor.color, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_pass_range_t append_graphic_pass(
        const Terminal_render_frame& render_frame,
        qreal                        opacity)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : render_frame.graphic_rects) {
            append_graphic_rect_instance(rect, opacity);
        }

        for (const Terminal_render_arc& arc : render_frame.graphic_arcs) {
            append_arc_instances(arc, opacity);
        }
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_text_pass_ranges_t append_text_pass(
        const std::vector<Terminal_render_text_run>& runs,
        qreal                                        opacity,
        Atlas_prepare_result&                       result,
        bool                                        cursor_text_run)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::append_text_pass");

        atlas_text_pass_ranges_t ranges;
        ranges.glyph.first = static_cast<quint32>(m_glyph_instances.size());
        ranges.msdf.first  = static_cast<quint32>(m_msdf_text_instances.size());
        for (std::size_t index = 0U; index < runs.size(); ++index) {
            append_text_run(
                runs[index],
                opacity,
                result,
                static_cast<int>(index),
                cursor_text_run);
        }
        ranges.glyph.count =
            static_cast<quint32>(m_glyph_instances.size()) -
            ranges.glyph.first;
        ranges.msdf.count =
            static_cast<quint32>(m_msdf_text_instances.size()) -
            ranges.msdf.first;
        return ranges;
    }

    std::vector<int> glyph_required_row_capacities(
        const atlas_pass_range_t& pass) const
    {
        std::vector<int> row_capacities;
        if (!pass.has_instances() || m_render_row_count <= 0) {
            return row_capacities;
        }

        row_capacities.assign(
            static_cast<std::size_t>(m_render_row_count),
            0);
        for (quint32 offset = 0U; offset < pass.count; ++offset) {
            const std::size_t instance =
                static_cast<std::size_t>(pass.first + offset);
            if (instance >= m_glyph_instance_rows.size()) {
                continue;
            }

            const int row = m_glyph_instance_rows[instance];
            if (row < 0 || row >= m_render_row_count) {
                continue;
            }

            int& row_count = row_capacities[static_cast<std::size_t>(row)];
            ++row_count;
        }

        return row_capacities;
    }

    void update_render_glyph_row_capacities(
        std::vector<int>&        capacities,
        const std::vector<int>&  required_capacities)
    {
        if (m_render_row_count <= 0) {
            capacities.clear();
            return;
        }

        if (capacities.size() != static_cast<std::size_t>(m_render_row_count)) {
            capacities.assign(static_cast<std::size_t>(m_render_row_count), 0);
        }

        const std::size_t row_count = std::min(
            capacities.size(),
            required_capacities.size());
        for (std::size_t row = 0; row < row_count; ++row) {
            const int required = required_capacities[row];
            if (required > 0) {
                capacities[row] = std::max(
                    capacities[row],
                    render_glyph_row_capacity_bucket(required));
            }
        }
    }

    atlas_pass_range_t append_row_stable_glyph_pass(
        const atlas_pass_range_t&                    logical_pass,
        const std::vector<int>&                     row_capacities,
        std::vector<Qsg_atlas_row_stable_range>& row_ranges)
    {
        atlas_pass_range_t buffer_pass;
        buffer_pass.first =
            static_cast<quint32>(m_glyph_buffer_instances.size());
        if (!logical_pass.has_instances()) {
            return buffer_pass;
        }

        std::vector<int> row_write_offsets;
        std::vector<std::size_t> row_first_instances;
        if (m_render_row_count > 0 && !row_capacities.empty()) {
            row_write_offsets.assign(
                static_cast<std::size_t>(m_render_row_count),
                0);
            row_first_instances.assign(
                static_cast<std::size_t>(m_render_row_count),
                0U);
            const std::size_t row_slot_count = static_cast<std::size_t>(
                render_glyph_row_capacity_sum(row_capacities));
            const std::size_t row_slot_first = m_glyph_buffer_instances.size();
            m_glyph_buffer_instances.resize(row_slot_first + row_slot_count);
            m_glyph_buffer_instance_rows.resize(
                row_slot_first + row_slot_count,
                k_qsg_atlas_non_row);
            std::size_t row_first = row_slot_first;
            for (int row = 0; row < m_render_row_count; ++row) {
                row_first_instances[static_cast<std::size_t>(row)] = row_first;
                const int row_capacity =
                    row < static_cast<int>(row_capacities.size())
                    ? row_capacities[static_cast<std::size_t>(row)]
                    : 0;
                if (row_capacity > 0) {
                    row_ranges.push_back({
                        row,
                        static_cast<int>(row_first),
                        row_capacity,
                    });
                    std::fill_n(
                        m_glyph_buffer_instance_rows.begin() +
                            static_cast<std::ptrdiff_t>(row_first),
                        row_capacity,
                        row);
                }
                row_first += static_cast<std::size_t>(std::max(0, row_capacity));
            }
        }

        for (quint32 offset = 0U; offset < logical_pass.count; ++offset) {
            const std::size_t logical_instance =
                static_cast<std::size_t>(logical_pass.first + offset);
            if (logical_instance >= m_glyph_instances.size()) {
                continue;
            }

            const int row = logical_instance < m_glyph_instance_rows.size()
                ? m_glyph_instance_rows[logical_instance]
                : k_qsg_atlas_non_row;
            const int row_capacity =
                row >= 0 && row < static_cast<int>(row_capacities.size())
                    ? row_capacities[static_cast<std::size_t>(row)]
                    : 0;
            if (row_capacity > 0 && row < m_render_row_count) {
                int& row_offset = row_write_offsets[static_cast<std::size_t>(row)];
                const std::size_t stable_instance =
                    row_first_instances[static_cast<std::size_t>(row)] +
                    static_cast<std::size_t>(row_offset);
                if (row_offset < row_capacity &&
                    stable_instance < m_glyph_buffer_instances.size())
                {
                    m_glyph_buffer_instances[stable_instance] =
                        m_glyph_instances[logical_instance];
                }
                ++row_offset;
                continue;
            }

            m_glyph_buffer_instances.push_back(
                m_glyph_instances[logical_instance]);
            m_glyph_buffer_instance_rows.push_back(k_qsg_atlas_non_row);
        }

        buffer_pass.count =
            static_cast<quint32>(m_glyph_buffer_instances.size()) -
            buffer_pass.first;
        return buffer_pass;
    }

    void build_render_glyph_buffer_layout(Atlas_prepare_result& result)
    {
        const atlas_pass_range_t logical_text_pass        = m_text_pass;
        const atlas_pass_range_t logical_cursor_text_pass = m_cursor_text_pass;
        m_glyph_buffer_instances.clear();
        m_glyph_buffer_instance_rows.clear();
        m_glyph_buffer_row_stable_ranges.clear();

        const std::vector<int> text_required_capacities =
            glyph_required_row_capacities(logical_text_pass);
        const std::vector<int> cursor_required_capacities =
            glyph_required_row_capacities(logical_cursor_text_pass);
        update_render_glyph_row_capacities(
            m_render_glyph_text_row_capacities,
            text_required_capacities);
        update_render_glyph_row_capacities(
            m_render_glyph_cursor_text_row_capacities,
            cursor_required_capacities);

        m_text_pass = append_row_stable_glyph_pass(
            logical_text_pass,
            m_render_glyph_text_row_capacities,
            m_glyph_buffer_row_stable_ranges);
        m_cursor_text_pass = append_row_stable_glyph_pass(
            logical_cursor_text_pass,
            m_render_glyph_cursor_text_row_capacities,
            m_glyph_buffer_row_stable_ranges);

        result.render.glyph_buffer_instances =
            static_cast<int>(m_glyph_buffer_instances.size());
        result.render.glyph_text_row_capacity =
            render_glyph_row_capacity_max(m_render_glyph_text_row_capacities);
        result.render.glyph_cursor_text_row_capacity =
            render_glyph_row_capacity_max(
                m_render_glyph_cursor_text_row_capacities);
    }

    void append_rect_instance(
        const QRectF& rect,
        QColor        color,
        qreal         opacity)
    {
        if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
            return;
        }

        atlas_instance_t instance;
        store_rect(instance.rect, rect);
        store_color(instance.color, color, opacity);
        m_rect_instances.push_back(instance);
        m_rect_instance_rows.push_back(atlas_rect_row(
            rect,
            m_frame.cell_metrics,
            m_render_row_count));
    }

    void append_graphic_rect_instance(
        const Terminal_render_rect& rect,
        qreal                       opacity)
    {
        if (rect.antialias) {
            append_antialiased_rect_instances(rect, opacity);
            return;
        }

        append_rect_instance(rect.rect, rect.color, opacity);
    }

    template <typename Coverage_fn>
    int append_coverage_rasterized_rect_instances(
        const QRectF&   bounds,
        const QColor&   color,
        qreal           opacity,
        Coverage_fn     coverage_at)
    {
        const qreal pixel  = atlas_logical_pixel_size(m_frame.device_pixel_ratio);
        const int   left   = static_cast<int>(std::floor(bounds.left()   / pixel));
        const int   top    = static_cast<int>(std::floor(bounds.top()    / pixel));
        const int   right  = static_cast<int>(std::ceil(bounds.right()   / pixel));
        const int   bottom = static_cast<int>(std::ceil(bounds.bottom()  / pixel));

        int instance_count = 0;
        for (int y = top; y < bottom; ++y) {
            bool   has_span   = false;
            int    span_start = left;
            QColor span_color;
            for (int x = left; x < right; ++x) {
                const QPointF center(
                    (static_cast<qreal>(x) + 0.5) * pixel,
                    (static_cast<qreal>(y) + 0.5) * pixel);
                const qreal coverage = coverage_at(center);
                const QColor span_pixel_color = coverage > 0.0
                    ? terminal_render_coverage_color(color, coverage)
                    : QColor(0, 0, 0, 0);
                if (has_span && span_pixel_color.rgba() == span_color.rgba()) {
                    continue;
                }

                if (has_span) {
                    append_rect_instance(
                        QRectF(
                            static_cast<qreal>(span_start) * pixel,
                            static_cast<qreal>(y) * pixel,
                            static_cast<qreal>(x - span_start) * pixel,
                            pixel),
                        span_color,
                        opacity);
                    ++instance_count;
                }
                has_span   = span_pixel_color.alpha() != 0;
                span_start = x;
                span_color = span_pixel_color;
            }

            if (has_span) {
                append_rect_instance(
                    QRectF(
                        static_cast<qreal>(span_start) * pixel,
                        static_cast<qreal>(y) * pixel,
                        static_cast<qreal>(right - span_start) * pixel,
                        pixel),
                    span_color,
                    opacity);
                ++instance_count;
            }
        }
        return instance_count;
    }

    void append_antialiased_rect_instances(
        const Terminal_render_rect& rect,
        qreal                       opacity)
    {
        const QRectF bounds = rect.rect.adjusted(
            -k_terminal_graphic_antialias_feather,
            -k_terminal_graphic_antialias_feather,
            k_terminal_graphic_antialias_feather,
            k_terminal_graphic_antialias_feather);
        (void)append_coverage_rasterized_rect_instances(
            bounds,
            rect.color,
            opacity,
            [&](QPointF center) {
                return atlas_antialiased_rect_pixel_coverage(rect, center);
            });
    }

    void append_arc_instances(
        const Terminal_render_arc& arc,
        qreal                      opacity)
    {
        const terminal_render_arc_geometry_t arc_spec =
            terminal_render_arc_geometry(arc);
        (void)append_coverage_rasterized_rect_instances(
            arc.rect,
            arc.color,
            opacity,
            [&](QPointF center) {
                return terminal_render_arc_pixel_coverage(arc, arc_spec, center);
            });
    }

    Atlas_prepare_result prepare_atlas_instances()
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::prepare_atlas_instances");

        const bool font_epoch_changed =
            m_have_previous_render_font_epoch &&
            m_previous_render_font_epoch != m_frame.font_epoch;
        if (font_epoch_changed) {
            m_render_glyph_text_row_capacities.clear();
            m_render_glyph_cursor_text_row_capacities.clear();
        }
        begin_prepared_text_cache_frame(font_epoch_changed);
        m_cache.set_epoch(m_frame.font_epoch);
        m_current_prepare_had_lazy_insert = false;
        m_rect_instances.clear();
        m_glyph_instances.clear();
        m_glyph_buffer_instances.clear();
        m_msdf_text_instances.clear();
        m_rect_instance_rows.clear();
        m_glyph_instance_rows.clear();
        m_glyph_buffer_instance_rows.clear();
        m_msdf_text_instance_rows.clear();
        m_glyph_buffer_row_stable_ranges.clear();
        m_background_pass     = {};
        m_selection_pass      = {};
        m_graphic_pass        = {};
        m_text_pass           = {};
        m_msdf_text_pass      = {};
        m_decoration_pass     = {};
        m_cursor_pass         = {};
        m_cursor_text_pass    = {};
        m_msdf_cursor_text_pass = {};
        m_overlay_pass        = {};

        Atlas_prepare_result result;
        const QRawFont base_raw_font = QRawFont::fromFont(m_frame.font);
        result.base_face_id = base_raw_font.isValid()
            ? qsg_atlas_face_id_for_raw_font(base_raw_font)
            : QString();
        ensure_atlas_warm_set(result);
        Terminal_render_options options = m_frame.options;
        options.packed_text_sidecars_enabled = false;
        const Terminal_render_frame render_frame = build_terminal_render_frame(
            m_frame.snapshot.get(),
            m_frame.logical_size,
            m_frame.cell_metrics,
            options,
            m_frame.cursor_blink_visible,
            &m_frame.ime_preedit);
        m_render_row_count = render_frame.grid_size.rows;
        m_render_dirty_row_ranges = render_frame.dirty_row_ranges;

        const qreal opacity = std::clamp(inheritedOpacity(), 0.0, 1.0);
        const std::vector<Terminal_render_rect> background_rects =
            coalesced_atlas_background_rects(render_frame.background_rects);
        result.render.background_rects_before_coalescing =
            static_cast<int>(render_frame.background_rects.size());
        result.render.background_rects_after_coalescing =
            static_cast<int>(background_rects.size());
        result.render.background_rects_coalesced =
            result.render.background_rects_before_coalescing -
            result.render.background_rects_after_coalescing;

        m_background_pass = append_rect_pass(background_rects, opacity);
        m_selection_pass  = append_rect_pass(render_frame.selection_rects, opacity);
        m_graphic_pass    = append_graphic_pass(render_frame, opacity);
        const atlas_text_pass_ranges_t text_passes = append_text_pass(
            render_frame.text_runs,
            opacity,
            result,
            false);
        m_text_pass      = text_passes.glyph;
        m_msdf_text_pass = text_passes.msdf;
        m_decoration_pass = append_decoration_pass(render_frame.decorations, opacity);
        m_cursor_pass     = append_cursor_pass(render_frame.cursors, opacity);
        const atlas_text_pass_ranges_t cursor_text_passes = append_text_pass(
            render_frame.cursor_text_runs,
            opacity,
            result,
            true);
        m_cursor_text_pass      = cursor_text_passes.glyph;
        m_msdf_cursor_text_pass = cursor_text_passes.msdf;
        prune_prepared_text_cache(result.producer);
        m_overlay_pass = append_rect_pass(render_frame.overlay_rects, opacity);
        build_render_glyph_buffer_layout(result);
        result.raw_font_rasterized = result.rasterized_glyphs > 0;
        finalize_frame_build_summary(render_frame, result);
        finalize_render_summary(render_frame, font_epoch_changed, result);
        finalize_warm_lazy_summary(result);
        return result;
    }

    void ensure_atlas_warm_set(Atlas_prepare_result& result)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::ensure_atlas_warm_set");

        const qreal normalized_device_pixel_ratio =
            atlas_normalized_device_pixel_ratio(m_frame.device_pixel_ratio);
        const QString font_key = m_frame.font.toString();
        if (atlas_warm_key_matches(
                m_warm_key,
                m_frame.font_epoch,
                font_key,
                normalized_device_pixel_ratio,
                m_frame.cell_metrics))
        {
            result.warm_lazy = m_warm_lazy;
            return;
        }

        m_warm_key.font_epoch          = m_frame.font_epoch;
        m_warm_key.font_key            = font_key;
        m_warm_key.device_pixel_ratio  = normalized_device_pixel_ratio;
        m_warm_key.cell_metrics        = m_frame.cell_metrics;
        m_warm_key.valid               = true;
        m_warm_lazy                    = {};
        m_warm_lazy.warm_epoch         = m_frame.font_epoch;
        m_warm_lazy.warm_seed_strings  =
            static_cast<int>(k_qsg_atlas_warm_seed_strings.size());

        QElapsedTimer timer;
        timer.start();
        for (const qsg_atlas_warm_seed_string_t& seed :
            k_qsg_atlas_warm_seed_strings)
        {
            prewarm_atlas_seed(seed, result);
        }

        const Glyph_atlas_cache_stats cache = m_cache.stats();
        m_warm_lazy.warm_completed     =
            m_warm_lazy.warm_shaped_glyph_records > 0     &&
            m_warm_lazy.warm_covered_glyph_records > 0    &&
            m_warm_lazy.warm_failed_glyph_records == 0    &&
            m_warm_lazy.warm_failed_inserts == 0          &&
            m_warm_lazy.warm_unsupported_images == 0      &&
            m_warm_lazy.warm_missing_string_indexes == 0  &&
            m_warm_lazy.warm_invalid_string_indexes == 0;
        m_warm_lazy.warm_elapsed_ms    =
            static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        m_warm_lazy.warm_page_pressure =
            cache.failed_inserts > 0U ||
            (cache.budget_bytes > 0U &&
                cache.used_bytes >= (cache.budget_bytes * 9U) / 10U);
        result.warm_lazy = m_warm_lazy;
    }

    void prewarm_atlas_seed(
        const qsg_atlas_warm_seed_string_t& seed,
        Atlas_prepare_result&               result)
    {
        Terminal_render_text_run run;
        run.row             = -1;
        run.logical_row     = -1;
        run.column          = 0;
        run.text            = qsg_atlas_warm_seed_qstring(seed);
        run.foreground      = QColor(Qt::white);
        run.background      = QColor(Qt::transparent);
        run.rect            = QRectF(
            0.0,
            0.0,
            static_cast<qreal>(std::max<qsizetype>(1, run.text.size())) *
                m_frame.cell_metrics.width,
            m_frame.cell_metrics.height);
        run.baseline_origin = QPointF(0.0, m_frame.cell_metrics.ascent);

        const bool emoji_presentation_run = text_has_emoji_presentation(run.text);
        const Qsg_atlas_shaped_text_run_result shaped =
            qsg_atlas_shape_text_run(
                run,
                m_frame.font,
                m_frame.cell_metrics,
                m_frame.device_pixel_ratio,
                -1,
                false);
        m_warm_lazy.warm_shaped_glyph_records +=
            static_cast<int>(shaped.records.size());
        m_warm_lazy.warm_missing_string_indexes +=
            shaped.missing_string_indexes;
        m_warm_lazy.warm_invalid_string_indexes +=
            shaped.invalid_string_indexes;

        for (const Qsg_atlas_shaped_glyph_record& record : shaped.records) {
            if (record.glyph_bounds.width() <= 0.0 ||
                record.glyph_bounds.height() <= 0.0)
            {
                if (qsg_atlas_warm_seed_source_range_is_non_rendering(
                        run.text,
                        record.source_string_start,
                        record.source_string_end))
                {
                    ++m_warm_lazy.warm_skipped_glyph_records;
                }
                else {
                    ++m_warm_lazy.warm_environment_skipped_glyph_records;
                }
                continue;
            }

            QRawFont raster_font = record.raw_font;
            raster_font.setPixelSize(record.physical_pixel_size);
            const Glyph_image_presentation presentation =
                emoji_presentation_run
                    ? glyph_image_presentation_for_source_range(
                        run.text,
                        record.source_string_start,
                        record.source_string_end)
                    : Glyph_image_presentation::TEXT;
            const Glyph_atlas_slot slot = glyph_slot_for_index(
                record,
                raster_font,
                presentation,
                result,
                Atlas_cache_insert_source::WARM);
            if (slot.is_valid()) {
                ++m_warm_lazy.warm_covered_glyph_records;
            }
        }
    }

    void finalize_frame_build_summary(
        const Terminal_render_frame& render_frame,
        Atlas_prepare_result&       result)
    {
        Qsg_atlas_frame_build_summary& summary = result.frame_build;
        if (m_frame.snapshot != nullptr) {
            summary.snapshot_basis    = m_frame.snapshot->basis;
            summary.snapshot_purpose  = m_frame.snapshot->purpose;
            summary.selection_provenance_valid =
                render_snapshot_visible_line_provenance_is_valid(*m_frame.snapshot);
        }
        summary.viewport_active_buffer    = render_frame.viewport.active_buffer;
        summary.viewport_offset_from_tail = render_frame.viewport.offset_from_tail;
        summary.viewport_scrollback_rows  = render_frame.viewport.scrollback_rows;
        summary.dirty_rows                = render_frame.stats.dirty_rows;
        summary.full_dirty_rows           = render_frame.stats.full_dirty_rows;
        summary.frame_background_rects    =
            static_cast<int>(render_frame.background_rects.size());
        summary.frame_selection_rects     =
            static_cast<int>(render_frame.selection_rects.size());
        summary.frame_graphic_rects       =
            static_cast<int>(render_frame.graphic_rects.size());
        summary.frame_graphic_arcs        =
            static_cast<int>(render_frame.graphic_arcs.size());
        summary.frame_text_runs           =
            static_cast<int>(render_frame.text_runs.size());
        summary.frame_overlay_rects       =
            static_cast<int>(render_frame.overlay_rects.size());
        summary.packed_rows               =
            static_cast<int>(render_frame.packed_rows.size());
        summary.rect_instances            =
            static_cast<int>(m_rect_instances.size());
        summary.glyph_instances           =
            static_cast<int>(m_glyph_instances.size());
        summary.distinct_glyph_faces      =
            static_cast<int>(result.glyph_face_ids.size());
        summary.fallback_glyph_faces      =
            static_cast<int>(result.fallback_face_ids.size());
        summary.full_dirty_range          = dirty_range_covers_full_grid(render_frame);
        summary.public_projection_full_repaint =
            summary.snapshot_basis == Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            summary.full_dirty_range;
        summary.scroll_full_repaint =
            summary.snapshot_purpose == Terminal_render_snapshot_purpose::SCROLL &&
            summary.full_dirty_range;
        summary.full_repaint_fallback =
            summary.public_projection_full_repaint ||
            summary.scroll_full_repaint            ||
            summary.full_dirty_range;

        if (!render_frame.packed_rows.empty()) {
            const terminal_packed_render_row_t& first_row =
                render_frame.packed_rows.front();
            summary.first_packed_logical_row   = first_row.logical_row;
            summary.first_packed_active_buffer = first_row.active_buffer;
        }
        if (!render_frame.text_runs.empty()) {
            const Terminal_render_text_run& first_run = render_frame.text_runs.front();
            summary.first_text_logical_row        = first_run.logical_row;
            summary.first_text_retained_line_id   = first_run.retained_line_id;
            summary.first_text_content_generation = first_run.content_generation;
        }
    }

    QByteArray render_options_key(const Terminal_render_options& options) const
    {
        QByteArray key;
        append_key_color(key, options.default_background);
        append_key_color(key, options.default_foreground);
        append_key_color(key, options.selection_background);
        append_key_color(key, options.cursor_color);
        append_key_color(key, options.preedit_background);
        append_key_color(key, options.visual_bell_color);
        append_key_bool(key, options.cursor_shape_override.has_value());
        append_key_int(
            key,
            options.cursor_shape_override.has_value()
                ? static_cast<int>(*options.cursor_shape_override)
                : 0);
        append_key_bool(key, options.cursor_blink_enabled_override.has_value());
        append_key_bool(
            key,
            options.cursor_blink_enabled_override.has_value() &&
                *options.cursor_blink_enabled_override);
        append_key_bool(key, options.visual_bell_enabled);
        append_key_bool(key, options.underline_hyperlinks);
        append_key_bool(key, options.packed_text_sidecars_enabled);
        return key;
    }

    Atlas_frame_state_keys render_state_keys(
        const Terminal_render_frame& render_frame) const
    {
        Atlas_frame_state_keys keys;
        append_key_vector(
            keys.selection,
            render_frame.selection_rects,
            append_key_render_rect);

        append_key_vector(
            keys.cursor,
            render_frame.cursors,
            [](QByteArray& key, const Terminal_render_cursor_primitive& cursor) {
                append_key_int(key, static_cast<int>(cursor.kind));
                append_key_rect(key, cursor.rect);
                append_key_color(key, cursor.color);
            });
        append_key_vector(keys.cursor, render_frame.cursor_text_runs, append_key_text_run);

        append_key_string(keys.preedit, m_frame.ime_preedit.text);
        append_key_int(keys.preedit, m_frame.ime_preedit.cursor_position);
        append_key_bool(keys.preedit, m_frame.ime_preedit.active);

        keys.options = render_options_key(m_frame.options);

        append_key_bool(
            keys.visual_bell,
            m_frame.snapshot != nullptr &&
                m_frame.snapshot->metadata.visual_bell_active);
        append_key_bool(keys.visual_bell, m_frame.options.visual_bell_enabled);
        append_key_color(keys.visual_bell, m_frame.options.visual_bell_color);
        append_key_vector(
            keys.visual_bell,
            render_frame.overlay_rects,
            append_key_render_rect);
        return keys;
    }

    void finalize_render_summary(
        const Terminal_render_frame& render_frame,
        bool                         font_epoch_changed,
        Atlas_prepare_result&       result)
    {
        Qsg_atlas_render_summary& summary = result.render;
        summary.msdf_text_renderer_enabled =
            k_qsg_atlas_msdf_text_renderer_enabled;
        summary.msdf_text_renderer_compiled =
            k_qsg_atlas_msdf_text_renderer_compiled;
        summary.msdf_text_renderer_active =
            k_qsg_atlas_msdf_text_renderer_active;
        summary.msdf_text_atlas_built = msdf_text_atlas_built();
        summary.msdf_text_atlas_ready = msdf_text_atlas_ready();
        record_msdf_text_cache_summary(summary);
        summary.msdf_text_glyph_instances =
            static_cast<int>(m_msdf_text_instances.size());

        const Atlas_frame_state_keys current_keys =
            render_state_keys(render_frame);
        const bool selection_changed =
            m_have_previous_render_state &&
            current_keys.selection != m_previous_render_state_keys.selection;
        const bool cursor_changed =
            m_have_previous_render_state &&
            current_keys.cursor != m_previous_render_state_keys.cursor;
        const bool preedit_changed =
            m_have_previous_render_state &&
            current_keys.preedit != m_previous_render_state_keys.preedit;
        const bool options_changed =
            m_have_previous_render_state &&
            current_keys.options != m_previous_render_state_keys.options;
        const bool visual_bell_changed =
            m_have_previous_render_state &&
            current_keys.visual_bell != m_previous_render_state_keys.visual_bell;
        const bool has_dirty_rows = !render_frame.dirty_row_ranges.empty();
        Terminal_render_options default_options;
        default_options.cursor_shape_override =
            Terminal_cursor_shape::BLOCK;
        default_options.cursor_blink_enabled_override = true;
        default_options.packed_text_sidecars_enabled  = false;
        const bool selection_present =
            !render_frame.selection_rects.empty();
        const bool cursor_present =
            !render_frame.cursors.empty()              ||
            !render_frame.cursor_text_runs.empty();
        const bool preedit_present =
            m_frame.ime_preedit.active &&
            !m_frame.ime_preedit.text.isEmpty();
        const bool options_present =
            current_keys.options != render_options_key(default_options);
        const bool visual_bell_present =
            m_frame.snapshot != nullptr &&
            m_frame.snapshot->metadata.visual_bell_active &&
            m_frame.options.visual_bell_enabled;

        summary.full_dirty_range_reupload       = result.frame_build.full_dirty_range;
        summary.public_projection_full_reupload =
            result.frame_build.public_projection_full_repaint;
        summary.scroll_full_reupload            = result.frame_build.scroll_full_repaint;
        summary.font_epoch_invalidation         = font_epoch_changed;
        summary.non_dirty_selection_invalidation =
            !has_dirty_rows &&
            (selection_changed ||
                (!m_have_previous_render_state && selection_present));
        summary.non_dirty_cursor_invalidation =
            !has_dirty_rows &&
            (cursor_changed ||
                (!m_have_previous_render_state && cursor_present));
        summary.non_dirty_preedit_invalidation =
            !has_dirty_rows &&
            (preedit_changed ||
                (!m_have_previous_render_state && preedit_present));
        summary.non_dirty_options_invalidation =
            !has_dirty_rows &&
            (options_changed ||
                (!m_have_previous_render_state && options_present));
        summary.non_dirty_visual_bell_invalidation =
            !has_dirty_rows &&
            (visual_bell_changed ||
                (!m_have_previous_render_state && visual_bell_present));

        summary.rect_draw_calls =
            atlas_pass_draw_count(m_background_pass)     +
            atlas_pass_draw_count(m_selection_pass)      +
            atlas_pass_draw_count(m_graphic_pass)        +
            atlas_pass_draw_count(m_decoration_pass)     +
            atlas_pass_draw_count(m_cursor_pass)         +
            atlas_pass_draw_count(m_overlay_pass);
        summary.glyph_draw_calls =
            atlas_pass_draw_count(m_text_pass) +
            atlas_pass_draw_count(m_cursor_text_pass);
        summary.msdf_text_draw_calls =
            atlas_pass_draw_count(m_msdf_text_pass) +
            atlas_pass_draw_count(m_msdf_cursor_text_pass);
        summary.draw_calls =
            summary.rect_draw_calls       +
            summary.glyph_draw_calls      +
            summary.msdf_text_draw_calls;

        const Glyph_atlas_cache_stats cache = m_cache.stats();
        summary.atlas_page_count      = cache.page_count;
        summary.atlas_page_budget     = cache.page_budget;
        summary.atlas_page_bytes      = cache.page_bytes;
        summary.atlas_allocated_bytes = cache.allocated_bytes;
        summary.atlas_budget_bytes    = cache.budget_bytes;
        summary.atlas_used_bytes      = cache.used_bytes;
        summary.atlas_failed_inserts  = cache.failed_inserts;
        summary.atlas_page_pressure =
            cache.failed_inserts > 0U ||
            (cache.budget_bytes > 0U &&
                cache.used_bytes >= (cache.budget_bytes * 9U) / 10U);
        summary.dual_source_blend_factors_runtime_probe =
            m_dual_source_blend_factors_probe_completed;
        summary.dual_source_probe_shader_package_available =
            dual_source_probe_shader_package_available();
        summary.dual_source_blend_factors_available =
            m_dual_source_blend_factors_available;

        m_render_force_full_reupload =
            result.frame_build.full_repaint_fallback;
        m_render_non_dirty_state_invalidation =
            selection_changed    ||
            preedit_changed      ||
            options_changed      ||
            visual_bell_changed  ||
            summary.non_dirty_selection_invalidation   ||
            summary.non_dirty_cursor_invalidation      ||
            summary.non_dirty_preedit_invalidation     ||
            summary.non_dirty_options_invalidation     ||
            summary.non_dirty_visual_bell_invalidation ||
            font_epoch_changed;
        m_previous_render_state_keys      = current_keys;
        m_have_previous_render_state      = true;
        m_previous_render_font_epoch      = m_frame.font_epoch;
        m_have_previous_render_font_epoch = true;
    }

    void finalize_warm_lazy_summary(Atlas_prepare_result& result)
    {
        if (result.frame_build.glyph_missed_instances > 0 ||
            result.frame_build.glyph_coverage_failures > 0 ||
            result.frame_build.glyph_atlas_insert_failures > 0)
        {
            ++m_warm_lazy.incomplete_frames;
        }
        result.warm_lazy = m_warm_lazy;
    }

    void record_cache_insert_attempt(
        Atlas_cache_insert_source source,
        const QElapsedTimer&      timer,
        const Glyph_atlas_cache_stats& before,
        const Glyph_atlas_cache_stats& after)
    {
        const std::uint64_t inserted =
            after.inserts >= before.inserts
                ? after.inserts - before.inserts
                : 0U;
        const std::uint64_t failed =
            after.failed_inserts >= before.failed_inserts
                ? after.failed_inserts - before.failed_inserts
                : 0U;

        if (source == Atlas_cache_insert_source::WARM) {
            ++m_warm_lazy.warm_insert_attempts;
            m_warm_lazy.warm_inserts += static_cast<int>(inserted);
            m_warm_lazy.warm_failed_inserts += static_cast<int>(failed);
            return;
        }

        if (!m_current_prepare_had_lazy_insert) {
            m_current_prepare_had_lazy_insert = true;
            ++m_warm_lazy.lazy_frames;
        }
        ++m_warm_lazy.lazy_insert_attempts;
        m_warm_lazy.lazy_inserts += static_cast<int>(inserted);
        m_warm_lazy.lazy_failed_inserts += static_cast<int>(failed);
        const qint64 elapsed_ns = timer.nsecsElapsed();
        m_warm_lazy.lazy_elapsed_ms += static_cast<double>(elapsed_ns) / 1000000.0;
        const int elapsed_us = static_cast<int>((elapsed_ns + 999) / 1000);
        m_warm_lazy.lazy_max_insert_us =
            std::max(m_warm_lazy.lazy_max_insert_us, elapsed_us);
    }

    void record_glyph_face(
        const QString&          face_id,
        Atlas_prepare_result&  result)
    {
        if (face_id.isEmpty()) {
            return;
        }

        result.glyph_face_ids.insert(face_id);
        if (!result.base_face_id.isEmpty() && face_id != result.base_face_id) {
            result.fallback_face_ids.insert(face_id);
        }
    }

    void record_first_glyph_miss(
        Qsg_atlas_frame_build_summary&       frame_build,
        const Qsg_atlas_shaped_glyph_record& record,
        const QImage&                        image,
        Glyph_image_presentation             presentation,
        Qsg_atlas_glyph_miss_cause           cause,
        const Glyph_rgba_tile*               tile = nullptr)
    {
        if (frame_build.first_glyph_miss.valid) {
            return;
        }

        Qsg_atlas_glyph_miss_diagnostic& miss =
            frame_build.first_glyph_miss;
        miss.valid = true;
        miss.cause = cause;
        miss.image = qsg_atlas_glyph_image_diagnostic_from_record(
            record,
            image,
            presentation);
        if (tile != nullptr) {
            miss.tile_size           = tile->size;
            miss.tile_bytes_per_line = tile->bytes_per_line;
        }
        const Glyph_atlas_cache_stats cache_stats = m_cache.stats();
        miss.atlas_page_count  = cache_stats.page_count;
        miss.atlas_page_budget = cache_stats.page_budget;
        miss.atlas_page_size   = cache_stats.page_size;
    }

    Glyph_atlas_slot glyph_slot_for_index(
        const Qsg_atlas_shaped_glyph_record& record,
        QRawFont&                raster_font,
        Glyph_image_presentation presentation,
        Atlas_prepare_result&   result,
        Atlas_cache_insert_source source =
            Atlas_cache_insert_source::VISIBLE_LAZY)
    {
        const Glyph_coverage_kind_candidates candidates =
            qsg_atlas_cache_lookup_candidates(presentation);
        for (int index = 0; index < candidates.count; ++index) {
            const Glyph_atlas_cache_key key = qsg_atlas_cache_key(
                record.glyph_index,
                record.fallback_face_id,
                record.physical_pixel_size,
                0,
                candidates.kinds[static_cast<std::size_t>(index)],
                presentation);
            if (const Glyph_atlas_slot* cached_slot = m_cache.find(key);
                cached_slot != nullptr)
            {
                if (source == Atlas_cache_insert_source::WARM) {
                    ++m_warm_lazy.warm_cache_hits;
                }
                return *cached_slot;
            }
        }

        QElapsedTimer insert_timer;
        insert_timer.start();
        const QPoint physical_offset =
            qsg_atlas_glyph_physical_offset_for_raster_font(
                raster_font,
                record.glyph_index,
                presentation);

        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::rasterize_glyph");
        result.raster_thread = current_thread_id();
        const QRawFont::AntialiasingType antialiasing =
            presentation == Glyph_image_presentation::TEXT
                ? QRawFont::SubPixelAntialiasing
                : QRawFont::PixelAntialiasing;
        const QImage alpha_map = raster_font.alphaMapForGlyph(
            record.glyph_index,
            antialiasing);
        const Glyph_rgba_tile tile =
            qsg_atlas_rgba_tile_from_image(alpha_map, presentation);
        if (!tile.is_valid()) {
            if (source == Atlas_cache_insert_source::WARM) {
                ++m_warm_lazy.warm_unsupported_images;
                ++m_warm_lazy.warm_failed_glyph_records;
            }
            else {
                record_rejected_glyph_image(
                    result.frame_build.glyph_coverage,
                    tile.coverage_kind);
                record_first_glyph_miss(
                    result.frame_build,
                    record,
                    alpha_map,
                    presentation,
                    Qsg_atlas_glyph_miss_cause::UNSUPPORTED_IMAGE,
                    &tile);
                ++result.frame_build.glyph_coverage_failures;
                ++result.frame_build.glyph_missed_instances;
            }
            return {};
        }

        const Glyph_atlas_cache_key key = qsg_atlas_cache_key(
            record.glyph_index,
            record.fallback_face_id,
            record.physical_pixel_size,
            0,
            tile.coverage_kind,
            presentation);
        const Glyph_atlas_cache_stats before_insert = m_cache.stats();
        const Glyph_atlas_slot slot = m_cache.insert_or_get(
            key,
            tile,
            physical_offset);
        const Glyph_atlas_cache_stats after_insert = m_cache.stats();
        record_cache_insert_attempt(
            source,
            insert_timer,
            before_insert,
            after_insert);
        if (slot.is_valid()) {
            ++result.rasterized_glyphs;
        }
        else {
            if (source == Atlas_cache_insert_source::WARM) {
                ++m_warm_lazy.warm_failed_glyph_records;
            }
            else {
                record_first_glyph_miss(
                    result.frame_build,
                    record,
                    alpha_map,
                    presentation,
                    Qsg_atlas_glyph_miss_cause::ATLAS_INSERT_FAILED,
                    &tile);
                ++result.frame_build.glyph_atlas_insert_failures;
                ++result.frame_build.glyph_missed_instances;
            }
        }
        return slot;
    }

    void append_glyph_instance(
        const Glyph_atlas_slot&        slot,
        QPointF                        glyph_origin,
        const Terminal_render_text_run& run,
        const std::array<float, 4>&     color,
        qreal                          device_pixel_ratio,
        qreal                          inverse_page_width,
        qreal                          inverse_page_height,
        Qsg_atlas_frame_build_summary& frame_build,
        int*                           out_appended_instances)
    {
        const qreal normalized_device_pixel_ratio =
            atlas_normalized_device_pixel_ratio(device_pixel_ratio);
        if (!atlas_physical_origin_is_snapped(
                glyph_origin,
                normalized_device_pixel_ratio))
        {
            ++frame_build.snapped_origin_failures;
        }

        const QPointF snapped_glyph_origin =
            qsg_atlas_snapped_physical_point(
                glyph_origin,
                normalized_device_pixel_ratio);

        QRectF glyph_rect = qsg_atlas_snapped_glyph_draw_rect(
            snapped_glyph_origin,
            slot.physical_offset,
            slot.rect.size(),
            normalized_device_pixel_ratio);
        QRectF uv_rect(
            static_cast<qreal>(slot.rect.x())      * inverse_page_width,
            static_cast<qreal>(slot.rect.y())      * inverse_page_height,
            static_cast<qreal>(slot.rect.width())  * inverse_page_width,
            static_cast<qreal>(slot.rect.height()) * inverse_page_height);
        if (run.clip_rect.isValid() &&
            !clip_glyph_instance(glyph_rect, uv_rect, run.clip_rect))
        {
            return;
        }

        record_accepted_glyph_image(frame_build.glyph_coverage, slot.coverage_kind);
        frame_build.max_glyph_instance_page =
            std::max(frame_build.max_glyph_instance_page, slot.page);
        atlas_glyph_instance_t instance;
        store_rect(instance.rect, glyph_rect);
        store_uv_rect(instance.uv_rect, uv_rect);
        store_color(instance.color, color);
        store_atlas_info(instance.atlas_info, slot.coverage_kind, slot.page);
        m_glyph_instances.push_back(instance);
        m_glyph_instance_rows.push_back(
            run.row >= 0 && run.row < m_render_row_count
                ? run.row
                : k_qsg_atlas_non_row);
        if (out_appended_instances != nullptr) {
            ++*out_appended_instances;
        }
    }

    void begin_prepared_text_cache_frame(bool font_epoch_changed)
    {
        ++m_prepared_text_cache_frame;
        if (font_epoch_changed) {
            m_prepared_text_cache.clear();
        }
    }

    void prune_prepared_text_cache(Qsg_atlas_producer_summary& producer)
    {
        for (auto it = m_prepared_text_cache.begin();
            it != m_prepared_text_cache.end();)
        {
            if (it->second.last_seen_frame == m_prepared_text_cache_frame) {
                ++it;
                continue;
            }

            it = m_prepared_text_cache.erase(it);
            ++producer.shape_cache_pruned;
        }
        producer.shape_cache_entries =
            static_cast<int>(m_prepared_text_cache.size());
    }

    bool simple_text_cache_matches(
        qreal normalized_device_pixel_ratio,
        const QString& font_key) const
    {
        return
            m_simple_text_cache.initialized &&
            m_simple_text_cache.font_epoch == m_frame.font_epoch &&
            m_simple_text_cache.font_key == font_key &&
            qsg_atlas_cell_metric_equal(
                m_simple_text_cache.device_pixel_ratio,
                normalized_device_pixel_ratio) &&
            qsg_atlas_cell_metrics_equal(
                m_simple_text_cache.cell_metrics,
                m_frame.cell_metrics);
    }

    bool simple_text_cache_matches_stability_probe(
        const Terminal_render_text_run&          probe,
        const Qsg_atlas_shaped_text_run_result& shaped) const
    {
        if (shaped.missing_string_indexes != 0 ||
            shaped.invalid_string_indexes != 0)
        {
            return false;
        }

        std::vector<unsigned char> seen(
            static_cast<std::size_t>(probe.text.size()));
        for (const Qsg_atlas_shaped_glyph_record& record : shaped.records) {
            if (record.source_string_start < 0 ||
                record.source_string_start >= probe.text.size() ||
                record.source_string_end != record.source_string_start + 1)
            {
                return false;
            }

            const int index = qsg_atlas_printable_ascii_index(
                probe.text.at(record.source_string_start));
            if (index < 0 || index >= k_atlas_printable_ascii_count) {
                return false;
            }

            const Simple_atlas_glyph_template& glyph =
                m_simple_text_cache.glyphs[static_cast<std::size_t>(index)];
            const Qsg_atlas_shaped_glyph_record& template_record =
                glyph.record;
            if (record.glyph_index != template_record.glyph_index ||
                record.fallback_face_id != template_record.fallback_face_id ||
                !qsg_atlas_cell_metric_equal(
                    record.physical_pixel_size,
                    template_record.physical_pixel_size))
            {
                return false;
            }

            const qreal expected_origin_x =
                static_cast<qreal>(record.source_string_start) *
                    m_frame.cell_metrics.width;
            if (std::abs(record.glyph_origin.x() - expected_origin_x) > 0.001 ||
                std::abs(record.glyph_origin.y() - probe.baseline_origin.y()) >
                    0.001)
            {
                return false;
            }

            seen[static_cast<std::size_t>(record.source_string_start)] = 1U;
        }

        for (unsigned char entry : seen) {
            if (entry == 0U) {
                return false;
            }
        }

        return true;
    }

    bool ensure_simple_text_cache(Atlas_prepare_result& result)
    {
        const qreal normalized_device_pixel_ratio =
            atlas_normalized_device_pixel_ratio(m_frame.device_pixel_ratio);
        const QFont ascii_font =
            qsg_atlas_cell_stable_ascii_layout_font(m_frame.font);
        const QString font_key = ascii_font.key();
        if (simple_text_cache_matches(normalized_device_pixel_ratio, font_key)) {
            return m_simple_text_cache.usable;
        }

        m_simple_text_cache = {};
        m_simple_text_cache.initialized        = true;
        m_simple_text_cache.font_epoch         = m_frame.font_epoch;
        m_simple_text_cache.font_key           = font_key;
        m_simple_text_cache.device_pixel_ratio = normalized_device_pixel_ratio;
        m_simple_text_cache.cell_metrics       = m_frame.cell_metrics;

        Terminal_render_text_run probe;
        probe.row                 = 0;
        probe.logical_row         = 0;
        probe.column              = 0;
        probe.rect                = QRectF(
            0.0,
            0.0,
            static_cast<qreal>(k_atlas_printable_ascii_count) *
                m_frame.cell_metrics.width,
            m_frame.cell_metrics.height);
        probe.baseline_origin     = QPointF(0.0, m_frame.cell_metrics.ascent);
        probe.text                = qsg_atlas_printable_ascii_probe_text();
        probe.foreground          = QColor(Qt::white);
        probe.background          = QColor(Qt::transparent);

        ++result.render.shaped_text_runs;
        ++result.producer.shaped_runs_built;
        const Qsg_atlas_shaped_text_run_result shaped =
            qsg_atlas_shape_text_run(
                probe,
                ascii_font,
                m_frame.cell_metrics,
                normalized_device_pixel_ratio,
                -1,
                false);
        result.render.shaped_glyph_records +=
            static_cast<int>(shaped.records.size());
        result.producer.shaped_glyph_records_built +=
            static_cast<int>(shaped.records.size());
        result.render.shaped_missing_string_indexes +=
            shaped.missing_string_indexes;
        result.render.shaped_invalid_string_indexes +=
            shaped.invalid_string_indexes;

        std::array<unsigned char, k_atlas_printable_ascii_count> seen = {};
        bool usable = shaped.missing_string_indexes == 0 &&
            shaped.invalid_string_indexes == 0;
        for (const Qsg_atlas_shaped_glyph_record& record : shaped.records) {
            if (!usable) {
                break;
            }
            if (record.source_string_start < 0 ||
                record.source_string_start >= probe.text.size() ||
                record.source_string_end != record.source_string_start + 1)
            {
                usable = false;
                break;
            }

            const int index = qsg_atlas_printable_ascii_index(
                probe.text.at(record.source_string_start));
            if (index < 0 ||
                index >= k_atlas_printable_ascii_count ||
                seen[static_cast<std::size_t>(index)] != 0U)
            {
                usable = false;
                break;
            }

            const qreal expected_origin_x =
                static_cast<qreal>(index) * m_frame.cell_metrics.width;
            if (std::abs(record.glyph_origin.x() - expected_origin_x) > 0.001 ||
                std::abs(record.glyph_origin.y() - probe.baseline_origin.y()) >
                    0.001)
            {
                usable = false;
                break;
            }

            Simple_atlas_glyph_template& glyph =
                m_simple_text_cache.glyphs[static_cast<std::size_t>(index)];
            glyph.record   = record;
            glyph.drawable =
                record.glyph_bounds.width() > 0.0 &&
                record.glyph_bounds.height() > 0.0;
            seen[static_cast<std::size_t>(index)] = 1U;
        }

        for (int index = 0; index < k_atlas_printable_ascii_count; ++index) {
            const ushort codepoint = static_cast<ushort>(
                k_atlas_printable_ascii_first + index);
            if (codepoint == 0x20U) {
                continue;
            }
            usable = usable &&
                seen[static_cast<std::size_t>(index)] != 0U;
        }

        if (usable) {
            Terminal_render_text_run stability_probe = probe;
            stability_probe.text = qsg_atlas_printable_ascii_stability_probe_text();
            stability_probe.rect.setWidth(
                static_cast<qreal>(stability_probe.text.size()) *
                    m_frame.cell_metrics.width);

            ++result.render.shaped_text_runs;
            ++result.producer.shaped_runs_built;
            const Qsg_atlas_shaped_text_run_result stability_shaped =
                qsg_atlas_shape_text_run(
                    stability_probe,
                    ascii_font,
                    m_frame.cell_metrics,
                    normalized_device_pixel_ratio,
                    -1,
                    false);
            result.render.shaped_glyph_records +=
                static_cast<int>(stability_shaped.records.size());
            result.producer.shaped_glyph_records_built +=
                static_cast<int>(stability_shaped.records.size());
            result.render.shaped_missing_string_indexes +=
                stability_shaped.missing_string_indexes;
            result.render.shaped_invalid_string_indexes +=
                stability_shaped.invalid_string_indexes;

            usable = simple_text_cache_matches_stability_probe(
                stability_probe,
                stability_shaped);
        }

        m_simple_text_cache.usable = usable;
        return m_simple_text_cache.usable;
    }

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
    bool msdf_text_cache_matches(
        qreal normalized_device_pixel_ratio,
        int   pixel_height) const
    {
        return
            m_msdf_text_cache.initialized &&
            m_msdf_text_cache.font_epoch == m_frame.font_epoch &&
            m_msdf_text_cache.pixel_height == pixel_height &&
            qsg_atlas_cell_metric_equal(
                m_msdf_text_cache.device_pixel_ratio,
                normalized_device_pixel_ratio) &&
            qsg_atlas_cell_metrics_equal(
                m_msdf_text_cache.cell_metrics,
                m_frame.cell_metrics);
    }

    bool ensure_msdf_text_cache(Atlas_prepare_result& result)
    {
        const qreal normalized_device_pixel_ratio =
            atlas_normalized_device_pixel_ratio(m_frame.device_pixel_ratio);
        const int pixel_height = std::max(
            1,
            atlas_snapped_physical_int(
                m_frame.cell_metrics.ascent,
                normalized_device_pixel_ratio));
        if (msdf_text_cache_matches(normalized_device_pixel_ratio, pixel_height)) {
            result.render.msdf_text_atlas_built = m_msdf_text_cache.atlas_built;
            result.render.msdf_text_atlas_ready = m_msdf_text_cache.ready;
            return m_msdf_text_cache.ready;
        }

        const std::uint64_t next_generation = m_msdf_text_cache.generation + 1U;
        m_msdf_text_cache = {};
        m_msdf_text_cache.generation         = next_generation;
        m_msdf_text_cache.initialized        = true;
        m_msdf_text_cache.font_epoch         = m_frame.font_epoch;
        m_msdf_text_cache.device_pixel_ratio = normalized_device_pixel_ratio;
        m_msdf_text_cache.cell_metrics       = m_frame.cell_metrics;
        m_msdf_text_cache.pixel_height       = pixel_height;
        m_msdf_text_uploaded_generation      = 0U;
        delete_resource(m_stencil_msdf_text_pipeline);
        delete_resource(m_msdf_text_pipeline);
        delete_resource(m_msdf_text_shader_resources);
        delete_resource(m_msdf_text_atlas_texture);

        QFile font_file(QString::fromLatin1(k_atlas_msdf_terminal_font_resource));
        if (!font_file.open(QIODevice::ReadOnly)) {
            m_msdf_text_cache.message = QStringLiteral(
                "failed to open embedded terminal monospace font: %1")
                .arg(font_file.errorString());
            result.render.msdf_text_atlas_built = false;
            result.render.msdf_text_atlas_ready = false;
            return false;
        }

        const QByteArray font_data = font_file.readAll();
        m_msdf_text_cache.font_data_bytes = static_cast<int>(font_data.size());
        if (font_data.isEmpty()) {
            m_msdf_text_cache.message =
                QStringLiteral("embedded terminal monospace font resource is empty");
            result.render.msdf_text_atlas_built = false;
            result.render.msdf_text_atlas_ready = false;
            return false;
        }

        const std::vector<char32_t>& codepoints = atlas_msdf_text_codepoints();
        msdf_text::build_result_t build = msdf_text::build_font_atlas(
            reinterpret_cast<const std::uint8_t*>(font_data.constData()),
            static_cast<std::size_t>(font_data.size()),
            pixel_height,
            std::span<const char32_t>(codepoints.data(), codepoints.size()),
            atlas_msdf_text_options());
        m_msdf_text_cache.atlas_built = build.ok;
        if (!build.ok) {
            m_msdf_text_cache.message = QString::fromStdString(build.message);
            result.render.msdf_text_atlas_built = false;
            result.render.msdf_text_atlas_ready = false;
            return false;
        }

        m_msdf_text_cache.atlas = std::move(build.atlas);
        m_msdf_text_cache.ready =
            atlas_msdf_text_atlas_has_printable_ascii(
                m_msdf_text_cache.atlas,
                &m_msdf_text_cache.message);
        if (m_msdf_text_cache.ready && m_msdf_text_cache.message.isEmpty()) {
            m_msdf_text_cache.message = QStringLiteral("ok");
        }
        result.render.msdf_text_atlas_built = m_msdf_text_cache.atlas_built;
        result.render.msdf_text_atlas_ready = m_msdf_text_cache.ready;
        return m_msdf_text_cache.ready;
    }

    void record_msdf_supported_text_miss(
        const Terminal_render_text_run& run,
        Atlas_prepare_result&           result,
        int                             missed_glyphs)
    {
        ++result.render.msdf_text_missed_supported_runs;
        result.render.msdf_text_missed_supported_glyphs +=
            missed_glyphs > 0
                ? missed_glyphs
                : qsg_atlas_printable_ascii_drawable_glyph_count(run.text);
    }

    void append_msdf_text_instance(
        const msdf_text_glyph_t&          glyph,
        QPointF                           baseline_origin,
        const Terminal_render_text_run&   run,
        const std::array<float, 4>&       color,
        qreal                             device_pixel_ratio)
    {
        const qreal normalized_device_pixel_ratio =
            atlas_normalized_device_pixel_ratio(device_pixel_ratio);
        const int physical_origin_x = atlas_snapped_physical_int(
            baseline_origin.x(),
            normalized_device_pixel_ratio);
        const int physical_origin_y = atlas_snapped_physical_int(
            baseline_origin.y(),
            normalized_device_pixel_ratio);
        const qreal physical_left =
            static_cast<qreal>(physical_origin_x) +
            static_cast<qreal>(glyph.plane_left);
        const qreal physical_top =
            static_cast<qreal>(physical_origin_y) +
            static_cast<qreal>(glyph.plane_bottom);
        const qreal physical_right =
            static_cast<qreal>(physical_origin_x) +
            static_cast<qreal>(glyph.plane_right);
        const qreal physical_bottom =
            static_cast<qreal>(physical_origin_y) +
            static_cast<qreal>(glyph.plane_top);
        if (physical_right <= physical_left || physical_bottom <= physical_top) {
            return;
        }
        QRectF glyph_rect(
            physical_left / normalized_device_pixel_ratio,
            physical_top / normalized_device_pixel_ratio,
            (physical_right - physical_left) /
                normalized_device_pixel_ratio,
            (physical_bottom - physical_top) /
                normalized_device_pixel_ratio);
        QRectF uv_rect(
            glyph.uv_left,
            glyph.uv_top,
            glyph.uv_right - glyph.uv_left,
            glyph.uv_bottom - glyph.uv_top);
        if (run.clip_rect.isValid() &&
            !clip_glyph_instance(glyph_rect, uv_rect, run.clip_rect))
        {
            return;
        }
        const qreal frame_left   = glyph_rect.left() * normalized_device_pixel_ratio;
        const qreal frame_top    = glyph_rect.top() * normalized_device_pixel_ratio;
        const qreal frame_width  = glyph_rect.width() * normalized_device_pixel_ratio;
        const qreal frame_height = glyph_rect.height() * normalized_device_pixel_ratio;

        atlas_msdf_instance_t instance;
        store_rect(instance.rect, glyph_rect);
        store_uv_rect(instance.uv_rect, uv_rect);
        store_color(instance.color, color);
        store_uv_bounds(
            instance.uv_bounds,
            glyph.uv_left,
            glyph.uv_top,
            glyph.uv_right,
            glyph.uv_bottom,
            m_msdf_text_cache.atlas.atlas_size);
        instance.frame_rect[0] = static_cast<float>(frame_left);
        instance.frame_rect[1] = static_cast<float>(frame_top);
        instance.frame_rect[2] = static_cast<float>(frame_width);
        instance.frame_rect[3] = static_cast<float>(frame_height);
        m_msdf_text_instances.push_back(instance);
        m_msdf_text_instance_rows.push_back(
            run.row >= 0 && run.row < m_render_row_count
                ? run.row
                : k_qsg_atlas_non_row);
    }

    void append_msdf_text_run(
        const Terminal_render_text_run& run,
        qreal                           opacity,
        Atlas_prepare_result&           result)
    {
        ++result.render.msdf_text_supported_runs;
        ++result.producer.presentation_fast_text_runs;

        if (!ensure_msdf_text_cache(result)) {
            record_msdf_supported_text_miss(run, result, 0);
            return;
        }

        const std::array<float, 4> color =
            atlas_glyph_color_components(run.foreground, opacity);
        int missed_glyphs = 0;
        for (qsizetype source = 0; source < run.text.size(); ++source) {
            const int index = qsg_atlas_printable_ascii_index(run.text.at(source));
            if (index < 0) {
                ++missed_glyphs;
                continue;
            }

            const char32_t codepoint = static_cast<char32_t>(
                k_atlas_printable_ascii_first + index);
            const auto glyph = m_msdf_text_cache.atlas.glyphs.find(codepoint);
            if (glyph == m_msdf_text_cache.atlas.glyphs.end()) {
                if (codepoint != U' ') {
                    ++missed_glyphs;
                }
                continue;
            }

            if (!atlas_msdf_text_glyph_is_drawable(glyph->second)) {
                if (codepoint != U' ') {
                    ++missed_glyphs;
                }
            }
        }

        if (missed_glyphs > 0) {
            record_msdf_supported_text_miss(run, result, missed_glyphs);
            return;
        }

        for (qsizetype source = 0; source < run.text.size(); ++source) {
            const int index = qsg_atlas_printable_ascii_index(run.text.at(source));
            const char32_t codepoint = static_cast<char32_t>(
                k_atlas_printable_ascii_first + index);
            const auto glyph = m_msdf_text_cache.atlas.glyphs.find(codepoint);
            if (glyph == m_msdf_text_cache.atlas.glyphs.end() ||
                !atlas_msdf_text_glyph_is_drawable(glyph->second))
            {
                continue;
            }
            append_msdf_text_instance(
                glyph->second,
                QPointF(
                    run.rect.left() +
                        static_cast<qreal>(source) * m_frame.cell_metrics.width,
                    run.baseline_origin.y()),
                run,
                color,
                m_frame.device_pixel_ratio);
        }

        ++result.render.msdf_text_runs;
    }
#endif

    bool append_simple_text_run(
        const Terminal_render_text_run& run,
        qreal                           opacity,
        Atlas_prepare_result&           result,
        int                             text_run_index,
        bool                            cursor_text_run)
    {
        if (!ensure_simple_text_cache(result)) {
            return false;
        }

        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        const QSize page_size = m_cache.stats().page_size;
        const qreal inverse_page_width =
            1.0 / static_cast<qreal>(std::max(1, page_size.width()));
        const qreal inverse_page_height =
            1.0 / static_cast<qreal>(std::max(1, page_size.height()));
        const std::array<float, 4> color =
            atlas_glyph_color_components(run.foreground, opacity);

        int reused_glyph_records = 0;
        for (qsizetype source = 0; source < run.text.size(); ++source) {
            const int index = qsg_atlas_printable_ascii_index(run.text.at(source));
            if (index < 0 || index >= k_atlas_printable_ascii_count) {
                return false;
            }

            Simple_atlas_glyph_template& glyph =
                m_simple_text_cache.glyphs[static_cast<std::size_t>(index)];
            if (!glyph.drawable) {
                continue;
            }

            Qsg_atlas_shaped_glyph_record record = glyph.record;
            record.text_run_index     = text_run_index;
            record.cursor_text_run    = cursor_text_run;
            record.row                = run.row;
            record.logical_row        = run.logical_row;
            record.retained_line_id   = run.retained_line_id;
            record.content_generation = run.content_generation;
            record.run_column         = run.column;
            record.owner_column       =
                run.column + static_cast<int>(source);
            record.owner_cell_span    = 1;
            record.source_string_start = source;
            record.source_string_end   = source + 1;
            record.glyph_origin        = QPointF(
                run.rect.left() +
                    static_cast<qreal>(source) * m_frame.cell_metrics.width,
                run.baseline_origin.y());
            record_glyph_face(record.fallback_face_id, result);
            if (!glyph.slot.is_valid()) {
                QRawFont raster_font = record.raw_font;
                raster_font.setPixelSize(record.physical_pixel_size);
                ++result.producer.slot_resolutions_built;
                glyph.slot = glyph_slot_for_index(
                    record,
                    raster_font,
                    Glyph_image_presentation::TEXT,
                    result);
                if (!glyph.slot.is_valid()) {
                    continue;
                }
            }
            else {
                ++result.producer.slot_resolutions_reused;
            }
            append_glyph_instance(
                glyph.slot,
                record.glyph_origin,
                run,
                color,
                device_pixel_ratio,
                inverse_page_width,
                inverse_page_height,
                result.frame_build,
                nullptr);
            ++reused_glyph_records;
        }

        ++result.producer.presentation_fast_text_runs;
        ++result.producer.simple_path_used;
        ++result.producer.shaped_runs_reused;
        result.producer.shaped_glyph_records_reused += reused_glyph_records;
        return true;
    }

    void append_prepared_text_run(
        const Prepared_atlas_text_run&  prepared,
        const Terminal_render_text_run& run,
        qreal                           opacity,
        Atlas_prepare_result&           result,
        int                             text_run_index,
        bool                            cursor_text_run)
    {
        if (prepared.glyphs.empty()) {
            return;
        }

        if (prepared.emoji_presentation_run) {
            ++result.frame_build.emoji_presentation_runs;
        }

        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        const QSize page_size = m_cache.stats().page_size;
        const qreal inverse_page_width =
            1.0 / static_cast<qreal>(std::max(1, page_size.width()));
        const qreal inverse_page_height =
            1.0 / static_cast<qreal>(std::max(1, page_size.height()));
        const std::array<float, 4> color =
            atlas_glyph_color_components(run.foreground, opacity);
        const QPointF origin_delta = run.baseline_origin - prepared.baseline_origin;

        for (const Prepared_atlas_glyph& glyph : prepared.glyphs) {
            Qsg_atlas_shaped_glyph_record record = glyph.record;
            record.text_run_index     = text_run_index;
            record.cursor_text_run    = cursor_text_run;
            record.row                = run.row;
            record.logical_row        = run.logical_row;
            record.retained_line_id   = run.retained_line_id;
            record.content_generation = run.content_generation;
            record.run_column         = run.column;
            record.glyph_origin      += origin_delta;
            record_glyph_face(record.fallback_face_id, result);
            ++result.producer.slot_resolutions_reused;
            append_glyph_instance(
                glyph.slot,
                record.glyph_origin,
                run,
                color,
                device_pixel_ratio,
                inverse_page_width,
                inverse_page_height,
                result.frame_build,
                nullptr);
        }

        ++result.producer.shaped_runs_reused;
        result.producer.shaped_glyph_records_reused +=
            static_cast<int>(prepared.glyphs.size());
    }

    void append_text_run(
        const Terminal_render_text_run&  run,
        qreal                            opacity,
        Atlas_prepare_result&           result,
        int                              text_run_index,
        bool                             cursor_text_run)
    {
        ++result.producer.text_runs_considered;
        if (run.text.isEmpty()) {
            ++result.producer.text_runs_empty;
            return;
        }

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (qsg_atlas_msdf_text_run_candidate(run, m_frame.cell_metrics) &&
            qsg_atlas_msdf_text_font_is_supported(m_frame.font))
        {
            (void)text_run_index;
            (void)cursor_text_run;
            append_msdf_text_run(
                run,
                opacity,
                result);
            return;
        }
#endif

        if (qsg_atlas_simple_text_run_candidate(run, m_frame.cell_metrics)) {
            ++result.producer.simple_path_attempts;
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
            if (qsg_atlas_msdf_text_font_is_supported(m_frame.font)) {
                (void)text_run_index;
                (void)cursor_text_run;
                append_msdf_text_run(
                    run,
                    opacity,
                    result);
                return;
            }
#endif
            if (append_simple_text_run(
                    run,
                    opacity,
                    result,
                    text_run_index,
                    cursor_text_run))
            {
                return;
            }
            ++result.producer.simple_path_fallbacks;
        }

        const bool cache_usable = prepared_text_cache_key_is_usable(run);
        const QByteArray cache_key = cache_usable
            ? prepared_text_cache_key(
                run,
                m_frame.font,
                m_frame.cell_metrics,
                m_frame.device_pixel_ratio,
                m_frame.font_epoch,
                cursor_text_run)
            : QByteArray();
        if (cache_usable) {
            ++result.producer.shape_cache_lookups;
            auto cached = m_prepared_text_cache.find(cache_key);
            if (cached != m_prepared_text_cache.end()) {
                ++result.producer.shape_cache_hits;
                cached->second.last_seen_frame = m_prepared_text_cache_frame;
                append_prepared_text_run(
                    cached->second,
                    run,
                    opacity,
                    result,
                    text_run_index,
                    cursor_text_run);
                return;
            }

            ++result.producer.shape_cache_misses;
        }

        const bool emoji_presentation_run =
            qsg_atlas_text_has_emoji_presentation(run.text, result.producer);
        if (emoji_presentation_run) {
            ++result.frame_build.emoji_presentation_runs;
            ++result.producer.presentation_emoji_runs;
        }

        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::append_shaped_text_run");
        ++result.render.shaped_text_runs;
        ++result.producer.shaped_runs_built;
        const Qsg_atlas_shaped_text_run_result shaped =
            qsg_atlas_shape_text_run(
                run,
                m_frame.font,
                m_frame.cell_metrics,
                m_frame.device_pixel_ratio,
                text_run_index,
                cursor_text_run);
        result.render.shaped_glyph_records +=
            static_cast<int>(shaped.records.size());
        result.producer.shaped_glyph_records_built +=
            static_cast<int>(shaped.records.size());
        result.render.shaped_missing_string_indexes +=
            shaped.missing_string_indexes;
        result.render.shaped_invalid_string_indexes +=
            shaped.invalid_string_indexes;
        if (shaped.records.empty()) {
            return;
        }

        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        const QSize page_size = m_cache.stats().page_size;
        const qreal inverse_page_width =
            1.0 / static_cast<qreal>(std::max(1, page_size.width()));
        const qreal inverse_page_height =
            1.0 / static_cast<qreal>(std::max(1, page_size.height()));
        const std::array<float, 4> color =
            atlas_glyph_color_components(run.foreground, opacity);
        Prepared_atlas_text_run prepared;
        prepared.baseline_origin        = run.baseline_origin;
        prepared.last_seen_frame        = m_prepared_text_cache_frame;
        prepared.emoji_presentation_run = emoji_presentation_run;
        bool cacheable = cache_usable;
        for (const Qsg_atlas_shaped_glyph_record& record : shaped.records) {
            if (record.glyph_bounds.width() <= 0.0 ||
                record.glyph_bounds.height() <= 0.0)
            {
                continue;
            }

            QRawFont raster_font = record.raw_font;
            raster_font.setPixelSize(record.physical_pixel_size);
            record_glyph_face(record.fallback_face_id, result);
            Glyph_image_presentation presentation = Glyph_image_presentation::TEXT;
            if (emoji_presentation_run) {
                ++result.producer.presentation_source_scans;
                presentation = glyph_image_presentation_for_source_range(
                    run.text,
                    record.source_string_start,
                    record.source_string_end);
            }
            ++result.producer.slot_resolutions_built;
            const Glyph_atlas_slot slot = glyph_slot_for_index(
                record,
                raster_font,
                presentation,
                result);
            if (!slot.is_valid()) {
                cacheable = false;
                continue;
            }

            append_glyph_instance(
                slot,
                record.glyph_origin,
                run,
                color,
                device_pixel_ratio,
                inverse_page_width,
                inverse_page_height,
                result.frame_build,
                nullptr);
            prepared.glyphs.push_back({record, presentation, slot});
        }

        if (cacheable) {
            m_prepared_text_cache[cache_key] = std::move(prepared);
            ++result.producer.shape_cache_inserts;
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
        bool                coverage_dirty,
        bool*               out_coverage_texture_created,
        bool*               out_coverage_upload_recorded)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::upload_coverage_texture");

        if (m_cache.stats().page_count <= 0) {
            if (out_coverage_upload_recorded != nullptr) {
                *out_coverage_upload_recorded = false;
            }
            return true;
        }

        bool texture_created = false;
        const bool texture_ready = ensure_coverage_texture(rhi, &texture_created);
        if (out_coverage_texture_created != nullptr) {
            *out_coverage_texture_created =
                texture_ready && m_coverage_texture != nullptr &&
                m_coverage_texture->format() == QRhiTexture::RGBA8;
        }
        if (!texture_ready) {
            return false;
        }
        if (!coverage_dirty && !texture_created) {
            if (out_coverage_upload_recorded != nullptr) {
                *out_coverage_upload_recorded = false;
            }
            return true;
        }

        std::vector<QRhiTextureUploadEntry> entries;
        const Glyph_atlas_cache_stats cache_stats = m_cache.stats();
        entries.reserve(static_cast<std::size_t>(cache_stats.page_count));
        for (int page = 0; page < cache_stats.page_count; ++page) {
            const QByteArray& page_bytes = m_cache.page_bytes(page);
            QRhiTextureSubresourceUploadDescription subresource(page_bytes);
            subresource.setDataStride(
                static_cast<quint32>(cache_stats.page_size.width() * 4));
            subresource.setSourceSize(cache_stats.page_size);
            entries.emplace_back(page, 0, subresource);
        }

        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        QRhiTextureUploadDescription upload;
        upload.setEntries(entries.begin(), entries.end());
        updates->uploadTexture(m_coverage_texture, upload);
        command_buffer->resourceUpdate(updates);

        if (out_coverage_upload_recorded != nullptr) {
            *out_coverage_upload_recorded = true;
        }
        return true;
    }

    bool ensure_dynamic_buffer(
        QRhi*                 rhi,
        QRhiBuffer*&          buffer,
        quint32&              current_size,
        quint32               required_size,
        QRhiBuffer::UsageFlag usage,
        bool*                 out_recreated = nullptr)
    {
        if (out_recreated != nullptr) {
            *out_recreated = false;
        }
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
        if (out_recreated != nullptr) {
            *out_recreated = true;
        }
        return true;
    }

    QByteArray rect_instance_layout_key() const
    {
        QByteArray key;
        append_pass_key(key, m_background_pass);
        append_pass_key(key, m_selection_pass);
        append_pass_key(key, m_graphic_pass);
        append_pass_key(key, m_decoration_pass);
        append_pass_key(key, m_cursor_pass);
        append_pass_key(key, m_overlay_pass);
        return key;
    }

    QByteArray glyph_instance_layout_key() const
    {
        QByteArray key;
        append_pass_key(key, m_text_pass);
        append_key_int_vector(key, m_render_glyph_text_row_capacities);
        append_pass_key(key, m_cursor_text_pass);
        append_key_int_vector(key, m_render_glyph_cursor_text_row_capacities);
        return key;
    }

    QByteArray msdf_text_instance_layout_key() const
    {
        QByteArray key;
        append_pass_key(key, m_msdf_text_pass);
        append_pass_key(key, m_msdf_cursor_text_pass);
        return key;
    }

    bool update_atlas_buffers(
        QRhi*                             rhi,
        QRhiCommandBuffer*                command_buffer,
        QSize                             render_target_size,
        Qsg_atlas_render_summary*         render_summary)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::update_atlas_buffers");

        const quint32 rect_buffer_size = static_cast<quint32>(
            std::max<std::size_t>(1U, m_rect_instances.size()) * sizeof(atlas_instance_t));
        const quint32 glyph_buffer_size = static_cast<quint32>(
            std::max<std::size_t>(1U, m_glyph_buffer_instances.size()) *
                sizeof(atlas_glyph_instance_t));
        bool rect_buffer_recreated  = false;
        bool glyph_buffer_recreated = false;
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        const quint32 msdf_text_buffer_size = static_cast<quint32>(
            std::max<std::size_t>(1U, m_msdf_text_instances.size()) *
                sizeof(atlas_msdf_instance_t));
        bool msdf_text_buffer_recreated = false;
#endif
        if (!ensure_dynamic_buffer(
                rhi,
                m_rect_instance_buffer,
                m_rect_instance_buffer_size,
                rect_buffer_size,
                QRhiBuffer::VertexBuffer,
                &rect_buffer_recreated))
        {
            return false;
        }
        if (rect_buffer_recreated) {
            m_rect_upload_planner.reset();
        }

        if (!ensure_dynamic_buffer(
                rhi,
                m_glyph_instance_buffer,
                m_glyph_instance_buffer_size,
                glyph_buffer_size,
                QRhiBuffer::VertexBuffer,
                &glyph_buffer_recreated))
        {
            return false;
        }
        if (glyph_buffer_recreated) {
            m_glyph_upload_planner.reset();
        }

#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (!ensure_dynamic_buffer(
                rhi,
                m_msdf_text_instance_buffer,
                m_msdf_text_instance_buffer_size,
                msdf_text_buffer_size,
                QRhiBuffer::VertexBuffer,
                &msdf_text_buffer_recreated))
        {
            return false;
        }
        if (msdf_text_buffer_recreated) {
            m_msdf_text_upload_planner.reset();
        }
#endif

        const int frames_in_flight =
            std::max(1, rhi->resourceLimit(QRhi::FramesInFlight));
        const int frame_slot =
            std::clamp(rhi->currentFrameSlot(), 0, frames_in_flight - 1);
        const int rect_byte_count = static_cast<int>(
            m_rect_instances.size() * sizeof(atlas_instance_t));
        const int glyph_byte_count = static_cast<int>(
            m_glyph_buffer_instances.size() * sizeof(atlas_glyph_instance_t));
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        const int msdf_text_byte_count = static_cast<int>(
            m_msdf_text_instances.size() * sizeof(atlas_msdf_instance_t));
#endif
        const Qsg_atlas_buffer_update_plan rect_plan =
            m_rect_upload_planner.plan({
                frames_in_flight,
                frame_slot,
                m_render_row_count,
                static_cast<int>(sizeof(atlas_instance_t)),
                !m_rect_instances.empty()
                    ? reinterpret_cast<const char*>(m_rect_instances.data())
                    : nullptr,
                rect_byte_count,
                &m_rect_instance_rows,
                rect_instance_layout_key(),
                m_render_dirty_row_ranges,
                rect_buffer_recreated,
                m_render_force_full_reupload,
                m_render_non_dirty_state_invalidation,
                -1,
                false,
            });
        const Qsg_atlas_buffer_update_plan glyph_plan =
            m_glyph_upload_planner.plan({
                frames_in_flight,
                frame_slot,
                m_render_row_count,
                static_cast<int>(sizeof(atlas_glyph_instance_t)),
                !m_glyph_buffer_instances.empty()
                    ? reinterpret_cast<const char*>(m_glyph_buffer_instances.data())
                    : nullptr,
                glyph_byte_count,
                &m_glyph_buffer_instance_rows,
                glyph_instance_layout_key(),
                m_render_dirty_row_ranges,
                glyph_buffer_recreated,
                m_render_force_full_reupload,
                m_render_non_dirty_state_invalidation,
                static_cast<int>(m_glyph_instances.size()),
                !m_glyph_buffer_instances.empty(),
                &m_glyph_buffer_row_stable_ranges,
            });
        Qsg_atlas_buffer_update_plan msdf_text_plan;
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        msdf_text_plan = m_msdf_text_upload_planner.plan({
                frames_in_flight,
                frame_slot,
                m_render_row_count,
                static_cast<int>(sizeof(atlas_msdf_instance_t)),
                !m_msdf_text_instances.empty()
                    ? reinterpret_cast<const char*>(m_msdf_text_instances.data())
                    : nullptr,
                msdf_text_byte_count,
                &m_msdf_text_instance_rows,
                msdf_text_instance_layout_key(),
                m_render_dirty_row_ranges,
                msdf_text_buffer_recreated,
                m_render_force_full_reupload,
                m_render_non_dirty_state_invalidation,
                static_cast<int>(m_msdf_text_instances.size()),
                false,
            });
#endif
        if (render_summary != nullptr) {
            render_summary->rect_buffer      = rect_plan.summary;
            render_summary->glyph_buffer     = glyph_plan.summary;
            render_summary->msdf_text_buffer = msdf_text_plan.summary;
        }

        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        if (m_static_vertex_upload_needed) {
            updates->uploadStaticBuffer(
                m_vertex_buffer,
                k_atlas_quad_vertices.data());
            m_static_vertex_upload_needed = false;
        }

        atlas_uniform_t uniform;
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
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        if (m_msdf_text_uniform_buffer != nullptr) {
            atlas_msdf_uniform_t msdf_uniform;
            std::copy(
                mvp.constData(),
                mvp.constData() + 16,
                std::begin(msdf_uniform.matrix));
            msdf_uniform.px_range      = m_msdf_text_cache.atlas.px_range;
            msdf_uniform.target_width  = static_cast<float>(
                std::max(1, render_target_size.width()));
            msdf_uniform.target_height = static_cast<float>(
                std::max(1, render_target_size.height()));
            updates->updateDynamicBuffer(
                m_msdf_text_uniform_buffer,
                0U,
                sizeof(msdf_uniform),
                &msdf_uniform);
        }
#endif
        for (const Qsg_atlas_buffer_update_range& range : rect_plan.ranges) {
            updates->updateDynamicBuffer(
                m_rect_instance_buffer,
                static_cast<quint32>(range.byte_offset),
                static_cast<quint32>(range.byte_count),
                reinterpret_cast<const char*>(m_rect_instances.data()) +
                    range.byte_offset);
        }
        for (const Qsg_atlas_buffer_update_range& range : glyph_plan.ranges) {
            updates->updateDynamicBuffer(
                m_glyph_instance_buffer,
                static_cast<quint32>(range.byte_offset),
                static_cast<quint32>(range.byte_count),
                reinterpret_cast<const char*>(m_glyph_buffer_instances.data()) +
                    range.byte_offset);
        }
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
        for (const Qsg_atlas_buffer_update_range& range : msdf_text_plan.ranges) {
            updates->updateDynamicBuffer(
                m_msdf_text_instance_buffer,
                static_cast<quint32>(range.byte_offset),
                static_cast<quint32>(range.byte_count),
                reinterpret_cast<const char*>(m_msdf_text_instances.data()) +
                    range.byte_offset);
        }
#endif

        command_buffer->resourceUpdate(updates);
        return true;
    }

    void draw_rect_pass(
        QRhiCommandBuffer*        command_buffer,
        const atlas_pass_range_t&  pass,
        bool                      stencil_enabled)
    {
        if (!pass.has_instances()) {
            return;
        }

        command_buffer->setGraphicsPipeline(
            stencil_enabled ? m_stencil_rect_pipeline : m_rect_pipeline);
        command_buffer->setShaderResources(m_rect_shader_resources);
        const quint32 instance_offset = pass.first * sizeof(atlas_instance_t);
        const QRhiCommandBuffer::VertexInput bindings[] = {
            {m_vertex_buffer,          0U},
            {m_rect_instance_buffer,   instance_offset},
        };
        command_buffer->setVertexInput(0, 2, bindings);
        command_buffer->draw(
            static_cast<quint32>(k_atlas_quad_vertices.size()),
            pass.count);
    }

    void draw_glyph_pass(
        QRhiCommandBuffer*        command_buffer,
        const atlas_pass_range_t&  pass,
        bool                      stencil_enabled)
    {
        if (!pass.has_instances()) {
            return;
        }

        command_buffer->setGraphicsPipeline(
            stencil_enabled ? m_stencil_glyph_pipeline : m_glyph_pipeline);
        command_buffer->setShaderResources(m_glyph_shader_resources);
        const quint32 instance_offset = pass.first * sizeof(atlas_glyph_instance_t);
        const QRhiCommandBuffer::VertexInput bindings[] = {
            {m_vertex_buffer,          0U},
            {m_glyph_instance_buffer,  instance_offset},
        };
        command_buffer->setVertexInput(0, 2, bindings);
        command_buffer->draw(
            static_cast<quint32>(k_atlas_quad_vertices.size()),
            pass.count);
    }

    void draw_msdf_text_pass(
        QRhiCommandBuffer*        command_buffer,
        const atlas_pass_range_t&  pass,
        bool                      stencil_enabled)
    {
        if (!pass.has_instances()) {
            return;
        }

        command_buffer->setGraphicsPipeline(
            stencil_enabled
                ? m_stencil_msdf_text_pipeline
                : m_msdf_text_pipeline);
        command_buffer->setShaderResources(m_msdf_text_shader_resources);
        const quint32 instance_offset = pass.first * sizeof(atlas_msdf_instance_t);
        const QRhiCommandBuffer::VertexInput bindings[] = {
            {m_vertex_buffer,               0U},
            {m_msdf_text_instance_buffer,   instance_offset},
        };
        command_buffer->setVertexInput(0, 2, bindings);
        command_buffer->draw(
            static_cast<quint32>(k_atlas_quad_vertices.size()),
            pass.count);
    }

    bool has_glyph_draw_passes() const
    {
        return m_text_pass.has_instances() || m_cursor_text_pass.has_instances();
    }

    bool has_msdf_text_draw_passes() const
    {
        return
            m_msdf_text_pass.has_instances() ||
            m_msdf_cursor_text_pass.has_instances();
    }

    quint32 total_instance_count() const
    {
        return
            static_cast<quint32>(m_rect_instances.size()) +
            static_cast<quint32>(m_glyph_instances.size()) +
            static_cast<quint32>(m_msdf_text_instances.size());
    }

    Captured_atlas_frame                     m_frame;
    std::shared_ptr<Qsg_atlas_recorder>
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
    QShader                                  m_dual_source_probe_fragment_shader;
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
    QShader                                  m_msdf_text_vertex_shader;
    QShader                                  m_msdf_text_fragment_shader;
#endif
    std::vector<atlas_instance_t>             m_rect_instances;
    std::vector<atlas_glyph_instance_t>       m_glyph_instances;
    std::vector<atlas_glyph_instance_t>       m_glyph_buffer_instances;
    std::vector<atlas_msdf_instance_t>        m_msdf_text_instances;
    std::vector<int>                         m_rect_instance_rows;
    std::vector<int>                         m_glyph_instance_rows;
    std::vector<int>                         m_glyph_buffer_instance_rows;
    std::vector<int>                         m_msdf_text_instance_rows;
    std::vector<Qsg_atlas_row_stable_range>
                                             m_glyph_buffer_row_stable_ranges;
    atlas_pass_range_t                        m_background_pass;
    atlas_pass_range_t                        m_selection_pass;
    atlas_pass_range_t                        m_graphic_pass;
    atlas_pass_range_t                        m_text_pass;
    atlas_pass_range_t                        m_msdf_text_pass;
    atlas_pass_range_t                        m_decoration_pass;
    atlas_pass_range_t                        m_cursor_pass;
    atlas_pass_range_t                        m_cursor_text_pass;
    atlas_pass_range_t                        m_msdf_cursor_text_pass;
    atlas_pass_range_t                        m_overlay_pass;
    QRhiBuffer*                              m_vertex_buffer = nullptr;
    QRhiBuffer*                              m_rect_instance_buffer = nullptr;
    QRhiBuffer*                              m_glyph_instance_buffer = nullptr;
    QRhiBuffer*                              m_msdf_text_instance_buffer = nullptr;
    QRhiBuffer*                              m_uniform_buffer = nullptr;
    QRhiBuffer*                              m_msdf_text_uniform_buffer = nullptr;
    QRhiShaderResourceBindings*              m_rect_shader_resources = nullptr;
    QRhiShaderResourceBindings*              m_glyph_shader_resources = nullptr;
    QRhiShaderResourceBindings*              m_msdf_text_shader_resources = nullptr;
    QRhiGraphicsPipeline*                    m_rect_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_stencil_rect_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_glyph_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_stencil_glyph_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_msdf_text_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_stencil_msdf_text_pipeline = nullptr;
    QRhiTexture*                             m_coverage_texture = nullptr;
    QRhiTexture*                             m_msdf_text_atlas_texture = nullptr;
    QRhiSampler*                             m_coverage_sampler = nullptr;
    QRhiSampler*                             m_msdf_text_sampler = nullptr;
    Qsg_atlas_sampler_mode                   m_glyph_sampler_mode =
        Qsg_atlas_sampler_mode::UNKNOWN;
    Qsg_atlas_sampler_mode                   m_msdf_text_sampler_mode =
        Qsg_atlas_sampler_mode::UNKNOWN;
    bool                                     m_dual_source_blend_factors_probe_completed =
        false;
    bool                                     m_dual_source_blend_factors_available = false;
    quint32                                  m_rect_instance_buffer_size = 0U;
    quint32                                  m_glyph_instance_buffer_size = 0U;
    quint32                                  m_msdf_text_instance_buffer_size = 0U;
    Qsg_atlas_buffer_upload_planner   m_rect_upload_planner;
    Qsg_atlas_buffer_upload_planner   m_glyph_upload_planner;
    Qsg_atlas_buffer_upload_planner   m_msdf_text_upload_planner;
    int                                      m_render_row_count = 0;
    std::vector<int>                         m_render_glyph_text_row_capacities;
    std::vector<int>                         m_render_glyph_cursor_text_row_capacities;
    std::vector<Terminal_render_dirty_row_range>
                                             m_render_dirty_row_ranges;
    bool                                     m_render_force_full_reupload = false;
    bool                                     m_render_non_dirty_state_invalidation = false;
    bool                                     m_have_previous_render_state = false;
    Atlas_frame_state_keys                  m_previous_render_state_keys;
    bool                                     m_have_previous_render_font_epoch = false;
    std::uint64_t                            m_previous_render_font_epoch = 0U;
    std::map<QByteArray, Prepared_atlas_text_run>
                                             m_prepared_text_cache;
    std::uint64_t                            m_prepared_text_cache_frame = 0U;
    Simple_atlas_text_cache                  m_simple_text_cache;
#if VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
    Msdf_terminal_text_cache                 m_msdf_text_cache;
    std::uint64_t                            m_msdf_text_uploaded_generation = 0U;
    bool                                     m_msdf_text_resources_ready = false;
#endif
    Atlas_warm_key                           m_warm_key;
    Qsg_atlas_warm_lazy_summary              m_warm_lazy;
    bool                                     m_current_prepare_had_lazy_insert =
        false;
};

}

void Qsg_atlas_buffer_upload_planner::reset()
{
    m_frames_in_flight = 0;
    m_slot_bytes.clear();
    m_slot_instance_rows.clear();
    m_slot_layout_keys.clear();
    m_seeded_slots.clear();
}

void Qsg_atlas_buffer_upload_planner::resize_slots(int frames_in_flight)
{
    const int normalized_frames = std::max(1, frames_in_flight);
    if (m_frames_in_flight == normalized_frames) {
        return;
    }

    m_frames_in_flight = normalized_frames;
    m_slot_bytes.assign(static_cast<std::size_t>(normalized_frames), {});
    m_slot_instance_rows.assign(static_cast<std::size_t>(normalized_frames), {});
    m_slot_layout_keys.assign(static_cast<std::size_t>(normalized_frames), {});
    m_seeded_slots.assign(static_cast<std::size_t>(normalized_frames), 0U);
}

Qsg_atlas_buffer_update_plan
Qsg_atlas_buffer_upload_planner::plan(
    const Qsg_atlas_buffer_update_input& input)
{
    const int frames_in_flight = std::max(1, input.frames_in_flight);
    const int frame_slot = std::clamp(input.frame_slot, 0, frames_in_flight - 1);
    resize_slots(frames_in_flight);

    Qsg_atlas_buffer_update_plan plan;
    Qsg_atlas_buffer_update_summary& summary = plan.summary;
    summary.rhi_frames_in_flight = frames_in_flight;
    summary.rhi_frame_slot       = frame_slot;
    summary.instance_bytes       = std::max(1, input.instance_size);

    const int byte_count = std::max(0, input.byte_count);
    summary.instance_count = byte_count / summary.instance_bytes;
    summary.active_instance_count =
        input.active_instance_count >= 0
            ? input.active_instance_count
            : summary.instance_count;
    summary.buffer_bytes   =
        std::max(1, summary.instance_count) * summary.instance_bytes;
    summary.row_stable_layout = input.row_stable_layout;

    const atlas_dirty_row_summary_t dirty_rows =
        atlas_dirty_rows(input.dirty_row_ranges, input.row_count);
    summary.dirty_rows = dirty_rows.dirty_rows;

    const std::size_t slot_index = static_cast<std::size_t>(frame_slot);
    if (input.bytes == nullptr && byte_count > 0) {
        summary.skipped_upload = true;
        return plan;
    }

    const auto make_current_rows = [&]() {
        std::vector<int> rows(
            static_cast<std::size_t>(summary.instance_count),
            k_qsg_atlas_non_row);
        if (input.instance_rows != nullptr) {
            const std::size_t row_count =
                std::min(rows.size(), input.instance_rows->size());
            std::copy_n(
                input.instance_rows->begin(),
                row_count,
                rows.begin());
        }
        return rows;
    };

    const bool slot_seeded = m_seeded_slots[slot_index] != 0U;
    const int previous_byte_count =
        slot_seeded ? m_slot_bytes[slot_index].size() : 0;
    const bool layout_changed =
        slot_seeded &&
        (previous_byte_count != byte_count ||
            m_slot_layout_keys[slot_index] != input.layout_key);
    const bool full_upload =
        input.buffer_recreated                ||
        input.force_full_reupload             ||
        input.non_dirty_state_invalidation    ||
        !slot_seeded;

    summary.rotating_slot_seed_upload      = !slot_seeded;
    summary.buffer_recreated_upload        = input.buffer_recreated;
    summary.instance_layout_changed_upload = layout_changed;
    summary.full_repaint_upload            = input.force_full_reupload;
    summary.non_dirty_state_upload         =
        input.non_dirty_state_invalidation;

    const auto record_full_upload = [&]() {
        if (byte_count > 0) {
            plan.ranges.push_back({0, byte_count});
            summary.full_upload    = true;
            summary.full_uploads   = 1;
            summary.uploaded_bytes = byte_count;
        }
        else {
            summary.skipped_upload = true;
        }
        m_slot_bytes[slot_index]         = QByteArray(input.bytes, byte_count);
        m_slot_instance_rows[slot_index] = make_current_rows();
        m_slot_layout_keys[slot_index]   = input.layout_key;
        m_seeded_slots[slot_index]       = 1U;
    };

    if (full_upload) {
        record_full_upload();
        summary.seeded_slots = static_cast<int>(std::count(
            m_seeded_slots.begin(),
            m_seeded_slots.end(),
            static_cast<unsigned char>(1U)));
        return plan;
    }

    const char* const current = input.bytes;
    const char* const previous = m_slot_bytes[slot_index].constData();
    const int previous_instance_count =
        previous_byte_count / summary.instance_bytes;
    const int common_instance_count = std::min(
        previous_instance_count,
        summary.instance_count);

    const auto append_upload_range = [&](int byte_offset, int byte_count) {
        if (byte_count <= 0) {
            return;
        }

        if (!plan.ranges.empty() &&
            plan.ranges.back().byte_offset + plan.ranges.back().byte_count ==
                byte_offset)
        {
            plan.ranges.back().byte_count += byte_count;
        }
        else {
            plan.ranges.push_back({byte_offset, byte_count});
        }
    };

    const auto append_instance_upload = [&](int instance) {
        append_upload_range(
            instance * summary.instance_bytes,
            summary.instance_bytes);
    };

    const auto instance_bytes_changed = [&](int instance) {
        const int byte_offset = instance * summary.instance_bytes;
        return std::memcmp(
            previous + byte_offset,
            current + byte_offset,
            static_cast<std::size_t>(summary.instance_bytes)) != 0;
    };

    const auto publish_seeded_slots = [&]() {
        summary.seeded_slots = static_cast<int>(std::count(
            m_seeded_slots.begin(),
            m_seeded_slots.end(),
            static_cast<unsigned char>(1U)));
    };

    const auto finalize_partial_upload = [&]() {
        for (const Qsg_atlas_buffer_update_range& range : plan.ranges) {
            summary.uploaded_bytes += range.byte_count;
            std::memcpy(
                m_slot_bytes[slot_index].data() + range.byte_offset,
                current + range.byte_offset,
                static_cast<std::size_t>(range.byte_count));
        }
        summary.partial_uploads = static_cast<int>(plan.ranges.size());
        summary.partial_upload  = !plan.ranges.empty();
        summary.skipped_upload  = plan.ranges.empty();
        m_slot_layout_keys[slot_index] = input.layout_key;
        m_seeded_slots[slot_index]     = 1U;
        publish_seeded_slots();
    };

    if (input.row_stable_layout       &&
        input.instance_rows != nullptr &&
        !layout_changed                &&
        previous_byte_count == byte_count)
    {
        if (input.row_stable_ranges != nullptr &&
            !input.row_stable_ranges->empty())
        {
            for (const Qsg_atlas_row_stable_range& range :
                *input.row_stable_ranges)
            {
                if (!atlas_row_is_dirty(dirty_rows, range.row)) {
                    continue;
                }

                const int first_instance = std::clamp(
                    range.first_instance,
                    0,
                    common_instance_count);
                const int last_instance = std::clamp(
                    range.first_instance + range.instance_count,
                    first_instance,
                    common_instance_count);
                bool range_changed = false;
                for (int instance = first_instance;
                    instance < last_instance;
                    ++instance)
                {
                    if (!instance_bytes_changed(instance)) {
                        continue;
                    }

                    range_changed = true;
                    break;
                }

                if (range_changed) {
                    append_upload_range(
                        first_instance * summary.instance_bytes,
                        (last_instance - first_instance) *
                            summary.instance_bytes);
                }
            }
        }
        else {
            for (int instance = 0; instance < common_instance_count; ++instance) {
                const std::size_t row_index = static_cast<std::size_t>(instance);
                const int row = row_index < input.instance_rows->size()
                    ? input.instance_rows->at(row_index)
                    : k_qsg_atlas_non_row;
                if (!atlas_row_is_dirty(dirty_rows, row)) {
                    continue;
                }

                if (!instance_bytes_changed(instance)) {
                    continue;
                }

                append_instance_upload(instance);
            }
        }

        bool full_required_for_clean_slot_change = false;
        for (int instance = 0; instance < common_instance_count; ++instance) {
            const std::size_t row_index = static_cast<std::size_t>(instance);
            const int row = row_index < input.instance_rows->size()
                ? input.instance_rows->at(row_index)
                : k_qsg_atlas_non_row;
            if (atlas_row_is_dirty(dirty_rows, row) ||
                !instance_bytes_changed(instance))
            {
                continue;
            }

            full_required_for_clean_slot_change = true;
            break;
        }

        if (full_required_for_clean_slot_change) {
            plan.ranges.clear();
            summary.non_dirty_state_upload = true;
            record_full_upload();
            publish_seeded_slots();
            return plan;
        }

        finalize_partial_upload();
        return plan;
    }

    bool full_required_for_non_dirty_change = false;
    const std::vector<int> current_rows = make_current_rows();
    const auto record_instance_upload = [&](int instance) {
        if (!atlas_row_is_dirty(
                dirty_rows,
                current_rows[static_cast<std::size_t>(instance)]))
        {
            full_required_for_non_dirty_change = true;
            return;
        }

        append_instance_upload(instance);
    };

    for (int instance = 0; instance < common_instance_count; ++instance) {
        if (!instance_bytes_changed(instance)) {
            continue;
        }

        record_instance_upload(instance);
        if (full_required_for_non_dirty_change) {
            break;
        }
    }

    if (!full_required_for_non_dirty_change &&
        summary.instance_count > previous_instance_count)
    {
        for (int instance = previous_instance_count;
            instance < summary.instance_count;
            ++instance)
        {
            record_instance_upload(instance);
            if (full_required_for_non_dirty_change) {
                break;
            }
        }
    }

    if (full_required_for_non_dirty_change) {
        plan.ranges.clear();
        summary.non_dirty_state_upload = true;
        record_full_upload();
        summary.seeded_slots = static_cast<int>(std::count(
            m_seeded_slots.begin(),
            m_seeded_slots.end(),
            static_cast<unsigned char>(1U)));
        return plan;
    }

    m_slot_bytes[slot_index]         = QByteArray(input.bytes, byte_count);
    m_slot_instance_rows[slot_index] = current_rows;
    finalize_partial_upload();
    return plan;
}

bool operator==(
    const Glyph_atlas_cache_key& left,
    const Glyph_atlas_cache_key& right)
{
    return
        left.glyph_index         == right.glyph_index         &&
        left.fallback_face_id    == right.fallback_face_id    &&
        left.physical_pixel_size == right.physical_pixel_size &&
        left.presentation        == right.presentation        &&
        left.coverage_kind       == right.coverage_kind       &&
        left.lcd_order           == right.lcd_order           &&
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
        left.presentation,
        left.coverage_kind,
        left.lcd_order,
        left.subpixel_bucket) <
        std::tie(
            right.glyph_index,
            right.fallback_face_id,
            right.physical_pixel_size,
            right.presentation,
            right.coverage_kind,
            right.lcd_order,
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

bool Glyph_rgba_tile::is_valid() const
{
    return
        coverage_kind != Glyph_coverage_kind::UNKNOWN     &&
        coverage_kind != Glyph_coverage_kind::AMBIGUOUS   &&
        coverage_kind != Glyph_coverage_kind::UNSUPPORTED &&
        size.width()   > 0                                &&
        size.height()  > 0                                &&
        bytes_per_line >= size.width() * 4                &&
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
    m_stats.page_size    = page_size;
    m_stats.page_budget  = m_packer.max_pages();
    m_stats.page_bytes   = qsg_atlas_rgba_tile_byte_count(page_size);
    m_stats.budget_bytes =
        m_stats.page_bytes * static_cast<std::uint64_t>(m_stats.page_budget);
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
    m_stats.page_count      = 0;
    m_stats.allocated_bytes = 0U;
    m_stats.used_bytes      = 0U;
}

void Glyph_atlas_cache::reset()
{
    const std::uint64_t epoch         = m_stats.epoch;
    const std::uint64_t invalidations = m_stats.invalidations;
    const QSize         page_size     = m_stats.page_size;
    const int           page_budget   = m_stats.page_budget;
    const std::uint64_t page_bytes    = m_stats.page_bytes;
    const std::uint64_t budget_bytes  = m_stats.budget_bytes;
    m_entries.clear();
    m_pages.clear();
    m_packer.reset();
    m_stats               = {};
    m_stats.epoch         = epoch;
    m_stats.invalidations = invalidations;
    m_stats.page_size     = page_size;
    m_stats.page_budget   = page_budget;
    m_stats.page_bytes    = page_bytes;
    m_stats.budget_bytes  = budget_bytes;
}

const Glyph_atlas_slot* Glyph_atlas_cache::find(
    const Glyph_atlas_cache_key& key)
{
    ++m_stats.lookups;
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        return nullptr;
    }

    ++m_stats.hits;
    return &found->second.slot;
}

Glyph_atlas_slot Glyph_atlas_cache::insert_or_get(
    const Glyph_atlas_cache_key& key,
    const Glyph_rgba_tile&       tile,
    QPoint                       physical_offset)
{
    const auto found = m_entries.find(key);
    if (found != m_entries.end()) {
        return found->second.slot;
    }

    const std::optional<Glyph_atlas_slot> slot = m_packer.pack(tile.size);
    if (!slot.has_value()) {
        ++m_stats.failed_inserts;
        return {};
    }

    ensure_page_count(m_packer.page_count());
    Glyph_atlas_slot stored_slot = *slot;
    stored_slot.physical_offset  = physical_offset;
    stored_slot.coverage_kind    = tile.coverage_kind;
    stored_slot.lcd_order        = tile.lcd_order;
    copy_tile_to_slot(stored_slot.page, stored_slot.rect, tile);
    m_entries.emplace(key, Entry{stored_slot});
    ++m_stats.inserts;
    m_stats.page_count      = m_packer.page_count();
    m_stats.allocated_bytes = m_stats.page_bytes *
        static_cast<std::uint64_t>(m_stats.page_count);
    m_stats.used_bytes += qsg_atlas_rgba_tile_byte_count(tile.size);
    return stored_slot;
}

Glyph_atlas_cache_stats Glyph_atlas_cache::stats() const
{
    Glyph_atlas_cache_stats stats = m_stats;
    stats.page_count      = m_packer.page_count();
    stats.page_budget     = m_packer.max_pages();
    stats.page_bytes      = qsg_atlas_rgba_tile_byte_count(stats.page_size);
    stats.allocated_bytes = stats.page_bytes *
        static_cast<std::uint64_t>(stats.page_count);
    stats.budget_bytes    = stats.page_bytes *
        static_cast<std::uint64_t>(stats.page_budget);
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
            QByteArray(
                static_cast<int>(qsg_atlas_rgba_tile_byte_count(m_stats.page_size)),
                '\0'));
    }
}

void Glyph_atlas_cache::copy_tile_to_slot(
    int                        page,
    const QRect&               rect,
    const Glyph_rgba_tile&      tile)
{
    QByteArray& page_bytes = m_pages[static_cast<std::size_t>(page)];
    const int page_stride  = m_stats.page_size.width() * 4;
    for (int y = 0; y < tile.size.height(); ++y) {
        const char* const source = tile.bytes.constData() + y * tile.bytes_per_line;
        char* const destination =
            page_bytes.data() + (rect.y() + y) * page_stride + rect.x() * 4;
        std::memcpy(
            destination,
            source,
            static_cast<std::size_t>(tile.size.width() * 4));
    }
}

void Qsg_atlas_recorder::reset()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_report = {};
}

void Qsg_atlas_recorder::record_capture(const Captured_atlas_frame& frame)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t frame_snapshot_sequence = snapshot_sequence(frame);
    const QColor frame_diagnostic_color = qsg_atlas_diagnostic_color(frame);
    const bool frame_light_options = captured_options_are_light(frame);
    if (m_report.capture_count == 0U) {
        m_report.first_captured_snapshot_sequence = frame_snapshot_sequence;
        m_report.first_captured_font_epoch        = frame.font_epoch;
        m_report.first_captured_diagnostic_color       = frame_diagnostic_color;
        m_report.first_captured_light_options     = frame_light_options;
    }
    ++m_report.capture_count;
    m_report.capture_sequence           = frame.capture_sequence;
    m_report.captured_snapshot_sequence = frame_snapshot_sequence;
    m_report.captured_font_epoch        = frame.font_epoch;
    m_report.captured_diagnostic_color       = frame_diagnostic_color;
    m_report.captured_light_options     = frame_light_options;
}

void Qsg_atlas_recorder::record_prepare(
    const Captured_atlas_frame&    frame,
    bool                           command_buffer_non_null,
    bool                           render_target_non_null,
    bool                           rhi_non_null,
    bool                           coverage_texture_created,
    bool                           coverage_upload_recorded,
    bool                           raw_font_rasterized,
    bool                           raw_font_rasterized_in_prepare,
    int                            rasterized_glyphs,
    std::uint64_t                  prepare_thread_id,
    std::uint64_t                  raw_font_raster_thread_id,
    const Glyph_atlas_cache_stats& cache,
    const Qsg_atlas_frame_build_summary& frame_build,
    const Qsg_atlas_render_summary&      render_summary,
    const Qsg_atlas_producer_summary&    producer_summary,
    const Qsg_atlas_warm_lazy_summary&   warm_lazy_summary)
{
    (void)frame;

    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_report.prepare_count;
    m_report.command_buffer_non_null        = command_buffer_non_null;
    m_report.render_target_non_null         = render_target_non_null;
    m_report.rhi_non_null                   = rhi_non_null;
    m_report.msdf_text_renderer_enabled =
        render_summary.msdf_text_renderer_enabled;
    m_report.msdf_text_renderer_compiled =
        render_summary.msdf_text_renderer_compiled;
    m_report.msdf_text_renderer_active =
        render_summary.msdf_text_renderer_active;
    m_report.msdf_text_shader_package_available =
        render_summary.msdf_text_shader_package_available;
    m_report.msdf_text_atlas_built =
        render_summary.msdf_text_atlas_built;
    m_report.msdf_text_atlas_ready =
        render_summary.msdf_text_atlas_ready;
    m_report.msdf_text_texture_ready =
        render_summary.msdf_text_texture_ready;
    m_report.msdf_text_resources_ready =
        render_summary.msdf_text_resources_ready;
    m_report.msdf_text_supported_runs =
        render_summary.msdf_text_supported_runs;
    m_report.msdf_text_runs =
        render_summary.msdf_text_runs;
    m_report.msdf_text_glyph_instances =
        render_summary.msdf_text_glyph_instances;
    m_report.msdf_text_draw_calls =
        render_summary.msdf_text_draw_calls;
    m_report.msdf_text_missed_supported_runs =
        render_summary.msdf_text_missed_supported_runs;
    m_report.msdf_text_missed_supported_glyphs =
        render_summary.msdf_text_missed_supported_glyphs;
    m_report.msdf_text_font_data_bytes =
        render_summary.msdf_text_font_data_bytes;
    m_report.msdf_text_pixel_height =
        render_summary.msdf_text_pixel_height;
    m_report.msdf_text_atlas_size =
        render_summary.msdf_text_atlas_size;
    m_report.msdf_text_px_range =
        render_summary.msdf_text_px_range;
    m_report.msdf_text_message =
        render_summary.msdf_text_message;
    m_report.coverage_texture_created       = coverage_texture_created;
    m_report.coverage_upload_recorded       = coverage_upload_recorded;
    m_report.raw_font_rasterized            = raw_font_rasterized;
    m_report.raw_font_rasterized_in_prepare = raw_font_rasterized_in_prepare;
    m_report.rasterized_glyphs              = rasterized_glyphs;
    m_report.prepare_thread_id              = prepare_thread_id;
    m_report.raw_font_raster_thread_id      = raw_font_raster_thread_id;
    m_report.atlas_page_count               = cache.page_count;
    m_report.cache                          = cache;
    m_report.frame_build                   = frame_build;
    m_report.render                        = render_summary;
    m_report.producer                      = producer_summary;
    m_report.warm_lazy                     = warm_lazy_summary;
}

void Qsg_atlas_recorder::record_render(
    const Captured_atlas_frame& frame,
    QRect                       viewport_rect,
    bool                        drew)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_report.render_count;
    m_report.render_capture_sequence  = frame.capture_sequence;
    m_report.render_snapshot_sequence = snapshot_sequence(frame);
    m_report.render_font_epoch        = frame.font_epoch;
    m_report.render_diagnostic_color  = qsg_atlas_diagnostic_color(frame);
    m_report.render_light_options     = captured_options_are_light(frame);
    m_report.viewport_rect            = viewport_rect;
    m_report.drew                     = drew;
    if (m_report.first_render_snapshot_sequence == 0U) {
        m_report.first_render_capture_sequence  = frame.capture_sequence;
        m_report.first_render_snapshot_sequence = m_report.render_snapshot_sequence;
        m_report.first_render_font_epoch        = frame.font_epoch;
        m_report.first_render_diagnostic_color  = m_report.render_diagnostic_color;
        m_report.first_render_light_options     = captured_options_are_light(frame);
    }
}

Qsg_atlas_frame_report Qsg_atlas_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_report;
}

Glyph_coverage_tile qsg_atlas_coverage_tile_from_image(const QImage& image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }

    if (!qsg_atlas_image_format_is_single_channel_coverage(image.format())) {
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

std::uint64_t qsg_atlas_rgba_tile_byte_count(QSize size)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return 0U;
    }

    return static_cast<std::uint64_t>(size.width()) *
        static_cast<std::uint64_t>(size.height()) * 4U;
}

Glyph_rgba_cache_accounting qsg_atlas_rgba_cache_accounting(
    const Glyph_atlas_cache_stats& cache)
{
    return {
        cache.page_bytes,
        cache.allocated_bytes,
        cache.budget_bytes,
        cache.used_bytes,
    };
}

static Glyph_lcd_order qsg_atlas_lcd_order_for_kind(Glyph_coverage_kind kind)
{
    switch (kind) {
        case Glyph_coverage_kind::LCD_RGB_MASK:
            return Glyph_lcd_order::RGB;
        case Glyph_coverage_kind::LCD_BGR_MASK:
            return Glyph_lcd_order::BGR;
        case Glyph_coverage_kind::UNKNOWN:
        case Glyph_coverage_kind::GRAYSCALE_MASK:
        case Glyph_coverage_kind::COLOR_IMAGE:
        case Glyph_coverage_kind::AMBIGUOUS:
        case Glyph_coverage_kind::UNSUPPORTED:
            break;
    }
    return Glyph_lcd_order::UNKNOWN;
}

static void qsg_atlas_append_rgba_pixel(
    QByteArray& bytes,
    int         red,
    int         green,
    int         blue,
    int         alpha)
{
    bytes.append(static_cast<char>(std::clamp(red,   0, 255)));
    bytes.append(static_cast<char>(std::clamp(green, 0, 255)));
    bytes.append(static_cast<char>(std::clamp(blue,  0, 255)));
    bytes.append(static_cast<char>(std::clamp(alpha, 0, 255)));
}

Glyph_rgba_tile qsg_atlas_rgba_tile_from_image(
    const QImage&             image,
    Glyph_image_presentation  presentation)
{
    Glyph_rgba_tile tile;
    tile.source_format = image.format();
    tile.size          = image.size();
    tile.coverage_kind =
        qsg_atlas_classify_glyph_image_candidate(image, presentation);
    tile.lcd_order = qsg_atlas_lcd_order_for_kind(tile.coverage_kind);

    if (image.isNull() || image.width() <= 0 || image.height() <= 0 ||
        tile.coverage_kind == Glyph_coverage_kind::UNKNOWN          ||
        tile.coverage_kind == Glyph_coverage_kind::AMBIGUOUS        ||
        tile.coverage_kind == Glyph_coverage_kind::UNSUPPORTED)
    {
        return tile;
    }

    tile.bytes_per_line = image.width() * 4;
    tile.bytes.reserve(tile.bytes_per_line * image.height());

    if (tile.coverage_kind == Glyph_coverage_kind::COLOR_IMAGE) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                qsg_atlas_append_rgba_pixel(
                    tile.bytes,
                    color.red(),
                    color.green(),
                    color.blue(),
                    color.alpha());
            }
        }
        return tile;
    }

    if (tile.coverage_kind == Glyph_coverage_kind::GRAYSCALE_MASK &&
        qsg_atlas_image_format_is_single_channel_coverage(image.format()))
    {
        for (int y = 0; y < image.height(); ++y) {
            const uchar* const line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const int coverage = line[x];
                qsg_atlas_append_rgba_pixel(
                    tile.bytes,
                    coverage,
                    coverage,
                    coverage,
                    coverage);
            }
        }
        return tile;
    }

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (tile.coverage_kind == Glyph_coverage_kind::GRAYSCALE_MASK) {
                const int coverage = qGray(color.rgb());
                qsg_atlas_append_rgba_pixel(
                    tile.bytes,
                    coverage,
                    coverage,
                    coverage,
                    coverage);
            }
            else {
                const int combined_coverage =
                    std::max({color.red(), color.green(), color.blue()});
                qsg_atlas_append_rgba_pixel(
                    tile.bytes,
                    color.red(),
                    color.green(),
                    color.blue(),
                    combined_coverage);
            }
        }
    }
    return tile;
}

Qsg_atlas_glyph_image_diagnostic qsg_atlas_glyph_image_diagnostic_from_record(
    const Qsg_atlas_shaped_glyph_record& record,
    const QImage&                        image,
    Glyph_image_presentation             presentation)
{
    Qsg_atlas_glyph_image_diagnostic diagnostic;
    diagnostic.coverage_kind =
        qsg_atlas_classify_glyph_image_candidate(image, presentation);
    diagnostic.presentation         = presentation;
    diagnostic.source_format        = image.format();
    diagnostic.source_size          = image.size();
    diagnostic.glyph_index          = record.glyph_index;
    diagnostic.fallback_face_id     = record.fallback_face_id;
    diagnostic.text_run_index       = record.text_run_index;
    diagnostic.glyph_run_index      = record.glyph_run_index;
    diagnostic.glyph_index_in_run   = record.glyph_index_in_run;
    diagnostic.source_string_start  = record.source_string_start;
    diagnostic.source_string_end    = record.source_string_end;
    return diagnostic;
}

static Glyph_coverage_kind qsg_atlas_classify_rgb_glyph_image_candidate(
    const QImage&             image,
    Glyph_image_presentation  presentation,
    Glyph_coverage_kind       lcd_kind)
{
    if (presentation == Glyph_image_presentation::COLOR) {
        return Glyph_coverage_kind::COLOR_IMAGE;
    }
    if (!qsg_atlas_rgb_image_has_channel_variation(image)) {
        return Glyph_coverage_kind::GRAYSCALE_MASK;
    }
    if (presentation == Glyph_image_presentation::TEXT) {
        return lcd_kind;
    }
    return Glyph_coverage_kind::AMBIGUOUS;
}

Glyph_coverage_kind qsg_atlas_classify_glyph_image_candidate(
    const QImage&             image,
    Glyph_image_presentation  presentation)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return Glyph_coverage_kind::UNKNOWN;
    }

    if (qsg_atlas_image_format_is_single_channel_coverage(image.format())) {
        return Glyph_coverage_kind::GRAYSCALE_MASK;
    }

    switch (image.format()) {
        case QImage::Format_RGB32:
        case QImage::Format_RGB888:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGB30:
        case QImage::Format_RGBX64:
        case QImage::Format_RGBX16FPx4:
        case QImage::Format_RGBX32FPx4:
            return qsg_atlas_classify_rgb_glyph_image_candidate(
                image,
                presentation,
                Glyph_coverage_kind::LCD_RGB_MASK);

        case QImage::Format_BGR888:
        case QImage::Format_BGR30:
            return qsg_atlas_classify_rgb_glyph_image_candidate(
                image,
                presentation,
                Glyph_coverage_kind::LCD_BGR_MASK);

        case QImage::Format_Invalid:
            return Glyph_coverage_kind::UNKNOWN;

        default:
            break;
    }

    if (qsg_atlas_image_format_is_color_alpha(image.format())) {
        return Glyph_coverage_kind::COLOR_IMAGE;
    }

    switch (image.format()) {
        case QImage::Format_ARGB8565_Premultiplied:
        case QImage::Format_ARGB6666_Premultiplied:
        case QImage::Format_ARGB8555_Premultiplied:
        case QImage::Format_ARGB4444_Premultiplied:
        case QImage::Format_A2BGR30_Premultiplied:
        case QImage::Format_A2RGB30_Premultiplied:
        case QImage::Format_RGBA64:
        case QImage::Format_RGBA64_Premultiplied:
        case QImage::Format_RGBA16FPx4:
        case QImage::Format_RGBA16FPx4_Premultiplied:
        case QImage::Format_RGBA32FPx4:
        case QImage::Format_RGBA32FPx4_Premultiplied:
            return Glyph_coverage_kind::COLOR_IMAGE;

        case QImage::Format_RGB16:
        case QImage::Format_RGB666:
        case QImage::Format_RGB555:
        case QImage::Format_RGB444:
            return Glyph_coverage_kind::AMBIGUOUS;

        default:
            return Glyph_coverage_kind::UNSUPPORTED;
    }
}

const char* qsg_atlas_glyph_coverage_kind_name(Glyph_coverage_kind kind)
{
    switch (kind) {
        case Glyph_coverage_kind::UNKNOWN:
            return "unknown";
        case Glyph_coverage_kind::GRAYSCALE_MASK:
            return "grayscale_mask";
        case Glyph_coverage_kind::LCD_RGB_MASK:
            return "lcd_rgb_mask";
        case Glyph_coverage_kind::LCD_BGR_MASK:
            return "lcd_bgr_mask";
        case Glyph_coverage_kind::COLOR_IMAGE:
            return "color_image";
        case Glyph_coverage_kind::AMBIGUOUS:
            return "ambiguous";
        case Glyph_coverage_kind::UNSUPPORTED:
            return "unsupported";
    }

    return "unknown";
}

const char* qsg_atlas_glyph_miss_cause_name(
    Qsg_atlas_glyph_miss_cause cause)
{
    switch (cause) {
        case Qsg_atlas_glyph_miss_cause::NONE:
            return "none";
        case Qsg_atlas_glyph_miss_cause::UNSUPPORTED_IMAGE:
            return "unsupported_image";
        case Qsg_atlas_glyph_miss_cause::ATLAS_INSERT_FAILED:
            return "atlas_insert_failed";
    }
    return "none";
}

const char* qsg_atlas_glyph_image_presentation_name(
    Glyph_image_presentation presentation)
{
    switch (presentation) {
        case Glyph_image_presentation::UNKNOWN:
            return "unknown";
        case Glyph_image_presentation::TEXT:
            return "text";
        case Glyph_image_presentation::COLOR:
            return "color";
    }

    return "unknown";
}

const char* qsg_atlas_sampler_mode_name(Qsg_atlas_sampler_mode mode)
{
    switch (mode) {
        case Qsg_atlas_sampler_mode::UNKNOWN:
            return "unknown";
        case Qsg_atlas_sampler_mode::NEAREST:
            return "nearest";
        case Qsg_atlas_sampler_mode::LINEAR:
            return "linear";
    }

    return "unknown";
}

QFont qsg_atlas_cell_stable_ascii_layout_font(const QFont& font)
{
    QFont layout_font = font;
    layout_font.setKerning(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    layout_font.setFeature(QFont::Tag("calt"), 0U);
    layout_font.setFeature(QFont::Tag("clig"), 0U);
    layout_font.setFeature(QFont::Tag("dlig"), 0U);
    layout_font.setFeature(QFont::Tag("hlig"), 0U);
    layout_font.setFeature(QFont::Tag("liga"), 0U);
    layout_font.setFeature(QFont::Tag("rlig"), 0U);
#endif
    layout_font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        static_cast<int>(layout_font.styleStrategy()) |
        static_cast<int>(QFont::PreferNoShaping)));
    return layout_font;
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

QPoint qsg_atlas_glyph_physical_offset_for_raster_font(
    const QRawFont&           raster_font,
    quint32                   glyph_index,
    Glyph_image_presentation  presentation)
{
    const QRawFontPrivate* const raw_font_private =
        QRawFontPrivate::get(raster_font);
    if (raw_font_private == nullptr ||
        raw_font_private->fontEngine == nullptr)
    {
        const QRectF bounds = raster_font.boundingRect(glyph_index);
        return QPoint(
            static_cast<int>(std::lround(bounds.left())),
            static_cast<int>(std::lround(bounds.top())));
    }

    QFontEngine* const font_engine = raw_font_private->fontEngine;
    const QFontEngine::GlyphFormat format =
        atlas_glyph_format_for_presentation(*font_engine, presentation);
    const glyph_metrics_t metrics =
        font_engine->alphaMapBoundingBox(
            glyph_index,
            QFixedPoint(),
            QTransform(),
            format);
    const int margin = font_engine->glyphMargin(format);
    return QPoint(
        metrics.x.truncate() - margin,
        metrics.y.truncate() - margin);
}

QPointF qsg_atlas_snapped_physical_point(
    QPointF point,
    qreal   device_pixel_ratio)
{
    return QPointF(
        atlas_snapped_physical_coordinate(point.x(), device_pixel_ratio),
        atlas_snapped_physical_coordinate(point.y(), device_pixel_ratio));
}

QRectF qsg_atlas_snapped_glyph_draw_rect(
    QPointF glyph_origin,
    QPoint  glyph_physical_offset,
    QSize   glyph_physical_size,
    qreal   device_pixel_ratio)
{
    const qreal normalized_device_pixel_ratio =
        atlas_normalized_device_pixel_ratio(device_pixel_ratio);
    const QPointF snapped_origin =
        qsg_atlas_snapped_physical_point(
            glyph_origin,
            normalized_device_pixel_ratio);
    const int physical_origin_x = atlas_snapped_physical_int(
        snapped_origin.x(),
        normalized_device_pixel_ratio);
    const int physical_origin_y = atlas_snapped_physical_int(
        snapped_origin.y(),
        normalized_device_pixel_ratio);
    const int physical_width = std::max(0, glyph_physical_size.width());
    const int physical_height = std::max(0, glyph_physical_size.height());
    return QRectF(
        static_cast<qreal>(
            physical_origin_x + glyph_physical_offset.x()) /
            normalized_device_pixel_ratio,
        static_cast<qreal>(
            physical_origin_y + glyph_physical_offset.y()) /
            normalized_device_pixel_ratio,
        static_cast<qreal>(physical_width) / normalized_device_pixel_ratio,
        static_cast<qreal>(physical_height) / normalized_device_pixel_ratio);
}

Glyph_atlas_cache_key qsg_atlas_cache_key(
    quint32                  glyph_index,
    QString                  fallback_face_id,
    qreal                    physical_pixel_size,
    int                      subpixel_bucket,
    Glyph_coverage_kind      coverage_kind,
    Glyph_image_presentation presentation)
{
    const Glyph_lcd_order normalized_lcd_order =
        qsg_atlas_lcd_order_for_kind(coverage_kind);
    return {
        glyph_index,
        std::move(fallback_face_id),
        physical_pixel_size,
        presentation,
        coverage_kind,
        normalized_lcd_order,
        subpixel_bucket,
    };
}

static int qsg_atlas_text_run_cell_span(
    const Terminal_render_text_run& run,
    terminal_cell_metrics_t         cell_metrics)
{
    if (std::isfinite(cell_metrics.width) &&
        cell_metrics.width > 0.0          &&
        run.rect.isValid())
    {
        const int rect_span = static_cast<int>(
            std::round(run.rect.width() / cell_metrics.width));
        if (rect_span > 0) {
            return rect_span;
        }
    }

    return std::max(1, measure_utf8_width(run.text.toUtf8()).cells);
}

static bool qsg_atlas_text_run_source_offsets_map_to_cells(
    const Terminal_render_text_run& run,
    terminal_cell_metrics_t         cell_metrics,
    int                             run_cell_span)
{
    // The frame builder only coalesces one-code-unit-per-cell printable ASCII
    // text. Other clusters stay as standalone cell spans and keep shared owner
    // metadata even when their UTF-16 length happens to equal the cell span.
    constexpr ushort k_printable_ascii_first = 0x20U;
    constexpr ushort k_printable_ascii_last  = 0x7eU;
    bool printable_ascii_cells = !run.text.isEmpty();
    for (QChar code_unit : run.text) {
        const ushort value = code_unit.unicode();
        printable_ascii_cells =
            printable_ascii_cells &&
            value >= k_printable_ascii_first &&
            value <= k_printable_ascii_last;
    }

    return
        run_cell_span > 0                                &&
        run.text.size() == static_cast<qsizetype>(run_cell_span) &&
        printable_ascii_cells                            &&
        std::isfinite(cell_metrics.width)                &&
        cell_metrics.width > 0.0                         &&
        run.rect.isValid();
}

static qsizetype qsg_atlas_clamped_source_string_index(
    qsizetype                         index,
    qsizetype                         text_size,
    Qsg_atlas_shaped_text_run_result& result)
{
    if (text_size <= 0) {
        return 0;
    }

    if (index < 0 || index >= text_size) {
        ++result.invalid_string_indexes;
        return std::clamp<qsizetype>(index, 0, text_size - 1);
    }

    return index;
}

static qsizetype qsg_atlas_source_string_range_end(
    const QList<qsizetype>&  string_indexes,
    int                      glyph_offset,
    qsizetype                source_start,
    qsizetype                text_size)
{
    for (int next = glyph_offset + 1; next < string_indexes.size(); ++next) {
        const qsizetype next_index = string_indexes.at(next);
        if (next_index > source_start && next_index <= text_size) {
            return next_index;
        }
    }

    return text_size;
}

static void qsg_atlas_apply_cell_ownership(
    Qsg_atlas_shaped_glyph_record& record,
    const Terminal_render_text_run& run,
    int                             run_cell_span,
    bool                            source_offsets_map_to_cells)
{
    record.owner_column    = run.column;
    record.owner_cell_span = std::max(1, run_cell_span);

    if (!source_offsets_map_to_cells) {
        return;
    }

    const int owner_offset = std::clamp(
        static_cast<int>(record.source_string_start),
        0,
        run_cell_span - 1);
    const int source_span = std::max(
        1,
        static_cast<int>(
            record.source_string_end - record.source_string_start));
    record.owner_column = run.column + owner_offset;
    record.owner_cell_span = std::clamp(
        source_span,
        1,
        std::max(1, run_cell_span - owner_offset));
}

static QPointF qsg_atlas_shaped_glyph_origin(
    const Qsg_atlas_shaped_glyph_record& record,
    const Terminal_render_text_run&      run,
    terminal_cell_metrics_t              cell_metrics,
    QPointF                              layout_origin,
    QPointF                              layout_position,
    bool                                 source_offsets_map_to_cells)
{
    const QPointF shaped_origin = layout_origin + layout_position;
    if (!source_offsets_map_to_cells) {
        return shaped_origin;
    }

    const int owner_offset =
        std::max(0, record.owner_column - run.column);
    return QPointF(
        run.rect.left() + static_cast<qreal>(owner_offset) * cell_metrics.width,
        shaped_origin.y());
}

Qsg_atlas_shaped_text_run_result qsg_atlas_shape_text_run(
    const Terminal_render_text_run& run,
    const QFont&                    font,
    terminal_cell_metrics_t         cell_metrics,
    qreal                           device_pixel_ratio,
    int                             text_run_index,
    bool                            cursor_text_run)
{
    Qsg_atlas_shaped_text_run_result result;
    if (run.text.isEmpty()) {
        return result;
    }

    QTextLayout layout(run.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return result;
    }
    line.setLineWidth(k_no_wrap_text_line_width);
    line.setPosition(QPointF(0.0, 0.0));
    const qreal line_ascent = line.ascent();
    const QList<QGlyphRun> glyph_runs = line.glyphRuns(
        0,
        run.text.size(),
        QTextLayout::RetrieveGlyphIndexes   |
            QTextLayout::RetrieveGlyphPositions |
            QTextLayout::RetrieveStringIndexes);
    layout.endLayout();

    const QPointF layout_origin(
        run.baseline_origin.x(),
        run.baseline_origin.y() - line_ascent);
    const int run_cell_span =
        qsg_atlas_text_run_cell_span(run, cell_metrics);
    const bool source_offsets_map_to_cells =
        qsg_atlas_text_run_source_offsets_map_to_cells(
            run,
            cell_metrics,
            run_cell_span);
    const qsizetype text_size = run.text.size();

    for (int glyph_run_index = 0; glyph_run_index < glyph_runs.size();
        ++glyph_run_index)
    {
        const QGlyphRun& glyph_run = glyph_runs.at(glyph_run_index);
        const QList<quint32> glyph_indexes = glyph_run.glyphIndexes();
        const QList<QPointF> positions     = glyph_run.positions();
        const QList<qsizetype> string_indexes = glyph_run.stringIndexes();
        const int glyph_count =
            std::min(glyph_indexes.size(), positions.size());
        if (glyph_count <= 0) {
            continue;
        }

        if (string_indexes.size() < glyph_count) {
            result.missing_string_indexes += glyph_count - string_indexes.size();
        }

        const QRawFont raw_font = glyph_run.rawFont();
        if (!raw_font.isValid()) {
            continue;
        }

        const QString face_id = qsg_atlas_face_id_for_raw_font(raw_font);
        const qreal physical_pixel_size =
            qsg_atlas_physical_pixel_size(raw_font, device_pixel_ratio);
        for (int glyph_index_in_run = 0; glyph_index_in_run < glyph_count;
            ++glyph_index_in_run)
        {
            Qsg_atlas_shaped_glyph_record record;
            record.text_run_index     = text_run_index;
            record.cursor_text_run    = cursor_text_run;
            record.glyph_run_index    = glyph_run_index;
            record.glyph_index_in_run = glyph_index_in_run;
            record.row                = run.row;
            record.logical_row        = run.logical_row;
            record.retained_line_id   = run.retained_line_id;
            record.content_generation = run.content_generation;
            record.run_column         = run.column;

            const qsizetype fallback_source =
                glyph_index_in_run < text_size ? glyph_index_in_run : 0;
            const qsizetype source_start =
                glyph_index_in_run < string_indexes.size()
                    ? string_indexes.at(glyph_index_in_run)
                    : fallback_source;
            record.source_string_start =
                qsg_atlas_clamped_source_string_index(
                    source_start,
                    text_size,
                    result);
            record.source_string_end =
                qsg_atlas_source_string_range_end(
                    string_indexes,
                    glyph_index_in_run,
                    record.source_string_start,
                    text_size);
            if (record.source_string_end <= record.source_string_start) {
                record.source_string_end =
                    std::min(text_size, record.source_string_start + 1);
            }

            qsg_atlas_apply_cell_ownership(
                record,
                run,
                run_cell_span,
                source_offsets_map_to_cells);
            record.glyph_index         = glyph_indexes.at(glyph_index_in_run);
            record.raw_font            = raw_font;
            record.fallback_face_id    = face_id;
            record.physical_pixel_size = physical_pixel_size;
            record.glyph_origin =
                qsg_atlas_shaped_glyph_origin(
                    record,
                    run,
                    cell_metrics,
                    layout_origin,
                    positions.at(glyph_index_in_run),
                    source_offsets_map_to_cells);
            record.glyph_bounds =
                raw_font.boundingRect(record.glyph_index);
            result.records.push_back(std::move(record));
        }
    }

    return result;
}

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
    bool                          cursor_blink_visible)
{
    Captured_atlas_frame frame;
    frame.snapshot             = std::move(snapshot);
    frame.ime_preedit          = std::move(ime_preedit);
    frame.options              = std::move(options);
    frame.cell_metrics         = cell_metrics;
    frame.logical_size         = logical_size;
    frame.font                 = std::move(font);
    frame.render_profiler      = std::move(render_profiler);
    frame.device_pixel_ratio   = device_pixel_ratio;
    frame.font_epoch           = font_epoch;
    frame.capture_sequence     = capture_sequence;
    frame.cursor_blink_visible = cursor_blink_visible;
    return frame;
}

QColor qsg_atlas_diagnostic_color(const Captured_atlas_frame& frame)
{
    const int sequence_component = 32 + static_cast<int>(snapshot_sequence(frame) % 160U);
    const int options_component  = captured_options_are_light(frame) ? 214 : 72;
    const int epoch_component    = 32 + static_cast<int>(frame.font_epoch % 160U);
    return QColor(sequence_component, options_component, epoch_component, 255);
}

QSGNode* update_qsg_atlas_node(
    QSGNode*                                      old_node,
    Captured_atlas_frame                         frame,
    const std::shared_ptr<Qsg_atlas_recorder>&
                                                  recorder)
{
    Qsg_atlas_render_node* node =
        dynamic_cast<Qsg_atlas_render_node*>(old_node);
    if (node == nullptr) {
        delete old_node;
        node = new Qsg_atlas_render_node(recorder);
    }

    node->set_frame(std::move(frame), recorder);
    return node;
}

}
