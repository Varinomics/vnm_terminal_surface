#include "vnm_terminal/vnm_terminal_canvas.h"

#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QColor>
#include <QFont>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace term = vnm_terminal::internal;

namespace {

constexpr std::uint16_t k_canvas_style_attribute_mask = 0xffU;

qreal normalized_font_pixel_size(qreal font_size)
{
    if (!std::isfinite(font_size) || font_size <= 0.0) {
        return font_size;
    }

    const qreal bounded = std::min(
        font_size,
        static_cast<qreal>(term::k_vnm_terminal_max_font_pixel_size));
    return static_cast<qreal>(
        std::max(1, static_cast<int>(std::round(bounded))));
}

qreal device_pixel_ratio(const QQuickWindow* window)
{
    if (window == nullptr) {
        return 1.0;
    }

    const qreal ratio = window->effectiveDevicePixelRatio();
    return std::isfinite(ratio) && ratio > 0.0 ? ratio : 1.0;
}

term::Terminal_cursor_shape internal_cursor_shape(
    vnm_terminal::Terminal_canvas_cursor_shape shape)
{
    switch (shape) {
        case vnm_terminal::Terminal_canvas_cursor_shape::BLOCK:
            return term::Terminal_cursor_shape::BLOCK;
        case vnm_terminal::Terminal_canvas_cursor_shape::BAR:
            return term::Terminal_cursor_shape::BAR;
        case vnm_terminal::Terminal_canvas_cursor_shape::UNDERLINE:
            return term::Terminal_cursor_shape::UNDERLINE;
    }

    return term::Terminal_cursor_shape::BLOCK;
}

bool canvas_cursor_shape_is_valid(
    vnm_terminal::Terminal_canvas_cursor_shape shape)
{
    switch (shape) {
        case vnm_terminal::Terminal_canvas_cursor_shape::BLOCK:
        case vnm_terminal::Terminal_canvas_cursor_shape::BAR:
        case vnm_terminal::Terminal_canvas_cursor_shape::UNDERLINE:
            return true;
    }

    return false;
}

std::shared_ptr<const term::Terminal_render_snapshot> materialize_snapshot(
    const vnm_terminal::Terminal_canvas_frame& frame)
{
    auto snapshot = std::make_shared<term::Terminal_render_snapshot>();
    snapshot->grid_size             = {frame.rows, frame.columns};
    snapshot->viewport.visible_rows = frame.rows;
    snapshot->viewport.follow_tail  = true;
    snapshot->color_state.default_foreground_rgba = frame.default_foreground_rgba;
    snapshot->color_state.default_background_rgba = frame.default_background_rgba;
    snapshot->color_state.cursor_rgba             = frame.cursor_rgba;
    snapshot->modes.reverse_video                  = frame.reverse_video;
    snapshot->metadata.sequence                    = frame.sequence;
    snapshot->metadata.publication_generation      = frame.publication_generation;
    snapshot->metadata.row_origin_generation       = frame.row_origin_generation;
    snapshot->cursor.position = {frame.cursor.row, frame.cursor.column};
    snapshot->cursor.shape         = internal_cursor_shape(frame.cursor.shape);
    snapshot->cursor.visible       = frame.cursor.visible;
    snapshot->cursor.blink_enabled = frame.cursor.blink_enabled;
    snapshot->dirty_row_ranges.push_back({0, frame.rows});

    snapshot->styles.reserve(frame.styles.size());
    snapshot->styles.push_back(term::make_default_terminal_text_style());
    for (std::size_t index = 1U; index < frame.styles.size(); ++index) {
        const vnm_terminal::Terminal_canvas_style& style = frame.styles[index];
        snapshot->styles.push_back({
            term::make_rgb_terminal_color_ref(style.foreground_rgba),
            term::make_rgb_terminal_color_ref(style.background_rgba),
            style.attributes,
        });
    }

    std::size_t materialized_cell_count = 0U;
    for (const vnm_terminal::Terminal_canvas_cell& cell : frame.cells) {
        materialized_cell_count += static_cast<std::size_t>(cell.display_width);
    }

    snapshot->cells.reserve(materialized_cell_count);
    for (const vnm_terminal::Terminal_canvas_cell& source_cell : frame.cells) {
        term::Terminal_render_cell base_cell;
        base_cell.position = {source_cell.row, source_cell.column};
        base_cell.text     = term::Terminal_render_cell_text::from_source_cell(
            source_cell.text,
            source_cell.display_width,
            false);
        base_cell.display_width = source_cell.display_width;
        base_cell.style_id      = source_cell.style_index;
        base_cell.text_category = base_cell.text.category();
        snapshot->cells.push_back(std::move(base_cell));

        for (int offset = 1; offset < source_cell.display_width; ++offset) {
            term::Terminal_render_cell continuation;
            continuation.position = {
                source_cell.row,
                source_cell.column + offset,
            };
            continuation.display_width     = 0;
            continuation.wide_continuation = true;
            continuation.style_id          = source_cell.style_index;
            continuation.text_category     = continuation.text.category();
            snapshot->cells.push_back(std::move(continuation));
        }
    }

    return snapshot;
}

bool canvas_frame_is_valid(const vnm_terminal::Terminal_canvas_frame& frame)
{
    if (frame.api_version != vnm_terminal::k_terminal_canvas_frame_api_version ||
        frame.rows <= 0 || frame.rows > vnm_terminal::k_terminal_canvas_max_rows ||
        frame.columns <= 0 ||
        frame.columns > vnm_terminal::k_terminal_canvas_max_columns ||
        frame.cells.size() > vnm_terminal::k_terminal_canvas_max_cells ||
        frame.styles.empty() ||
        frame.styles.size() > vnm_terminal::k_terminal_canvas_max_styles ||
        !canvas_cursor_shape_is_valid(frame.cursor.shape))
    {
        return false;
    }

    const vnm_terminal::Terminal_canvas_style& default_style = frame.styles.front();
    if (default_style.foreground_rgba != frame.default_foreground_rgba ||
        default_style.background_rgba != frame.default_background_rgba ||
        default_style.attributes != 0U)
    {
        return false;
    }

    for (std::size_t index = 1U; index < frame.styles.size(); ++index) {
        const vnm_terminal::Terminal_canvas_style& style = frame.styles[index];
        if ((style.attributes & ~k_canvas_style_attribute_mask) != 0U) {
            return false;
        }
    }

    std::size_t materialized_cell_count = 0U;
    std::size_t frame_text_utf8_bytes   = 0U;
    int         previous_row            = -1;
    int         previous_end_column     = 0;
    for (const vnm_terminal::Terminal_canvas_cell& cell : frame.cells) {
        if (cell.row < 0 || cell.row >= frame.rows ||
            cell.column < 0 || cell.column >= frame.columns ||
            cell.display_width <= 0 ||
            cell.display_width > frame.columns - cell.column ||
            static_cast<std::size_t>(cell.style_index) >= frame.styles.size() ||
            materialized_cell_count >
                vnm_terminal::k_terminal_canvas_max_cells -
                    static_cast<std::size_t>(cell.display_width))
        {
            return false;
        }

        if (previous_row > cell.row ||
            (previous_row == cell.row && previous_end_column > cell.column))
        {
            return false;
        }

        if (cell.text.size() >
            vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units)
        {
            return false;
        }
        const std::size_t cell_text_utf8_bytes =
            static_cast<std::size_t>(cell.text.toUtf8().size());
        if (cell_text_utf8_bytes >
            static_cast<std::size_t>(
                vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes) -
                frame_text_utf8_bytes)
        {
            return false;
        }

        materialized_cell_count += static_cast<std::size_t>(cell.display_width);
        frame_text_utf8_bytes   += cell_text_utf8_bytes;
        previous_row            = cell.row;
        previous_end_column     = cell.column + cell.display_width;
    }

    if (frame.cursor.visible &&
        (frame.cursor.row < 0 || frame.cursor.row >= frame.rows ||
         frame.cursor.column < 0 || frame.cursor.column >= frame.columns))
    {
        return false;
    }

    return true;
}

term::Terminal_render_options render_options(
    const vnm_terminal::Terminal_canvas_frame& frame)
{
    term::Terminal_render_options options;
    options.default_foreground = QColor::fromRgba(frame.default_foreground_rgba);
    options.default_background = QColor::fromRgba(frame.default_background_rgba);
    options.cursor_color       = QColor::fromRgba(frame.cursor_rgba);
    options.text_renderer_policy = term::Terminal_text_renderer_policy::GLYPH;
    return options;
}

bool canvas_cursor_blink_enabled(
    const std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>& frame)
{
    return
        frame != nullptr &&
        frame->cursor.visible &&
        frame->cursor.blink_enabled;
}

} // namespace

