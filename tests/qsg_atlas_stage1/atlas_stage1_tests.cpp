#include "vnm_terminal/internal/qsg_atlas_renderer_stage1.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QMetaObject>
#include <QQuickWindow>
#include <QRawFont>
#include <QRectF>
#include <QSGRendererInterface>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

#ifndef VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH
#define VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH ""
#endif

#ifndef VNM_TERMINAL_SURFACE_SOURCE_PATH
#define VNM_TERMINAL_SURFACE_SOURCE_PATH ""
#endif

constexpr int k_unsupported_backend_skip_return_code = 77;

bool has_argument(int argc, char** argv, const char* expected)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], expected) == 0) {
            return true;
        }
    }

    return false;
}

QByteArray byte_array(std::initializer_list<int> values)
{
    QByteArray out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (int value : values) {
        out.append(static_cast<char>(value));
    }
    return out;
}

const char* argument_value(int argc, char** argv, const char* option, const char* fallback)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], option) == 0) {
            return argv[index + 1];
        }
    }

    return fallback;
}

void configure_graphics_api(const char* backend)
{
    if (std::strcmp(backend, "d3d11") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
    }
    else
    if (std::strcmp(backend, "d3d12") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
    }
    else
    if (std::strcmp(backend, "vulkan") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::VulkanRhi);
    }
    else
    if (std::strcmp(backend, "opengl") == 0) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGLRhi);
    }
    else {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11Rhi);
    }
}

term::Terminal_render_snapshot make_text_snapshot(std::uint64_t sequence, QString text)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = 1;
    viewport.follow_tail      = true;
    viewport.active_buffer    = term::Terminal_buffer_id::PRIMARY;
    viewport.offset_from_tail = 0;
    viewport.scrollback_rows  = 0;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({1, 8}, viewport, sequence);
    snapshot.dirty_row_ranges = {{0, 1}};
    if (!text.isEmpty()) {
        snapshot.cells.push_back({
            {0, 0},
            term::Terminal_render_cell_text::from_source_cell(text, 1, false),
            0U,
            1,
            false,
            term::k_default_terminal_style_id,
            term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        });
    }
    return snapshot;
}

struct Stage2_diff_stats
{
    int compared_pixels = 0;
    int diff_pixels     = 0;
    int max_delta       = 0;
};

struct Stage2_glyph_stats
{
    int compared_pixels      = 0;
    int diff_pixels          = 0;
    int max_delta            = 0;
    int reference_ink_pixels = 0;
    int atlas_ink_pixels     = 0;
};

struct Stage2_aa_budget
{
    int compared_pixels = 0;
    int max_delta       = 0;
    int diff_pixels     = 0;
    int min_ink_pixels  = 0;
    int max_ink_pixels  = 0;
};

struct Stage2_exact_mask_class
{
    std::string         name;
    std::vector<QRectF> masks;
};

struct Stage2_glyph_mask
{
    QRectF rect;
    QColor background;
    bool   require_ink = true;
};

struct Stage2_parity_fixture
{
    std::string                         name;
    term::Terminal_render_snapshot      snapshot;
    QSizeF                              logical_size;
    std::vector<Stage2_exact_mask_class>
                                        exact_mask_classes;
    std::vector<Stage2_glyph_mask>      glyph_masks;
};

struct Stage2_render_result
{
    QImage                              image;
    term::Qsg_atlas_stage1_frame_report atlas_report;
    bool                                ready = false;
};

int pixel_delta(const QColor& left, const QColor& right);

term::terminal_cell_metrics_t stage2_metrics()
{
    term::Qt_grid_metrics_provider provider(
        term::vnm_terminal_font(QString(), 18.0),
        1.0);
    return provider.cell_metrics();
}

QRectF stage2_cell_rect(
    int                         row,
    int                         column,
    int                         column_count,
    term::terminal_cell_metrics_t metrics)
{
    return QRectF(
        static_cast<qreal>(column) * metrics.width,
        static_cast<qreal>(row) * metrics.height,
        static_cast<qreal>(column_count) * metrics.width,
        metrics.height);
}

QRectF inset_rect(QRectF rect, qreal dx, qreal dy)
{
    return rect.adjusted(dx, dy, -dx, -dy);
}

term::Terminal_render_options stage2_render_options()
{
    term::Terminal_render_options options;
    options.packed_text_sidecars_enabled = false;
    return options;
}

term::Terminal_render_frame stage2_expected_frame(
    const Stage2_parity_fixture& fixture,
    term::terminal_cell_metrics_t metrics)
{
    return term::build_terminal_render_frame(
        &fixture.snapshot,
        fixture.logical_size,
        metrics,
        stage2_render_options(),
        true,
        &fixture.snapshot.ime_preedit);
}

void append_exact_class(
    Stage2_parity_fixture& fixture,
    std::string            name,
    std::vector<QRectF>    masks)
{
    masks.erase(
        std::remove_if(
            masks.begin(),
            masks.end(),
            [](const QRectF& rect) {
                return rect.width() <= 0.0 || rect.height() <= 0.0;
            }),
        masks.end());
    fixture.exact_mask_classes.push_back({
        std::move(name),
        std::move(masks),
    });
}

std::vector<QRectF> decoration_masks(
    const term::Terminal_render_frame&      frame,
    term::Terminal_render_decoration_kind   kind)
{
    std::vector<QRectF> masks;
    for (const term::Terminal_render_decoration& decoration : frame.decorations) {
        if (decoration.kind == kind) {
            masks.push_back(decoration.rect);
        }
    }
    return masks;
}

std::vector<QRectF> rect_masks(
    const std::vector<term::Terminal_render_rect>& rects)
{
    std::vector<QRectF> masks;
    masks.reserve(rects.size());
    for (const term::Terminal_render_rect& rect : rects) {
        masks.push_back(rect.rect);
    }
    return masks;
}

