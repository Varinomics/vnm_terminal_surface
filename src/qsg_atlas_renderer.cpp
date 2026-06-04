#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/terminal_graphic_geometry.h"
#include "vnm_terminal/internal/unicode_width.h"

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
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
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
constexpr qreal k_no_wrap_text_line_width = 1024.0 * 1024.0;
constexpr int k_atlas_stencil_mask = 0xff;
constexpr int k_atlas_printable_ascii_first = 0x20;
constexpr int k_atlas_printable_ascii_last  = 0x7e;
constexpr std::size_t k_atlas_printable_ascii_count =
    static_cast<std::size_t>(
        k_atlas_printable_ascii_last - k_atlas_printable_ascii_first + 1);

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
    float rect[4]    = {};
    float uv_rect[4] = {};
    float color[4]   = {};
};

struct atlas_ascii_glyph_record_t
{
    quint32          glyph_index = 0U;
    QRectF           bounds;
    Glyph_atlas_slot slot;
};

struct Atlas_ascii_glyph_cache
{
    QFont       font;
    QRawFont    raw_font;
    QRawFont    raster_font;
    QString     face_id;
    qreal       device_pixel_ratio   = 1.0;
    qreal       physical_pixel_size  = 0.0;
    std::uint64_t font_epoch         = 0U;
    bool        valid                = false;
    std::array<atlas_ascii_glyph_record_t, k_atlas_printable_ascii_count>
                glyphs;
};

struct atlas_uniform_t
{
    float matrix[16] = {};
};

struct atlas_pass_range_t
{
    quint32 first = 0U;
    quint32 count = 0U;

    bool has_instances() const { return count > 0U; }
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
};

struct Atlas_frame_state_keys
{
    QByteArray selection;
    QByteArray cursor;
    QByteArray preedit;
    QByteArray options;
    QByteArray visual_bell;
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

void store_color(float* target, const std::array<float, 4>& color)
{
    std::copy(color.begin(), color.end(), target);
}

void store_color(float* target, QColor color, qreal opacity)
{
    store_color(target, atlas_color_components(color, opacity));
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

bool atlas_same_text_geometry(qreal left, qreal right)
{
    constexpr qreal k_epsilon = 0.001;
    return std::abs(left - right) < k_epsilon;
}

qreal atlas_normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return std::max<qreal>(1.0, device_pixel_ratio);
}

std::size_t atlas_printable_ascii_index(ushort code_unit)
{
    return static_cast<std::size_t>(code_unit - k_atlas_printable_ascii_first);
}

bool atlas_text_is_printable_ascii(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }

    unsigned int outside_printable_ascii = 0U;
    const qsizetype text_size            = text.size();
    const ushort* const code_units       = text.utf16();
    for (qsizetype index = 0; index < text_size; ++index) {
        const unsigned int code_unit = code_units[index];
        outside_printable_ascii |= static_cast<unsigned int>(
            code_unit - k_atlas_printable_ascii_first >
                k_atlas_printable_ascii_last - k_atlas_printable_ascii_first);
    }

    return outside_printable_ascii == 0U;
}

bool atlas_direct_ascii_run_candidate(
    const Terminal_render_text_run&    run,
    terminal_cell_metrics_t            cell_metrics)
{
    if (!std::isfinite(cell_metrics.width) ||
        cell_metrics.width <= 0.0          ||
        !run.rect.isValid()                ||
        run.clip_rect.isValid())
    {
        return false;
    }

    return
        atlas_text_is_printable_ascii(run.text) &&
        atlas_same_text_geometry(
            run.rect.width(),
            static_cast<qreal>(run.text.size()) * cell_metrics.width) &&
        atlas_same_text_geometry(run.baseline_origin.x(), run.rect.left());
}