struct VNM_TerminalCanvas::Private
{
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame> frame;
    std::shared_ptr<const term::Terminal_render_snapshot>      snapshot;
    QFont                                                       render_font;
    term::Qt_grid_metrics_provider                              metrics_provider;
    term::terminal_cell_metrics_t                               cell_metrics;
    std::shared_ptr<term::Qsg_atlas_recorder>                   recorder =
        std::make_shared<term::Qsg_atlas_recorder>();
    QTimer*                                                     cursor_blink_timer   = nullptr;
    bool                                                        cursor_blink_visible = true;
    qreal                                                       device_pixel_ratio = 1.0;
    std::uint64_t                                               font_epoch          = 1U;
    std::uint64_t                                               capture_sequence    = 0U;
};

VNM_TerminalCanvas::VNM_TerminalCanvas(QQuickItem* parent)
:
    QQuickItem(parent),
    m_private(std::make_unique<Private>()),
    m_font_family(term::vnm_terminal_default_monospace_font_family()),
    m_font_size(term::k_vnm_terminal_default_font_pixel_size)
{
    setFlag(ItemHasContents, true);
    setClip(true);
    m_private->cursor_blink_timer = new QTimer(this);
    m_private->cursor_blink_timer->setInterval(500);
    connect(
        m_private->cursor_blink_timer,
        &QTimer::timeout,
        this,
        &VNM_TerminalCanvas::toggle_cursor_blink_phase);
    refresh_render_state();
}