std::vector<QRectF> inset_masks(
    std::vector<QRectF> masks,
    qreal               dx,
    qreal               dy)
{
    for (QRectF& mask : masks) {
        mask = inset_rect(mask, dx, dy);
    }
    return masks;
}

std::vector<QRectF> cursor_masks(
    const std::vector<term::Terminal_render_cursor_primitive>& cursors)
{
    std::vector<QRectF> masks;
    masks.reserve(cursors.size());
    for (const term::Terminal_render_cursor_primitive& cursor : cursors) {
        masks.push_back(cursor.rect);
    }
    return masks;
}

std::vector<QRectF> cursor_text_fill_masks(QRectF cursor_rect)
{
    const qreal strip_width = 2.0;
    const qreal inset_y     = 2.0;
    const qreal height      = std::max<qreal>(2.0, std::floor(cursor_rect.height() * 0.3));
    if (cursor_rect.width() <= strip_width * 3.0 || height <= 0.0) {
        return {};
    }

    return {
        QRectF(
            cursor_rect.left() + 1.0,
            cursor_rect.top()  + inset_y,
            strip_width,
            height),
        QRectF(
            cursor_rect.right() - strip_width - 1.0,
            cursor_rect.top()   + inset_y,
            strip_width,
            height),
    };
}

void append_text_glyph_masks(
    Stage2_parity_fixture&              fixture,
    const std::vector<term::Terminal_render_text_run>& runs,
    std::optional<QColor>               background_override = std::nullopt)
{
    for (const term::Terminal_render_text_run& run : runs) {
        if (run.text.isEmpty()) {
            continue;
        }

        const QColor background = background_override.value_or(run.background);
        fixture.glyph_masks.push_back({
            run.rect,
            background,
            pixel_delta(run.foreground, background) > 8,
        });
    }
}

term::Terminal_color_state stage2_color_state()
{
    term::Terminal_color_state state;
    state.default_foreground_rgba = 0xffd8dee9U;
    state.default_background_rgba = 0xff101820U;
    state.cursor_rgba             = 0xfff8f8f2U;
    for (std::size_t index = 0; index < state.palette_rgba.size(); ++index) {
        state.palette_rgba[index] = 0xff202020U + static_cast<quint32>(index);
    }
    return state;
}

term::Terminal_text_style rgb_style(
    quint32       foreground,
    quint32       background,
    std::uint16_t attributes = 0U)
{
    term::Terminal_text_style style = term::make_default_terminal_text_style();
    style.foreground = term::make_rgb_terminal_color_ref(foreground);
    style.background = term::make_rgb_terminal_color_ref(background);
    style.attributes = attributes;
    return style;
}

term::Terminal_render_cell make_stage2_cell(
    int                    row,
    int                    column,
    QString                text,
    int                    display_width,
    term::Terminal_style_id style_id)
{
    return {
        {row, column},
        term::Terminal_render_cell_text::from_source_cell(
            text,
            display_width,
            false),
        0U,
        display_width,
        false,
        style_id,
        term::render_cell_text_category_for_validation(text),
    };
}

term::Terminal_render_cell make_stage2_continuation_cell(
    int                    row,
    int                    column,
    term::Terminal_style_id style_id)
{
    const QString empty_text;
    return {
        {row, column},
        term::Terminal_render_cell_text::from_source_cell(empty_text, 0, true),
        0U,
        0,
        true,
        style_id,
        term::Terminal_render_cell_text_category::EMPTY,
    };
}

term::Terminal_render_snapshot make_stage2_base_snapshot(
    term::terminal_grid_size_t grid_size,
    std::uint64_t              sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = grid_size.rows;
    viewport.follow_tail      = true;
    viewport.active_buffer    = term::Terminal_buffer_id::PRIMARY;
    viewport.offset_from_tail = 0;
    viewport.scrollback_rows  = 0;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot(grid_size, viewport, sequence);
    snapshot.color_state      = stage2_color_state();
    snapshot.dirty_row_ranges = {{0, grid_size.rows}};
    snapshot.cursor.position  = {0, 0};
    snapshot.cursor.visible   = false;
    snapshot.visible_line_provenance.reserve(static_cast<std::size_t>(grid_size.rows));
    for (int row = 0; row < grid_size.rows; ++row) {
        snapshot.visible_line_provenance.push_back({
            row,
            static_cast<std::uint64_t>(row + 1),
            1U,
        });
    }
    return snapshot;
}