QString atlas_printable_ascii_text()
{
    QString text;
    text.reserve(static_cast<qsizetype>(k_atlas_printable_ascii_count));
    for (int codepoint = k_atlas_printable_ascii_first;
        codepoint <= k_atlas_printable_ascii_last;
        ++codepoint)
    {
        text.append(QChar(static_cast<ushort>(codepoint)));
    }
    return text;
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

        bool r8_texture_created  = false;
        bool r8_upload_recorded  = false;
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
                &r8_texture_created,
                &r8_upload_recorded);
            const bool glyph_ready =
                rect_ready &&
                (!has_glyph_draw_passes() || ensure_glyph_resources(rhi, target));
            m_resources_ready = rect_ready &&
                atlas_ready &&
                glyph_ready &&
                update_atlas_buffers(rhi, command_buffer, &prepare_result.render);
            prepare_result.render.coverage_texture_uploaded = r8_upload_recorded;
            prepare_result.render.coverage_texture_skipped =
                atlas_ready && !r8_upload_recorded && m_cache.stats().page_count > 0;
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
                m_cache.stats(),
                prepare_result.frame_build,
                prepare_result.render);
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
                draw_rect_pass(command_buffer, m_decoration_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_cursor_pass, stencil_enabled);
                draw_rect_pass(command_buffer, m_cursor_graphic_pass, stencil_enabled);
                draw_glyph_pass(command_buffer, m_cursor_text_pass, stencil_enabled);
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
        delete_resource(m_stencil_glyph_pipeline);
        delete_resource(m_glyph_pipeline);
        delete_resource(m_stencil_rect_pipeline);
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
        m_rect_upload_planner.reset();
        m_glyph_upload_planner.reset();
        m_render_glyph_text_row_capacities.clear();
        m_render_glyph_cursor_text_row_capacities.clear();
        m_glyph_buffer_row_stable_ranges.clear();
    }