VNM_TerminalCanvas::~VNM_TerminalCanvas() = default;

QString VNM_TerminalCanvas::font_family() const
{
    return m_font_family;
}

void VNM_TerminalCanvas::set_font_family(const QString& font_family)
{
    if (m_font_family == font_family) {
        return;
    }
    m_font_family = font_family;
    emit font_family_changed();
    refresh_render_state();
}

qreal VNM_TerminalCanvas::font_size() const
{
    return m_font_size;
}

void VNM_TerminalCanvas::set_font_size(qreal font_size)
{
    const qreal normalized = normalized_font_pixel_size(font_size);
    if (m_font_size == normalized ||
        (std::isnan(m_font_size) && std::isnan(normalized)))
    {
        return;
    }
    m_font_size = normalized;
    emit font_size_changed();
    refresh_render_state();
}

int VNM_TerminalCanvas::rows() const
{
    return m_private->frame != nullptr ? m_private->frame->rows : 0;
}

int VNM_TerminalCanvas::columns() const
{
    return m_private->frame != nullptr ? m_private->frame->columns : 0;
}

qulonglong VNM_TerminalCanvas::frame_sequence() const
{
    return m_private->frame != nullptr ? m_private->frame->sequence : 0U;
}

bool VNM_TerminalCanvas::set_canvas_frame(
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame> frame)
{
    if (thread() != QThread::currentThread()) {
        return false;
    }

    if (frame == nullptr) {
        if (m_private->frame == nullptr) {
            return true;
        }
        const bool cursor_blink_was_enabled =
            canvas_cursor_blink_enabled(m_private->frame);
        m_private->frame.reset();
        m_private->snapshot.reset();
        refresh_cursor_blink(cursor_blink_was_enabled);
        refresh_render_state();
        emit frame_changed();
        return true;
    }

    if (!canvas_frame_is_valid(*frame)) {
        return false;
    }

    const std::shared_ptr<const vnm_terminal::Terminal_canvas_frame> owned_frame =
        std::make_shared<const vnm_terminal::Terminal_canvas_frame>(*frame);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        materialize_snapshot(*owned_frame);
    if (term::validate_render_snapshot(*snapshot).status !=
        term::Terminal_render_snapshot_status::OK)
    {
        return false;
    }

    const bool cursor_blink_was_enabled =
        canvas_cursor_blink_enabled(m_private->frame);
    m_private->frame    = owned_frame;
    m_private->snapshot = snapshot;
    refresh_cursor_blink(cursor_blink_was_enabled);
    refresh_render_state();
    emit frame_changed();
    return true;
}