std::vector<Stage2_parity_fixture> make_stage2_parity_fixtures()
{
    const term::terminal_cell_metrics_t metrics = stage2_metrics();

    Stage2_parity_fixture fills;
    fills.name = "fills_decorations_cursor";
    fills.snapshot = make_stage2_base_snapshot({4, 12}, 710U);
    fills.logical_size = QSizeF(metrics.width * 12.0, metrics.height * 4.0);
    const term::Terminal_style_id red_background = 1U;
    const term::Terminal_style_id underline      = 2U;
    const term::Terminal_style_id strike         = 3U;
    fills.snapshot.styles.push_back(rgb_style(0xfff8f8f2U, 0xff402020U));
    fills.snapshot.styles.push_back(rgb_style(
        0xff80ff80U,
        0xff101820U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::UNDERLINE)));
    fills.snapshot.styles.push_back(rgb_style(
        0xff80d0ffU,
        0xff101820U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::STRIKE)));
    fills.snapshot.selection_spans.push_back({
        {{0, 1}, {0, 5}, term::Terminal_selection_mode::NORMAL},
        0,
        1,
        4,
    });
    fills.snapshot.cursor.position = {1, 6};
    fills.snapshot.cursor.visible  = false;
    fills.snapshot.ime_preedit.active = true;
    fills.snapshot.ime_preedit.text = QStringLiteral(" ");
    fills.snapshot.ime_preedit.cursor_position = 1;
    fills.snapshot.cells.push_back(
        make_stage2_cell(1, 1, QStringLiteral(" "), 1, red_background));
    fills.snapshot.cells.push_back(
        make_stage2_cell(2, 2, QStringLiteral(" "), 1, underline));
    fills.snapshot.cells.push_back(
        make_stage2_cell(2, 4, QStringLiteral(" "), 1, strike));
    const term::Terminal_render_frame fills_frame =
        stage2_expected_frame(fills, metrics);
    append_exact_class(
        fills,
        "selection_fill",
        {inset_rect(stage2_cell_rect(0, 1, 4, metrics), 2.0, 2.0)});
    append_exact_class(
        fills,
        "style_background_fill",
        {inset_rect(stage2_cell_rect(1, 1, 1, metrics), 2.0, 2.0)});
    append_exact_class(
        fills,
        "preedit_background_fill",
        {inset_rect(stage2_cell_rect(1, 6, 1, metrics), 2.0, 2.0)});
    append_exact_class(
        fills,
        "underline_decoration",
        decoration_masks(
            fills_frame,
            term::Terminal_render_decoration_kind::UNDERLINE));
    append_exact_class(
        fills,
        "strike_decoration",
        decoration_masks(
            fills_frame,
            term::Terminal_render_decoration_kind::STRIKE));
    append_exact_class(
        fills,
        "preedit_caret",
        decoration_masks(
            fills_frame,
            term::Terminal_render_decoration_kind::PREEDIT_CARET));

    Stage2_parity_fixture glyphs;
    glyphs.name = "ascii_bmp_attributes";
    glyphs.snapshot = make_stage2_base_snapshot({3, 14}, 711U);
    glyphs.logical_size = QSizeF(metrics.width * 14.0, metrics.height * 3.0);
    const term::Terminal_style_id normal    = 1U;
    const term::Terminal_style_id faint     = 2U;
    const term::Terminal_style_id inverse   = 3U;
    const term::Terminal_style_id invisible = 4U;
    glyphs.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    glyphs.snapshot.styles.push_back(rgb_style(
        0xffffd166U,
        0xff101820U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::FAINT)));
    glyphs.snapshot.styles.push_back(rgb_style(
        0xff3ddc97U,
        0xff203850U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::INVERSE)));
    glyphs.snapshot.styles.push_back(rgb_style(
        0xffff6b6bU,
        0xff203020U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::INVISIBLE)));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(0, 1, QStringLiteral("Atlas"), 5, normal));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(1, 1, QString::fromUtf8("\xce\xa9"), 1, normal));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(1, 3, QString::fromUtf8("\xe7\x95\x8c"), 2, normal));
    glyphs.snapshot.cells.push_back(make_stage2_continuation_cell(1, 4, normal));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(2, 1, QStringLiteral("F"), 1, faint));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(2, 3, QStringLiteral("I"), 1, inverse));
    glyphs.snapshot.cells.push_back(
        make_stage2_cell(2, 5, QStringLiteral("X"), 1, invisible));
    append_text_glyph_masks(
        glyphs,
        stage2_expected_frame(glyphs, metrics).text_runs);

    Stage2_parity_fixture cursor;
    cursor.name = "block_cursor_text";
    cursor.snapshot = make_stage2_base_snapshot({1, 10}, 712U);
    cursor.logical_size = QSizeF(metrics.width * 10.0, metrics.height);
    cursor.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    cursor.snapshot.cursor.position      = {0, 2};
    cursor.snapshot.cursor.visible       = true;
    cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    cursor.snapshot.cursor.blink_enabled = false;
    cursor.snapshot.cells.push_back(
        make_stage2_cell(0, 2, QStringLiteral("."), 1, normal));
    const term::Terminal_render_frame cursor_frame =
        stage2_expected_frame(cursor, metrics);
    append_exact_class(
        cursor,
        "block_cursor_fill_outside_glyph",
        cursor_text_fill_masks(stage2_cell_rect(0, 2, 1, metrics)));
    append_text_glyph_masks(
        cursor,
        cursor_frame.cursor_text_runs,
        stage2_render_options().cursor_color);

    Stage2_parity_fixture empty_cursor;
    empty_cursor.name = "block_cursor_empty_fill";
    empty_cursor.snapshot = make_stage2_base_snapshot({1, 8}, 713U);
    empty_cursor.logical_size = QSizeF(metrics.width * 8.0, metrics.height);
    empty_cursor.snapshot.cursor.position      = {0, 3};
    empty_cursor.snapshot.cursor.visible       = true;
    empty_cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    empty_cursor.snapshot.cursor.blink_enabled = false;
    append_exact_class(
        empty_cursor,
        "empty_block_cursor_fill",
        {inset_rect(stage2_cell_rect(0, 3, 1, metrics), 2.0, 2.0)});

    Stage2_parity_fixture graphic_cursor;
    graphic_cursor.name = "block_cursor_graphic_carveout";
    graphic_cursor.snapshot = make_stage2_base_snapshot({1, 8}, 714U);
    graphic_cursor.logical_size = QSizeF(metrics.width * 8.0, metrics.height);
    graphic_cursor.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    graphic_cursor.snapshot.cursor.position      = {0, 3};
    graphic_cursor.snapshot.cursor.visible       = true;
    graphic_cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    graphic_cursor.snapshot.cursor.blink_enabled = false;
    graphic_cursor.snapshot.cells.push_back(
        make_stage2_cell(0, 3, QString::fromUtf8("\xe2\x96\x8c"), 1, normal));
    const term::Terminal_render_frame graphic_frame =
        stage2_expected_frame(graphic_cursor, metrics);
    append_exact_class(
        graphic_cursor,
        "graphic_cursor_fill_carveout",
        inset_masks(cursor_masks(graphic_frame.cursors), 1.0, 2.0));
    append_exact_class(
        graphic_cursor,
        "graphic_cursor_overlay",
        inset_masks(rect_masks(graphic_frame.cursor_graphic_rects), 1.0, 2.0));

    return {
        std::move(fills),
        std::move(glyphs),
        std::move(cursor),
        std::move(empty_cursor),
        std::move(graphic_cursor),
    };
}