private:
    bool ensure_shaders()
    {
        if (!m_shader_packages_checked) {
            m_vertex_shader              = load_shader(k_atlas_vertex_shader_path);
            m_fragment_shader            = load_shader(k_atlas_fragment_shader_path);
            m_glyph_vertex_shader        = load_shader(k_atlas_glyph_vertex_shader_path);
            m_glyph_fragment_shader      = load_shader(k_atlas_glyph_fragment_shader_path);
            m_shader_packages_checked = true;
        }

        return
            m_vertex_shader.isValid()         &&
            m_fragment_shader.isValid()       &&
            m_glyph_vertex_shader.isValid()   &&
            m_glyph_fragment_shader.isValid();
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
        pipeline->setTargetBlends({atlas_blend()});
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

        m_static_vertex_upload_needed = true;
        return true;
    }

    bool ensure_coverage_texture(QRhi* rhi, bool* out_created = nullptr)
    {
        if (out_created != nullptr) {
            *out_created = false;
        }

        const QSize page_size = m_cache.stats().page_size;
        if (m_coverage_texture != nullptr &&
            m_coverage_texture->pixelSize() == page_size)
        {
            return true;
        }

        delete_resource(m_stencil_glyph_pipeline);
        delete_resource(m_glyph_pipeline);
        delete_resource(m_glyph_shader_resources);
        delete_resource(m_coverage_texture);

        QRhiTexture* texture = rhi->newTexture(QRhiTexture::R8, page_size);
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
                QRhiSampler::Linear,
                QRhiSampler::Linear,
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

    atlas_pass_range_t append_cursor_graphic_pass(
        const std::vector<Terminal_render_rect>& rects,
        const std::vector<Terminal_render_arc>&  arcs,
        Atlas_prepare_result&                   result,
        qreal                                    opacity)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : rects) {
            Terminal_render_rect overlay_rect = rect;
            overlay_rect.color = atlas_cursor_graphic_overlay_color(rect.color);
            append_graphic_rect_instance(overlay_rect, opacity);
        }
        const int before_arcs = static_cast<int>(m_rect_instances.size());
        for (Terminal_render_arc arc : arcs) {
            arc.color = atlas_cursor_graphic_overlay_color(arc.color);
            append_arc_instances(arc, opacity);
        }
        result.frame_build.cursor_graphic_arc_raster_rects +=
            static_cast<int>(m_rect_instances.size()) - before_arcs;
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_pass_range_t append_graphic_pass(
        const Terminal_render_frame& render_frame,
        qreal                        opacity,
        Atlas_prepare_result&       result)
    {
        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_rect_instances.size());
        for (const Terminal_render_rect& rect : render_frame.graphic_rects) {
            append_graphic_rect_instance(rect, opacity);
        }

        const std::vector<Terminal_render_rect> packed_hard_blocks =
            terminal_render_packed_hard_graphic_rects(render_frame);
        result.frame_build.packed_hard_block_rects =
            static_cast<int>(packed_hard_blocks.size());
        for (const Terminal_render_rect& rect : packed_hard_blocks) {
            append_graphic_rect_instance(rect, opacity);
        }

        const int before_arcs = static_cast<int>(m_rect_instances.size());
        for (const Terminal_render_arc& arc : render_frame.graphic_arcs) {
            append_arc_instances(arc, opacity);
        }
        result.frame_build.graphic_arc_raster_rects =
            static_cast<int>(m_rect_instances.size()) - before_arcs;
        range.count =
            static_cast<quint32>(m_rect_instances.size()) - range.first;
        return range;
    }

    atlas_pass_range_t append_text_pass(
        const std::vector<Terminal_render_text_run>& runs,
        qreal                                        opacity,
        Atlas_prepare_result&                       result)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::append_text_pass");

        atlas_pass_range_t range;
        range.first = static_cast<quint32>(m_glyph_instances.size());
        Atlas_ascii_glyph_cache* ascii_cache = nullptr;
        for (const Terminal_render_text_run& run : runs) {
            append_text_run(run, opacity, result, ascii_cache);
        }
        range.count =
            static_cast<quint32>(m_glyph_instances.size()) - range.first;
        return range;
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
        m_cache.set_epoch(m_frame.font_epoch);
        m_rect_instances.clear();
        m_glyph_instances.clear();
        m_glyph_buffer_instances.clear();
        m_rect_instance_rows.clear();
        m_glyph_instance_rows.clear();
        m_glyph_buffer_instance_rows.clear();
        m_glyph_buffer_row_stable_ranges.clear();
        m_background_pass     = {};
        m_selection_pass      = {};
        m_graphic_pass        = {};
        m_text_pass           = {};
        m_decoration_pass     = {};
        m_cursor_pass         = {};
        m_cursor_graphic_pass = {};
        m_cursor_text_pass    = {};
        m_overlay_pass        = {};

        Atlas_prepare_result result;
        const QRawFont base_raw_font = QRawFont::fromFont(m_frame.font);
        result.base_face_id = base_raw_font.isValid()
            ? qsg_atlas_face_id_for_raw_font(base_raw_font)
            : QString();
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

        m_background_pass     = append_rect_pass(background_rects, opacity);
        m_selection_pass      = append_rect_pass(render_frame.selection_rects, opacity);
        m_graphic_pass        = append_graphic_pass(render_frame, opacity, result);
        m_text_pass           = append_text_pass(render_frame.text_runs, opacity, result);
        m_decoration_pass     = append_decoration_pass(render_frame.decorations, opacity);
        m_cursor_pass         = append_cursor_pass(render_frame.cursors, opacity);
        m_cursor_graphic_pass =
            append_cursor_graphic_pass(
                render_frame.cursor_graphic_rects,
                render_frame.cursor_graphic_arcs,
                result,
                opacity);
        m_cursor_text_pass    = append_text_pass(render_frame.cursor_text_runs, opacity, result);
        m_overlay_pass        = append_rect_pass(render_frame.overlay_rects, opacity);
        build_render_glyph_buffer_layout(result);
        result.raw_font_rasterized = result.rasterized_glyphs > 0;
        finalize_frame_build_summary(render_frame, result);
        finalize_render_summary(render_frame, font_epoch_changed, result);
        return result;
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
        summary.frame_cursor_graphic_rects =
            static_cast<int>(render_frame.cursor_graphic_rects.size());
        summary.frame_cursor_graphic_arcs =
            static_cast<int>(render_frame.cursor_graphic_arcs.size());
        summary.frame_overlay_rects       =
            static_cast<int>(render_frame.overlay_rects.size());
        summary.packed_rows               =
            static_cast<int>(render_frame.packed_rows.size());
        summary.packed_graphic_cells      = render_frame.stats.packed_graphic_cells;
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
        append_key_vector(
            keys.cursor,
            render_frame.cursor_graphic_rects,
            append_key_render_rect);
        append_key_vector(keys.cursor, render_frame.cursor_graphic_arcs, append_key_arc);
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
            !render_frame.cursor_graphic_rects.empty() ||
            !render_frame.cursor_graphic_arcs.empty()  ||
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
            atlas_pass_draw_count(m_cursor_graphic_pass) +
            atlas_pass_draw_count(m_overlay_pass);
        summary.glyph_draw_calls =
            atlas_pass_draw_count(m_text_pass) +
            atlas_pass_draw_count(m_cursor_text_pass);
        summary.draw_calls = summary.rect_draw_calls + summary.glyph_draw_calls;

        const Glyph_atlas_cache_stats cache = m_cache.stats();
        summary.atlas_page_count      = cache.page_count;
        summary.atlas_page_budget     = cache.page_budget;
        summary.atlas_page_bytes      = cache.page_bytes;
        summary.atlas_allocated_bytes = cache.allocated_bytes;
        summary.atlas_budget_bytes    = cache.budget_bytes;
        summary.atlas_used_bytes      = cache.used_bytes;
        summary.atlas_failed_inserts  = cache.failed_inserts;

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

    Atlas_ascii_glyph_cache& ascii_glyph_cache()
    {
        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        if (m_ascii_glyph_cache.valid                         &&
            m_ascii_glyph_cache.font == m_frame.font          &&
            m_ascii_glyph_cache.font_epoch == m_frame.font_epoch &&
            atlas_same_text_geometry(
                m_ascii_glyph_cache.device_pixel_ratio,
                device_pixel_ratio))
        {
            return m_ascii_glyph_cache;
        }

        m_ascii_glyph_cache = {};
        m_ascii_glyph_cache.font               = m_frame.font;
        m_ascii_glyph_cache.font_epoch         = m_frame.font_epoch;
        m_ascii_glyph_cache.device_pixel_ratio = device_pixel_ratio;
        m_ascii_glyph_cache.raw_font           = QRawFont::fromFont(m_frame.font);
        if (!m_ascii_glyph_cache.raw_font.isValid()) {
            return m_ascii_glyph_cache;
        }

        m_ascii_glyph_cache.face_id =
            qsg_atlas_face_id_for_raw_font(m_ascii_glyph_cache.raw_font);
        m_ascii_glyph_cache.physical_pixel_size = qsg_atlas_physical_pixel_size(
            m_ascii_glyph_cache.raw_font,
            m_frame.device_pixel_ratio);
        m_ascii_glyph_cache.raster_font = m_ascii_glyph_cache.raw_font;
        m_ascii_glyph_cache.raster_font.setPixelSize(
            m_ascii_glyph_cache.physical_pixel_size);

        const QList<quint32> glyph_indexes =
            m_ascii_glyph_cache.raw_font.glyphIndexesForString(
                atlas_printable_ascii_text());
        if (glyph_indexes.size() !=
            static_cast<qsizetype>(k_atlas_printable_ascii_count))
        {
            m_ascii_glyph_cache = {};
            return m_ascii_glyph_cache;
        }

        for (int codepoint = k_atlas_printable_ascii_first;
            codepoint <= k_atlas_printable_ascii_last;
            ++codepoint)
        {
            const std::size_t glyph_index =
                atlas_printable_ascii_index(static_cast<ushort>(codepoint));
            const quint32 raw_glyph_index = glyph_indexes.at(
                static_cast<qsizetype>(glyph_index));
            if (raw_glyph_index == 0U) {
                m_ascii_glyph_cache = {};
                return m_ascii_glyph_cache;
            }

            atlas_ascii_glyph_record_t& glyph =
                m_ascii_glyph_cache.glyphs[glyph_index];
            glyph.glyph_index = raw_glyph_index;
            glyph.bounds      =
                m_ascii_glyph_cache.raw_font.boundingRect(raw_glyph_index);
        }

        m_ascii_glyph_cache.valid = true;
        return m_ascii_glyph_cache;
    }

    Glyph_atlas_slot glyph_slot_for_index(
        quint32                  glyph_index,
        const QString&           face_id,
        qreal                    physical_pixel_size,
        QRawFont&                raster_font,
        Atlas_prepare_result&   result)
    {
        const Glyph_atlas_cache_key key = qsg_atlas_cache_key(
            glyph_index,
            face_id,
            physical_pixel_size,
            0);
        if (const Glyph_atlas_slot* cached_slot = m_cache.find(key);
            cached_slot != nullptr)
        {
            return *cached_slot;
        }

        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::rasterize_glyph");
        result.raster_thread = current_thread_id();
        const QImage alpha_map = raster_font.alphaMapForGlyph(
            glyph_index,
            QRawFont::PixelAntialiasing);
        if (qsg_atlas_image_format_is_color_alpha(alpha_map.format())) {
            ++result.frame_build.color_glyph_alpha_demotions;
            ++result.frame_build.glyph_color_alpha_failures;
            ++result.frame_build.glyph_missed_instances;
            return {};
        }
        const Glyph_coverage_tile tile =
            qsg_atlas_coverage_tile_from_image(alpha_map);
        if (!tile.is_valid()) {
            ++result.frame_build.glyph_coverage_failures;
            ++result.frame_build.glyph_missed_instances;
            return {};
        }

        const Glyph_atlas_slot slot = m_cache.insert_or_get(key, tile);
        if (slot.is_valid()) {
            ++result.rasterized_glyphs;
        }
        else {
            ++result.frame_build.glyph_atlas_insert_failures;
            ++result.frame_build.glyph_missed_instances;
        }
        return slot;
    }

    Glyph_atlas_slot glyph_slot_for_ascii(
        atlas_ascii_glyph_record_t&  glyph,
        Atlas_ascii_glyph_cache&   cache,
        Atlas_prepare_result&      result)
    {
        if (glyph.slot.is_valid()) {
            return glyph.slot;
        }

        glyph.slot = glyph_slot_for_index(
            glyph.glyph_index,
            cache.face_id,
            cache.physical_pixel_size,
            cache.raster_font,
            result);
        return glyph.slot;
    }

    void append_glyph_instance(
        const Glyph_atlas_slot&        slot,
        const QRectF&                  bounds,
        QPointF                        glyph_origin,
        const Terminal_render_text_run& run,
        const std::array<float, 4>&     color,
        qreal                          device_pixel_ratio,
        qreal                          inverse_page_width,
        qreal                          inverse_page_height,
        int*                           out_appended_instances)
    {
        const qreal inverse_device_pixel_ratio =
            1.0 / atlas_normalized_device_pixel_ratio(device_pixel_ratio);
        QRectF glyph_rect(
            glyph_origin + bounds.topLeft(),
            QSizeF(
                static_cast<qreal>(slot.rect.width())  * inverse_device_pixel_ratio,
                static_cast<qreal>(slot.rect.height()) * inverse_device_pixel_ratio));
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

        atlas_glyph_instance_t instance;
        store_rect(instance.rect, glyph_rect);
        store_uv_rect(instance.uv_rect, uv_rect);
        store_color(instance.color, color);
        m_glyph_instances.push_back(instance);
        m_glyph_instance_rows.push_back(
            run.row >= 0 && run.row < m_render_row_count
                ? run.row
                : k_qsg_atlas_non_row);
        if (out_appended_instances != nullptr) {
            ++*out_appended_instances;
        }
    }

    bool append_direct_ascii_text_run(
        const Terminal_render_text_run&  run,
        qreal                            opacity,
        Atlas_prepare_result&           result,
        Atlas_ascii_glyph_cache*&       ascii_cache)
    {
        if (!atlas_direct_ascii_run_candidate(run, m_frame.cell_metrics)) {
            return false;
        }

        if (ascii_cache == nullptr) {
            ascii_cache = &ascii_glyph_cache();
        }
        if (!ascii_cache->valid) {
            return false;
        }
        Atlas_ascii_glyph_cache& cache = *ascii_cache;

        VNM_TERMINAL_PROFILE_SCOPE(
            "Qsg_atlas_render_node::append_direct_ascii_text_run");
        if (result.render.direct_ascii_text_runs == 0) {
            record_glyph_face(cache.face_id, result);
        }
        int appended_instances = 0;
        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        const QSize page_size = m_cache.stats().page_size;
        const qreal inverse_page_width =
            1.0 / static_cast<qreal>(std::max(1, page_size.width()));
        const qreal inverse_page_height =
            1.0 / static_cast<qreal>(std::max(1, page_size.height()));
        const std::array<float, 4> color =
            atlas_color_components(run.foreground, opacity);
        const ushort* const code_units = run.text.utf16();
        for (qsizetype index = 0; index < run.text.size(); ++index) {
            atlas_ascii_glyph_record_t& glyph =
                cache.glyphs[atlas_printable_ascii_index(code_units[index])];
            if (glyph.bounds.width() <= 0.0 || glyph.bounds.height() <= 0.0) {
                continue;
            }

            const QPointF glyph_origin(
                run.baseline_origin.x() +
                    static_cast<qreal>(index) * m_frame.cell_metrics.width,
                run.baseline_origin.y());
            const Glyph_atlas_slot slot =
                glyph_slot_for_ascii(glyph, cache, result);
            if (!slot.is_valid()) {
                continue;
            }

            append_glyph_instance(
                slot,
                glyph.bounds,
                glyph_origin,
                run,
                color,
                device_pixel_ratio,
                inverse_page_width,
                inverse_page_height,
                &appended_instances);
        }

        ++result.render.direct_ascii_text_runs;
        result.render.direct_ascii_glyph_instances += appended_instances;
        return true;
    }

    void append_text_run(
        const Terminal_render_text_run&  run,
        qreal                            opacity,
        Atlas_prepare_result&           result,
        Atlas_ascii_glyph_cache*&       ascii_cache)
    {
        if (run.text.isEmpty()) {
            return;
        }

        if (append_direct_ascii_text_run(run, opacity, result, ascii_cache)) {
            return;
        }

        if (text_has_emoji_presentation(run.text)) {
            ++result.frame_build.emoji_presentation_runs;
        }

        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::append_qt_layout_text_run");
        ++result.render.qt_layout_text_runs;
        const int glyph_instances_before = static_cast<int>(m_glyph_instances.size());
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
        result.render.qt_layout_glyph_instances +=
            static_cast<int>(m_glyph_instances.size()) - glyph_instances_before;
    }

    void append_glyph_run(
        const QGlyphRun&                glyph_run,
        const Terminal_render_text_run& run,
        QPointF                         layout_origin,
        qreal                           opacity,
        Atlas_prepare_result&          result)
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
        const qreal device_pixel_ratio =
            std::max<qreal>(1.0, m_frame.device_pixel_ratio);
        const QSize page_size = m_cache.stats().page_size;
        const qreal inverse_page_width =
            1.0 / static_cast<qreal>(std::max(1, page_size.width()));
        const qreal inverse_page_height =
            1.0 / static_cast<qreal>(std::max(1, page_size.height()));
        const std::array<float, 4> color =
            atlas_color_components(run.foreground, opacity);
        QRawFont raster_font = raw_font;
        raster_font.setPixelSize(physical_pixel_size);
        const QString face_id = qsg_atlas_face_id_for_raw_font(raw_font);
        record_glyph_face(face_id, result);
        for (int index = 0; index < glyph_count; ++index) {
            const quint32 glyph_index = glyph_indexes.at(index);
            const QRectF bounds = raw_font.boundingRect(glyph_index);
            if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
                continue;
            }

            const QPointF glyph_origin = layout_origin + positions.at(index);
            const Glyph_atlas_slot slot = glyph_slot_for_index(
                glyph_index,
                face_id,
                physical_pixel_size,
                raster_font,
                result);
            if (!slot.is_valid()) {
                continue;
            }

            append_glyph_instance(
                slot,
                bounds,
                glyph_origin,
                run,
                color,
                device_pixel_ratio,
                inverse_page_width,
                inverse_page_height,
                nullptr);
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
        bool*               out_r8_texture_created,
        bool*               out_r8_upload_recorded)
    {
        VNM_TERMINAL_PROFILE_SCOPE("Qsg_atlas_render_node::upload_coverage_texture");

        if (m_cache.stats().page_count <= 0) {
            if (out_r8_upload_recorded != nullptr) {
                *out_r8_upload_recorded = false;
            }
            return true;
        }

        bool texture_created = false;
        const bool texture_ready = ensure_coverage_texture(rhi, &texture_created);
        if (out_r8_texture_created != nullptr) {
            *out_r8_texture_created =
                texture_ready && m_coverage_texture != nullptr &&
                m_coverage_texture->format() == QRhiTexture::R8;
        }
        if (!texture_ready) {
            return false;
        }
        if (!coverage_dirty && !texture_created) {
            if (out_r8_upload_recorded != nullptr) {
                *out_r8_upload_recorded = false;
            }
            return true;
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
        append_pass_key(key, m_cursor_graphic_pass);
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

    bool update_atlas_buffers(
        QRhi*                             rhi,
        QRhiCommandBuffer*                command_buffer,
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

        const int frames_in_flight =
            std::max(1, rhi->resourceLimit(QRhi::FramesInFlight));
        const int frame_slot =
            std::clamp(rhi->currentFrameSlot(), 0, frames_in_flight - 1);
        const int rect_byte_count = static_cast<int>(
            m_rect_instances.size() * sizeof(atlas_instance_t));
        const int glyph_byte_count = static_cast<int>(
            m_glyph_buffer_instances.size() * sizeof(atlas_glyph_instance_t));
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
        if (render_summary != nullptr) {
            render_summary->rect_buffer  = rect_plan.summary;
            render_summary->glyph_buffer = glyph_plan.summary;
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

    bool has_glyph_draw_passes() const
    {
        return m_text_pass.has_instances() || m_cursor_text_pass.has_instances();
    }

    quint32 total_instance_count() const
    {
        return
            static_cast<quint32>(m_rect_instances.size()) +
            static_cast<quint32>(m_glyph_instances.size());
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
    std::vector<atlas_instance_t>             m_rect_instances;
    std::vector<atlas_glyph_instance_t>       m_glyph_instances;
    std::vector<atlas_glyph_instance_t>       m_glyph_buffer_instances;
    std::vector<int>                         m_rect_instance_rows;
    std::vector<int>                         m_glyph_instance_rows;
    std::vector<int>                         m_glyph_buffer_instance_rows;
    std::vector<Qsg_atlas_row_stable_range>
                                             m_glyph_buffer_row_stable_ranges;
    atlas_pass_range_t                        m_background_pass;
    atlas_pass_range_t                        m_selection_pass;
    atlas_pass_range_t                        m_graphic_pass;
    atlas_pass_range_t                        m_text_pass;
    atlas_pass_range_t                        m_decoration_pass;
    atlas_pass_range_t                        m_cursor_pass;
    atlas_pass_range_t                        m_cursor_graphic_pass;
    atlas_pass_range_t                        m_cursor_text_pass;
    atlas_pass_range_t                        m_overlay_pass;
    QRhiBuffer*                              m_vertex_buffer = nullptr;
    QRhiBuffer*                              m_rect_instance_buffer = nullptr;
    QRhiBuffer*                              m_glyph_instance_buffer = nullptr;
    QRhiBuffer*                              m_uniform_buffer = nullptr;
    QRhiShaderResourceBindings*              m_rect_shader_resources = nullptr;
    QRhiShaderResourceBindings*              m_glyph_shader_resources = nullptr;
    QRhiGraphicsPipeline*                    m_rect_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_stencil_rect_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_glyph_pipeline = nullptr;
    QRhiGraphicsPipeline*                    m_stencil_glyph_pipeline = nullptr;
    QRhiTexture*                             m_coverage_texture = nullptr;
    QRhiSampler*                             m_coverage_sampler = nullptr;
    quint32                                  m_rect_instance_buffer_size = 0U;
    quint32                                  m_glyph_instance_buffer_size = 0U;
    Qsg_atlas_buffer_upload_planner   m_rect_upload_planner;
    Qsg_atlas_buffer_upload_planner   m_glyph_upload_planner;
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
    Atlas_ascii_glyph_cache                 m_ascii_glyph_cache;
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

    const auto append_instance_upload = [&](int instance) {
        const int byte_offset = instance * summary.instance_bytes;
        if (!plan.ranges.empty() &&
            plan.ranges.back().byte_offset + plan.ranges.back().byte_count ==
                byte_offset)
        {
            plan.ranges.back().byte_count += summary.instance_bytes;
        }
        else {
            plan.ranges.push_back({byte_offset, summary.instance_bytes});
        }
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
                for (int instance = first_instance;
                    instance < last_instance;
                    ++instance)
                {
                    if (!instance_bytes_changed(instance)) {
                        continue;
                    }

                    append_instance_upload(instance);
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
    m_stats.page_size    = page_size;
    m_stats.page_budget  = m_packer.max_pages();
    m_stats.page_bytes   = static_cast<std::uint64_t>(
        std::max(0, page_size.width()) * std::max(0, page_size.height()));
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
        ++m_stats.failed_inserts;
        return {};
    }

    ensure_page_count(m_packer.page_count());
    copy_tile_to_slot(slot->page, slot->rect, tile);
    m_entries.emplace(key, Entry{*slot});
    ++m_stats.inserts;
    m_stats.page_count      = m_packer.page_count();
    m_stats.allocated_bytes = m_stats.page_bytes *
        static_cast<std::uint64_t>(m_stats.page_count);
    m_stats.used_bytes += static_cast<std::uint64_t>(
        std::max(0, tile.size.width()) * std::max(0, tile.size.height()));
    return *slot;
}

Glyph_atlas_cache_stats Glyph_atlas_cache::stats() const
{
    Glyph_atlas_cache_stats stats = m_stats;
    stats.page_count      = m_packer.page_count();
    stats.page_budget     = m_packer.max_pages();
    stats.page_bytes      = static_cast<std::uint64_t>(
        std::max(0, stats.page_size.width()) *
        std::max(0, stats.page_size.height()));
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
    bool                           r8_texture_created,
    bool                           r8_upload_recorded,
    bool                           raw_font_rasterized,
    bool                           raw_font_rasterized_in_prepare,
    int                            rasterized_glyphs,
    std::uint64_t                  prepare_thread_id,
    std::uint64_t                  raw_font_raster_thread_id,
    const Glyph_atlas_cache_stats& cache,
    const Qsg_atlas_frame_build_summary& frame_build,
    const Qsg_atlas_render_summary&      render_summary)
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
    m_report.frame_build                   = frame_build;
    m_report.render                        = render_summary;
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