std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>
VNM_TerminalCanvas::canvas_frame() const
{
    return m_private->frame;
}

QSGNode* VNM_TerminalCanvas::updatePaintNode(
    QSGNode*             old_node,
    UpdatePaintNodeData*)
{
    if (m_private->snapshot == nullptr ||
        !term::is_valid_cell_metrics(m_private->cell_metrics) ||
        width() <= 0.0 || height() <= 0.0)
    {
        delete old_node;
        return nullptr;
    }

    term::Captured_atlas_frame captured = term::capture_qsg_atlas_frame(
        m_private->snapshot,
        {},
        render_options(*m_private->frame),
        m_private->cell_metrics,
        boundingRect().size(),
        m_private->render_font,
        {},
        m_private->device_pixel_ratio,
        m_private->font_epoch,
        ++m_private->capture_sequence,
        m_private->cursor_blink_visible);
    return term::update_qsg_atlas_node(
        old_node,
        std::move(captured),
        m_private->recorder);
}

void VNM_TerminalCanvas::releaseResources()
{
    QQuickItem::releaseResources();
}

void VNM_TerminalCanvas::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
        refresh_render_state();
    }
}

void VNM_TerminalCanvas::refresh_render_state()
{
    const qreal previous_ratio = m_private->device_pixel_ratio;
    const QFont previous_font  = m_private->render_font;
    m_private->device_pixel_ratio = device_pixel_ratio(window());
    m_private->render_font = term::vnm_terminal_font(m_font_family, m_font_size);
    if (previous_ratio != m_private->device_pixel_ratio ||
        previous_font != m_private->render_font)
    {
        ++m_private->font_epoch;
        if (m_private->font_epoch == 0U) {
            m_private->font_epoch = 1U;
        }
    }
    m_private->metrics_provider.set_font(m_private->render_font);
    m_private->metrics_provider.set_device_pixel_ratio(
        m_private->device_pixel_ratio);
    m_private->cell_metrics = m_private->metrics_provider.cell_metrics();

    if (m_private->frame != nullptr &&
        term::is_valid_cell_metrics(m_private->cell_metrics))
    {
        setImplicitWidth(
            static_cast<qreal>(m_private->frame->columns) *
                m_private->cell_metrics.width);
        setImplicitHeight(
            static_cast<qreal>(m_private->frame->rows) *
                m_private->cell_metrics.height);
    }
    else {
        setImplicitWidth(0.0);
        setImplicitHeight(0.0);
    }
    update();
}

void VNM_TerminalCanvas::refresh_cursor_blink(bool was_enabled)
{
    const bool enabled = canvas_cursor_blink_enabled(m_private->frame);
    if (enabled == was_enabled) {
        // Active replacements preserve the established timer cadence and
        // visible phase; the disabled state is already stopped and normalized.
        return;
    }

    if (enabled) {
        if (!m_private->cursor_blink_visible) {
            m_private->cursor_blink_visible = true;
            emit cursor_blink_phase_changed(true);
        }
        m_private->cursor_blink_timer->start();
        return;
    }

    m_private->cursor_blink_timer->stop();
    if (!m_private->cursor_blink_visible) {
        m_private->cursor_blink_visible = true;
        emit cursor_blink_phase_changed(true);
    }
}

void VNM_TerminalCanvas::toggle_cursor_blink_phase()
{
    if (m_private->frame == nullptr ||
        !m_private->frame->cursor.visible ||
        !m_private->frame->cursor.blink_enabled)
    {
        return;
    }

    m_private->cursor_blink_visible = !m_private->cursor_blink_visible;
    emit cursor_blink_phase_changed(m_private->cursor_blink_visible);
    update();
}