Stage2_aa_budget stage2_budget_for_backend(
    const char*         backend,
    const std::string&  fixture_name)
{
    if (fixture_name == "ascii_bmp_attributes") {
        if (std::strcmp(backend, "d3d11") == 0) {
            return {3003, 230, 1350, 850, 1050};
        }
        if (std::strcmp(backend, "d3d12") == 0) {
            return {3003, 230, 1350, 850, 1050};
        }
        if (std::strcmp(backend, "vulkan") == 0) {
            return {3003, 230, 1350, 850, 1050};
        }
        if (std::strcmp(backend, "opengl") == 0) {
            return {3003, 230, 1350, 850, 1050};
        }
        return {3003, 230, 1350, 850, 1050};
    }

    if (fixture_name == "block_cursor_text") {
        if (std::strcmp(backend, "d3d11") == 0) {
            return {276, 230, 120, 25, 55};
        }
        if (std::strcmp(backend, "d3d12") == 0) {
            return {276, 230, 120, 25, 55};
        }
        if (std::strcmp(backend, "vulkan") == 0) {
            return {276, 230, 120, 25, 55};
        }
        if (std::strcmp(backend, "opengl") == 0) {
            return {276, 230, 120, 25, 55};
        }
        return {276, 230, 120, 25, 55};
    }

    return {};
}

QRect logical_rect_to_pixels(const QRectF& rect, qreal dpr)
{
    const int left   = static_cast<int>(std::floor(rect.left() * dpr));
    const int top    = static_cast<int>(std::floor(rect.top() * dpr));
    const int right  = static_cast<int>(std::ceil(rect.right() * dpr));
    const int bottom = static_cast<int>(std::ceil(rect.bottom() * dpr));
    return QRect(
        QPoint(left, top),
        QSize(std::max(0, right - left), std::max(0, bottom - top)));
}

int pixel_delta(const QColor& left, const QColor& right)
{
    return std::max({
        std::abs(left.red()   - right.red()),
        std::abs(left.green() - right.green()),
        std::abs(left.blue()  - right.blue()),
        std::abs(left.alpha() - right.alpha()),
    });
}

Stage2_diff_stats compare_regions(
    const QImage&              left,
    const QImage&              right,
    const std::vector<QRectF>& logical_regions)
{
    Stage2_diff_stats stats;
    const qreal dpr = std::max<qreal>(1.0, left.devicePixelRatio());
    for (const QRectF& logical_region : logical_regions) {
        const QRect region = logical_rect_to_pixels(logical_region, dpr)
            .intersected(left.rect())
            .intersected(right.rect());
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                ++stats.compared_pixels;
                const int delta = pixel_delta(
                    left.pixelColor(x, y),
                    right.pixelColor(x, y));
                stats.max_delta = std::max(stats.max_delta, delta);
                if (delta != 0) {
                    ++stats.diff_pixels;
                }
            }
        }
    }
    return stats;
}

bool first_region_diff(
    const QImage&              left,
    const QImage&              right,
    const std::vector<QRectF>& logical_regions,
    QPoint&                    out_position,
    QColor&                    out_left,
    QColor&                    out_right)
{
    const qreal dpr = std::max<qreal>(1.0, left.devicePixelRatio());
    for (const QRectF& logical_region : logical_regions) {
        const QRect region = logical_rect_to_pixels(logical_region, dpr)
            .intersected(left.rect())
            .intersected(right.rect());
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                const QColor left_pixel  = left.pixelColor(x, y);
                const QColor right_pixel = right.pixelColor(x, y);
                if (pixel_delta(left_pixel, right_pixel) != 0) {
                    out_position = QPoint(x, y);
                    out_left     = left_pixel;
                    out_right    = right_pixel;
                    return true;
                }
            }
        }
    }
    return false;
}

Stage2_glyph_stats compare_glyph_regions(
    const QImage&                         reference,
    const QImage&                         atlas,
    const std::vector<Stage2_glyph_mask>& logical_regions)
{
    constexpr int k_ink_delta_threshold = 8;

    Stage2_glyph_stats stats;
    const qreal dpr = std::max<qreal>(1.0, reference.devicePixelRatio());
    for (const Stage2_glyph_mask& logical_region : logical_regions) {
        const QRect region = logical_rect_to_pixels(logical_region.rect, dpr)
            .intersected(reference.rect())
            .intersected(atlas.rect());
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                ++stats.compared_pixels;
                const QColor reference_pixel = reference.pixelColor(x, y);
                const QColor atlas_pixel     = atlas.pixelColor(x, y);
                const int delta = pixel_delta(reference_pixel, atlas_pixel);
                stats.max_delta = std::max(stats.max_delta, delta);
                if (delta != 0) {
                    ++stats.diff_pixels;
                }
                if (logical_region.require_ink) {
                    if (pixel_delta(reference_pixel, logical_region.background) >
                        k_ink_delta_threshold)
                    {
                        ++stats.reference_ink_pixels;
                    }
                    if (pixel_delta(atlas_pixel, logical_region.background) >
                        k_ink_delta_threshold)
                    {
                        ++stats.atlas_ink_pixels;
                    }
                }
            }
        }
    }
    return stats;
}

bool pump_stage2_surface(
    QGuiApplication&     app,
    QQuickWindow&        window,
    VNM_TerminalSurface& surface,
    bool                 atlas,
    Stage2_render_result& out)
{
    for (int attempt = 0; attempt < 160; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out.image = window.grabWindow();
        if (out.image.isNull()) {
            continue;
        }

        if (atlas) {
            out.atlas_report =
                term::VNM_TerminalSurface_render_bridge::qsg_atlas_stage1_frame(surface);
            if (out.atlas_report.render_count > 0U && out.atlas_report.drew) {
                out.ready = true;
                return true;
            }
        }
        else {
            const term::terminal_renderer_stats_t stats =
                term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
            if (stats.paint_completed) {
                out.ready = true;
                return true;
            }
        }
    }

    return false;
}

Stage2_render_result render_stage2_fixture(
    QGuiApplication&              app,
    const Stage2_parity_fixture&  fixture,
    bool                          atlas)
{
    QQuickWindow window;
    window.setColor(QColor(1, 2, 3));
    window.resize(
        static_cast<int>(std::ceil(fixture.logical_size.width())),
        static_cast<int>(std::ceil(fixture.logical_size.height())));

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(fixture.logical_size);
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(surface, true);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(fixture.snapshot));
    if (term::ime_preedit_has_content(fixture.snapshot.ime_preedit)) {
        const QList<QInputMethodEvent::Attribute> attributes = {
            QInputMethodEvent::Attribute(
                QInputMethodEvent::Cursor,
                fixture.snapshot.ime_preedit.cursor_position,
                1,
                QVariant()),
        };
        QInputMethodEvent event(fixture.snapshot.ime_preedit.text, attributes);
        QCoreApplication::sendEvent(&surface, &event);
    }
    if (atlas) {
        term::VNM_TerminalSurface_render_bridge::set_qsg_atlas_stage1_probe_enabled(
            surface,
            true);
    }

    window.show();
    Stage2_render_result result;
    pump_stage2_surface(app, window, surface, atlas, result);
    window.hide();
    app.processEvents(QEventLoop::AllEvents, 50);
    return result;
}

bool compare_stage2_fixture(
    const Stage2_parity_fixture& fixture,
    const Stage2_render_result&  reference,
    const Stage2_render_result&  atlas,
    const char*                  backend)
{
    if (!reference.ready || !atlas.ready) {
        std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
            << " did not render both backends\n";
        return false;
    }
    if (reference.image.size() != atlas.image.size()) {
        std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
            << " rendered different image sizes\n";
        return false;
    }

    bool ok = true;
    int exact_compared_pixels = 0;
    int exact_diff_pixels     = 0;
    int exact_max_delta       = 0;
    for (const Stage2_exact_mask_class& exact_class : fixture.exact_mask_classes) {
        const Stage2_diff_stats exact = compare_regions(
            reference.image,
            atlas.image,
            exact_class.masks);
        exact_compared_pixels += exact.compared_pixels;
        exact_diff_pixels     += exact.diff_pixels;
        exact_max_delta        = std::max(exact_max_delta, exact.max_delta);
        if (exact.compared_pixels == 0) {
            std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
                << " exact class " << exact_class.name
                << " compared zero pixels\n";
            ok = false;
        }
        if (exact.diff_pixels != 0) {
            std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
                << " exact class " << exact_class.name
                << " changed " << exact.diff_pixels
                << " / " << exact.compared_pixels
                << " pixels; max delta " << exact.max_delta << '\n';
            QPoint position;
            QColor reference_pixel;
            QColor atlas_pixel;
            if (first_region_diff(
                    reference.image,
                    atlas.image,
                    exact_class.masks,
                    position,
                    reference_pixel,
                    atlas_pixel))
            {
                std::cerr << "  first exact diff x=" << position.x()
                    << " y=" << position.y()
                    << " reference=(" << reference_pixel.red()
                    << ',' << reference_pixel.green()
                    << ',' << reference_pixel.blue()
                    << ',' << reference_pixel.alpha()
                    << ") atlas=(" << atlas_pixel.red()
                    << ',' << atlas_pixel.green()
                    << ',' << atlas_pixel.blue()
                    << ',' << atlas_pixel.alpha()
                    << ")\n";
            }
            ok = false;
        }
    }

    const Stage2_aa_budget budget = stage2_budget_for_backend(backend, fixture.name);
    const Stage2_glyph_stats glyphs = compare_glyph_regions(
        reference.image,
        atlas.image,
        fixture.glyph_masks);
    if (!fixture.glyph_masks.empty() &&
        glyphs.compared_pixels != budget.compared_pixels)
    {
        std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
            << " glyph compared pixels changed on " << backend
            << ": compared pixels " << glyphs.compared_pixels
            << " / " << budget.compared_pixels << '\n';
        ok = false;
    }
    if (!fixture.glyph_masks.empty() &&
        (glyphs.max_delta > budget.max_delta ||
        glyphs.diff_pixels > budget.diff_pixels)
    ) {
        std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
            << " glyph budget exceeded on " << backend
            << ": diff pixels " << glyphs.diff_pixels << " / " << budget.diff_pixels
            << ", max delta " << glyphs.max_delta << " / " << budget.max_delta << '\n';
        ok = false;
    }
    if (!fixture.glyph_masks.empty() &&
        (glyphs.atlas_ink_pixels < budget.min_ink_pixels ||
            glyphs.atlas_ink_pixels > budget.max_ink_pixels))
    {
        std::cerr << "FAIL: Stage 2 parity fixture " << fixture.name
            << " glyph ink budget exceeded on " << backend
            << ": atlas ink pixels " << glyphs.atlas_ink_pixels
            << " / [" << budget.min_ink_pixels
            << ", " << budget.max_ink_pixels << "]"
            << ", reference ink pixels " << glyphs.reference_ink_pixels << '\n';
        ok = false;
    }

    if (ok) {
        std::cout << "PASS: Stage 2 parity " << fixture.name
            << " backend=" << backend
            << " exact_diff_pixels=" << exact_diff_pixels
            << " exact_compared_pixels=" << exact_compared_pixels
            << " exact_max_delta=" << exact_max_delta
            << " glyph_diff_pixels=" << glyphs.diff_pixels
            << " glyph_compared_pixels=" << glyphs.compared_pixels
            << " glyph_max_delta=" << glyphs.max_delta
            << " glyph_budget_pixels=" << budget.diff_pixels
            << " glyph_budget_max_delta=" << budget.max_delta
            << " glyph_reference_ink_pixels=" << glyphs.reference_ink_pixels
            << " glyph_atlas_ink_pixels=" << glyphs.atlas_ink_pixels
            << " glyph_budget_ink_pixels=[" << budget.min_ink_pixels
            << ", " << budget.max_ink_pixels << "]\n";
    }
    return ok;
}

int test_stage2_parity(QGuiApplication& app, const char* backend)
{
    bool ok = true;
    for (const Stage2_parity_fixture& fixture : make_stage2_parity_fixtures()) {
        const Stage2_render_result reference = render_stage2_fixture(app, fixture, false);
        const Stage2_render_result atlas = render_stage2_fixture(app, fixture, true);
        if (!atlas.ready &&
            (atlas.atlas_report.prepare_count == 0U ||
                atlas.atlas_report.render_count == 0U ||
                (atlas.atlas_report.prepare_count > 0U && !atlas.atlas_report.rhi_non_null)))
        {
            std::cerr << "SKIP: Stage 2 parity did not reach usable QRhi render state\n";
            return k_unsupported_backend_skip_return_code;
        }

        ok &= compare_stage2_fixture(fixture, reference, atlas, backend);
    }
    return ok ? 0 : 1;
}

bool read_source_file(const char* path, QByteArray& out)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "FAIL: could not open source file: " << path << '\n';
        return false;
    }

    out = file.readAll();
    return true;
}

bool source_contains_once(const QByteArray& source, const QByteArray& token)
{
    const qsizetype first = source.indexOf(token);
    return first >= 0 && source.indexOf(token, first + token.size()) < 0;
}

bool test_source_posture()
{
    QByteArray atlas_source;
    QByteArray surface_source;
    bool ok = true;
    ok &= check(read_source_file(VNM_TERMINAL_QSG_ATLAS_STAGE1_SOURCE_PATH, atlas_source),
        "atlas Stage 1 source path is readable");
    ok &= check(read_source_file(VNM_TERMINAL_SURFACE_SOURCE_PATH, surface_source),
        "surface source path is readable");
    if (!ok) {
        return false;
    }

    const std::vector<QByteArray> forbidden_atlas_tokens = {
        QByteArrayLiteral("VNM_TerminalSurface"),
        QByteArrayLiteral("m_private"),
        QByteArrayLiteral("boundingRect()"),
        QByteArrayLiteral("render_options_for_surface"),
        QByteArrayLiteral("QQuickItem"),
        QByteArrayLiteral("QQuickWindow"),
        QByteArrayLiteral("requestUpdate"),
        QByteArrayLiteral("request_render_update"),
        QByteArrayLiteral("updateInputMethod"),
        QByteArrayLiteral("->update("),
        QByteArrayLiteral(".update("),
    };
    for (const QByteArray& token : forbidden_atlas_tokens) {
        if (atlas_source.contains(token)) {
            std::cerr << "FAIL: Stage 1 atlas render-node source contains forbidden token "
                << token.constData() << '\n';
            ok = false;
        }
    }

    ok &= check(atlas_source.contains(QByteArrayLiteral("void prepare() override")),
        "Stage 1 atlas source defines QSGRenderNode::prepare");
    ok &= check(atlas_source.contains(QByteArrayLiteral("void render(const RenderState* state) override")),
        "Stage 1 atlas source defines QSGRenderNode::render");
    ok &= check(atlas_source.contains(QByteArrayLiteral("commandBuffer()")) &&
            atlas_source.contains(QByteArrayLiteral("renderTarget()")),
        "Stage 1 atlas prepare/render use QSGRenderNode render-thread accessors");
    ok &= check(atlas_source.contains(QByteArrayLiteral("m_frame")),
        "Stage 1 atlas prepare/render consume the captured frame value");

    const QByteArray capture_call =
        QByteArrayLiteral("capture_qsg_atlas_stage1_frame(");
    ok &= check(source_contains_once(surface_source, capture_call),
        "Captured_atlas_frame builder is called exactly once from the surface source");

    const qsizetype update_paint_node = surface_source.indexOf(
        QByteArrayLiteral("QSGNode* VNM_TerminalSurface::updatePaintNode"));
    const qsizetype bridge_start = surface_source.indexOf(
        QByteArrayLiteral("void term::VNM_TerminalSurface_render_bridge::set_render_snapshot"));
    const qsizetype capture_pos = surface_source.indexOf(capture_call);
    ok &= check(update_paint_node >= 0 &&
            bridge_start > update_paint_node &&
            capture_pos > update_paint_node &&
            capture_pos < bridge_start,
        "Captured_atlas_frame builder call is inside updatePaintNode");
    return ok;
}

bool test_cache_key_includes_physical_size_and_face()
{
    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const QRawFont raw_font = QRawFont::fromFont(font);
    const QString face_id = term::qsg_atlas_face_id_for_raw_font(raw_font);
    const term::Glyph_atlas_cache_key base_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        term::qsg_atlas_physical_pixel_size(font, 1.0),
        0);
    const term::Glyph_atlas_cache_key hidpi_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        term::qsg_atlas_physical_pixel_size(font, 2.0),
        0);
    const term::Glyph_atlas_cache_key fallback_key = term::qsg_atlas_cache_key(
        65U,
        QStringLiteral("fallback-face"),
        term::qsg_atlas_physical_pixel_size(font, 1.0),
        0);
    const qreal raw_physical_pixel_size =
        term::qsg_atlas_physical_pixel_size(raw_font, 2.0);

    bool ok = true;
    ok &= check(!face_id.isEmpty(),
        "raw-font face id is populated for cache keys");
    ok &= check(!(base_key == hidpi_key),
        "glyph cache key includes physical pixel size");
    ok &= check(!(base_key == fallback_key),
        "glyph cache key includes fallback face id");
    ok &= check(std::abs(raw_physical_pixel_size - raw_font.pixelSize() * 2.0) < 0.001,
        "raw-font physical pixel size is derived from the run font");
    return ok;
}

bool test_packing_and_stride_copy()
{
    term::Glyph_atlas_packer packer(QSize(10, 8), 1);
    const std::optional<term::Glyph_atlas_slot> first  = packer.pack(QSize(3, 2));
    const std::optional<term::Glyph_atlas_slot> second = packer.pack(QSize(3, 2));
    const std::optional<term::Glyph_atlas_slot> third  = packer.pack(QSize(3, 2));

    bool ok = true;
    ok &= check(first.has_value() && second.has_value() && third.has_value(),
        "shelf packer places multiple glyph tiles");
    ok &= check(first->page == 0 && second->page == 0 && third->page == 0,
        "shelf packer keeps fitting glyphs on one page");
    ok &= check(!first->rect.intersects(second->rect) &&
            !first->rect.intersects(third->rect) &&
            !second->rect.intersects(third->rect),
        "shelf packer places non-overlapping tile rects");

    term::Glyph_coverage_tile tile;
    tile.size           = QSize(2, 2);
    tile.bytes_per_line = 4;
    tile.bytes          = byte_array({1, 2, 90, 91, 3, 4, 92, 93});

    term::Glyph_atlas_cache cache(QSize(8, 8));
    cache.set_epoch(1U);
    const term::Glyph_atlas_cache_key key =
        term::qsg_atlas_cache_key(1U, QStringLiteral("face"), 12.0, 0);
    const term::Glyph_atlas_slot slot = cache.insert_or_get(key, tile);
    const QByteArray& page = cache.page_bytes(slot.page);
    const int stride = cache.stats().page_size.width();
    ok &= check(page.at(slot.rect.y() * stride + slot.rect.x()) == 1 &&
            page.at(slot.rect.y() * stride + slot.rect.x() + 1) == 2 &&
            page.at((slot.rect.y() + 1) * stride + slot.rect.x()) == 3 &&
            page.at((slot.rect.y() + 1) * stride + slot.rect.x() + 1) == 4,
        "atlas cache copies coverage rows using tile stride");

    term::Glyph_atlas_cache capped_cache(QSize(4, 4));
    capped_cache.set_epoch(1U);
    term::Glyph_coverage_tile capped_tile;
    capped_tile.size           = QSize(2, 2);
    capped_tile.bytes_per_line = 2;
    capped_tile.bytes          = byte_array({1, 2, 3, 4});
    const term::Glyph_atlas_slot capped_first = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(2U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    const term::Glyph_atlas_slot capped_second = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(3U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    ok &= check(capped_first.is_valid() &&
            !capped_second.is_valid() &&
            capped_cache.stats().page_count == 1,
        "Stage 1 atlas cache rejects glyphs that would require page 1");
    return ok;
}

bool test_indexed8_and_grayscale_conversion()
{
    uchar indexed_storage[16] = {
        10, 20, 30, 200, 201, 202, 203, 204,
        40, 50, 60, 205, 206, 207, 208, 209,
    };
    QImage indexed(
        indexed_storage,
        3,
        2,
        8,
        QImage::Format_Indexed8);
    QVector<QRgb> color_table(256);
    for (int index = 0; index < color_table.size(); ++index) {
        color_table[index] = qRgb(index, index, index);
    }
    indexed.setColorTable(color_table);

    const term::Glyph_coverage_tile indexed_tile =
        term::qsg_atlas_coverage_tile_from_image(indexed);

    uchar gray_storage[16] = {
        7, 8, 9, 200, 201, 202, 203, 204,
        17, 18, 19, 205, 206, 207, 208, 209,
    };
    QImage gray(
        gray_storage,
        3,
        2,
        8,
        QImage::Format_Grayscale8);
    const term::Glyph_coverage_tile gray_tile =
        term::qsg_atlas_coverage_tile_from_image(gray);

    QImage rgb(3, 2, QImage::Format_RGB32);
    rgb.fill(QColor(10, 20, 30));
    const term::Glyph_coverage_tile rgb_tile =
        term::qsg_atlas_coverage_tile_from_image(rgb);

    bool ok = true;
    ok &= check(indexed_tile.is_valid() &&
            indexed_tile.bytes_per_line == 3 &&
            indexed_tile.bytes == byte_array({10, 20, 30, 40, 50, 60}),
        "Format_Indexed8 glyph alpha map converts to tight R8 coverage rows");
    ok &= check(gray_tile.is_valid() &&
            gray_tile.bytes_per_line == 3 &&
            gray_tile.bytes == byte_array({7, 8, 9, 17, 18, 19}),
        "Format_Grayscale8 glyph alpha map conversion honors source stride");
    ok &= check(!rgb_tile.is_valid(),
        "RGB subpixel glyph maps are rejected for the R8 atlas");
    return ok;
}

bool test_epoch_invalidation()
{
    term::Glyph_coverage_tile tile;
    tile.size           = QSize(2, 2);
    tile.bytes_per_line = 2;
    tile.bytes          = byte_array({11, 12, 13, 14});

    term::Glyph_atlas_cache cache(QSize(8, 8));
    const term::Glyph_atlas_cache_key key =
        term::qsg_atlas_cache_key(4U, QStringLiteral("face"), 14.0, 0);

    cache.set_epoch(100U);
    const term::Glyph_atlas_slot first = cache.insert_or_get(key, tile);
    cache.set_epoch(100U);
    const term::Glyph_atlas_cache_stats stable_stats = cache.stats();
    cache.set_epoch(101U);
    const term::Glyph_atlas_cache_stats invalidated_stats = cache.stats();

    bool ok = true;
    ok &= check(first.is_valid(),
        "atlas cache inserts a glyph before epoch invalidation");
    ok &= check(stable_stats.invalidations == 0U &&
            stable_stats.page_count == 1,
        "same epoch does not invalidate atlas cache");
    ok &= check(cache.find(key) == nullptr &&
            invalidated_stats.invalidations == 1U &&
            invalidated_stats.page_count == 0,
        "changed font epoch invalidates atlas cache pages and keys");
    return ok;
}

bool report_ready_for_render(const term::Qsg_atlas_stage1_frame_report& report)
{
    return
        report.render_count > 0U            &&
        report.drew                         &&
        report.command_buffer_non_null      &&
        report.render_target_non_null       &&
        report.rhi_non_null                 &&
        report.r8_texture_created           &&
        report.r8_upload_recorded           &&
        report.raw_font_rasterized          &&
        report.raw_font_rasterized_in_prepare;
}

bool pump_until(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    const std::function<bool(const term::Qsg_atlas_stage1_frame_report&)>&
                        predicate,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_stage1_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_stage1_frame(surface);
        if (predicate(report)) {
            return true;
        }
    }

    return false;
}

void mutate_surface_after_sync(VNM_TerminalSurface& surface)
{
    surface.set_color_theme(QStringLiteral("default"));
    surface.set_font_size(surface.font_size() + 1.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_text_snapshot(777U, QStringLiteral("B"))));
}

int test_render_smoke(QGuiApplication& app)
{
    const QByteArray render_loop = qgetenv("QSG_RENDER_LOOP");
    const bool threaded_loop = render_loop == QByteArrayLiteral("threaded");
    const bool basic_loop    = render_loop == QByteArrayLiteral("basic");

    QQuickWindow window;
    window.setColor(QColor(4, 6, 8));
    window.resize(260, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(180.0, 96.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_theme(QStringLiteral("light"));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_text_snapshot(601U, QStringLiteral("A"))));
    term::VNM_TerminalSurface_render_bridge::set_qsg_atlas_stage1_probe_enabled(
        surface,
        true);

    std::atomic_bool mutation_started = false;
    std::atomic_bool mutation_done    = false;
    const QMetaObject::Connection mutation_connection = QObject::connect(
        &window,
        &QQuickWindow::beforeRendering,
        &surface,
        [&]() {
            bool expected = false;
            if (!mutation_started.compare_exchange_strong(expected, true)) {
                return;
            }

            const auto mutate = [&]() {
                mutate_surface_after_sync(surface);
                mutation_done.store(true);
            };

            if (QThread::currentThread() == surface.thread()) {
                mutate();
            }
            else {
                QMetaObject::invokeMethod(
                    &surface,
                    [&]() {
                        mutate_surface_after_sync(surface);
                        mutation_done.store(true);
                    },
                    Qt::QueuedConnection);
            }
        },
        Qt::DirectConnection);

    window.show();

    const bool rendered = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_stage1_frame_report& report) {
            return report_ready_for_render(report) && mutation_done.load();
        });
    QObject::disconnect(mutation_connection);

    const term::Qsg_atlas_stage1_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_stage1_frame(surface);
    if (!rendered &&
        (report.prepare_count == 0U ||
            report.render_count == 0U ||
            (report.prepare_count > 0U && !report.rhi_non_null)))
    {
        std::cerr << "SKIP: Stage 1 render smoke did not reach usable QRhi render state\n";
        return k_unsupported_backend_skip_return_code;
    }

    bool ok = true;
    ok &= check(rendered,
        "Stage 1 render smoke draws the captured atlas probe");
    ok &= check(report.first_render_snapshot_sequence == 601U,
        "Stage 1 render smoke uses captured snapshot for first render");
    ok &= check(report.first_render_snapshot_sequence ==
            report.first_captured_snapshot_sequence,
        "Stage 1 first render uses the first captured snapshot value");
    ok &= check(report.first_render_light_options,
        "Stage 1 render smoke uses captured options for first render");
    ok &= check(report.first_render_light_options == report.first_captured_light_options,
        "Stage 1 first render uses the first captured options value");
    ok &= check(report.first_render_font_epoch == report.first_captured_font_epoch,
        "Stage 1 first render uses the first captured font epoch");
    ok &= check(!threaded_loop || mutation_done.load(),
        "threaded Stage 1 smoke mutates GUI-thread state after sync through beforeRendering");
    ok &= check(!basic_loop || mutation_done.load(),
        "basic Stage 1 smoke mutates GUI state after sync through beforeRendering");
    ok &= check(report.raw_font_rasterized_in_prepare &&
            report.prepare_thread_id == report.raw_font_raster_thread_id,
        "Stage 1 smoke rasterizes QRawFont glyphs in prepare");
    return ok ? 0 : 1;
}

bool run_unit_tests()
{
    bool ok = true;
    ok &= test_source_posture();
    ok &= test_cache_key_includes_physical_size_and_face();
    ok &= test_packing_and_stride_copy();
    ok &= test_indexed8_and_grayscale_conversion();
    ok &= test_epoch_invalidation();
    return ok;
}

}

int main(int argc, char** argv)
{
    const bool render_smoke = has_argument(argc, argv, "--render-smoke");
    const bool stage2_parity = has_argument(argc, argv, "--stage2-parity");
    const char* backend = argument_value(argc, argv, "--backend", "d3d11");
    if (render_smoke || stage2_parity) {
        configure_graphics_api(backend);
    }

    QGuiApplication app(argc, argv);
    if (stage2_parity) {
        return test_stage2_parity(app, backend);
    }
    if (render_smoke) {
        return test_render_smoke(app);
    }

    return run_unit_tests() ? 0 : 1;
}
