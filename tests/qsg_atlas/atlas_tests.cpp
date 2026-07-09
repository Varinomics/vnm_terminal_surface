#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_atlas_font_bytes.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/qsg_atlas_warm_set.h"
#include "vnm_terminal/internal/terminal_graphic_geometry.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QGlyphRun>
#include <QImage>
#include <QInputMethodEvent>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1Char>
#include <QLibraryInfo>
#include <QMetaObject>
#include <QPainter>
#include <QSaveFile>
#include <QTextLayout>
#include <QTextOption>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRawFont>
#include <QRectF>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QThread>
#include <private/qquickitem_p.h>
#include <rhi/qrhi.h>

#if !defined(VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_ENABLED)
#define VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_ENABLED 0
#endif

#if !defined(VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED)
#define VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED 0
#endif

#if defined(VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_DIAGNOSTIC) || \
    VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_ENABLED
#if defined(__has_include)
#if __has_include(<vnm_msdf_text/msdf_text.h>)
#include <vnm_msdf_text/msdf_text.h>
#define VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE 1
#else
#error "MSDF diagnostic requires vnm_msdf_text headers and link target"
#endif
#else
#include <vnm_msdf_text/msdf_text.h>
#define VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE 1
#endif
#else
#define VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace {

#if !defined(VNM_TERMINAL_TEST_BUILD_TYPE)
#define VNM_TERMINAL_TEST_BUILD_TYPE "unknown"
#endif

#if !defined(VNM_TERMINAL_PROFILING_ENABLED)
#define VNM_TERMINAL_PROFILING_ENABLED 0
#endif

#if defined(NDEBUG)
constexpr bool k_lcd_probe_debug_build = false;
#else
constexpr bool k_lcd_probe_debug_build = true;
#endif

constexpr int k_lcd_repeated_w_cell_count = 8;
constexpr int k_lcd_repeated_w_row = 1;
constexpr int k_lcd_repeated_w_first_column = 1;
constexpr int k_lcd_repeated_w_column_stride = 2;
constexpr qreal k_lcd_repeated_w_translated_host_x = 16.0;
constexpr qreal k_lcd_repeated_w_translated_host_y = 8.0;
constexpr int k_lcd_contiguous_x_cell_count = 80;
constexpr int k_lcd_contiguous_x_row = 1;
constexpr int k_lcd_contiguous_x_first_column = 1;
constexpr int k_lcd_contiguous_x_column_stride = 1;
constexpr qreal k_lcd_contiguous_x_font_size =
    static_cast<qreal>(
        vnm_terminal::internal::k_vnm_terminal_default_font_pixel_size);
constexpr int k_lcd_intensity_x_columns = 96;
constexpr int k_lcd_intensity_x_first_row = 1;
constexpr int k_lcd_intensity_x_first_column = 1;
constexpr int k_lcd_intensity_x_column_stride = 1;
constexpr int k_lcd_intensity_x_uppercase_rows = 6;
constexpr int k_lcd_intensity_x_lowercase_rows = 5;
constexpr int k_lcd_intensity_x_rows =
    k_lcd_intensity_x_uppercase_rows +
    k_lcd_intensity_x_lowercase_rows;
constexpr qreal k_lcd_intensity_x_font_size = 10.0;
constexpr std::array<quint32, k_lcd_intensity_x_rows>
    k_lcd_intensity_x_foreground_rgba = {
        0xfff0f0f0U,
        0xffd8d8d8U,
        0xffc0c0c0U,
        0xffa8a8a8U,
        0xff909090U,
        0xff787878U,
        0xff686868U,
        0xff585858U,
        0xff484848U,
        0xff3f3f3fU,
        0xff383838U,
    };
constexpr int k_lcd_fragmented_x_columns = 96;
constexpr int k_lcd_fragmented_x_row = 1;
constexpr int k_lcd_fragmented_x_first_column = 1;
constexpr int k_lcd_fragmented_x_column_stride = 1;
constexpr qreal k_lcd_fragmented_x_font_size = 10.0;
constexpr std::array<quint32, 8> k_lcd_fragmented_x_foreground_rgba = {
    0xfff0f0f0U,
    0xffd8d8d8U,
    0xffc0c0c0U,
    0xffa8a8a8U,
    0xff909090U,
    0xff787878U,
    0xff686868U,
    0xff585858U,
};
constexpr int k_lcd_repeated_on_row = 1;
constexpr int k_lcd_repeated_on_first_column = 1;
constexpr int k_lcd_repeated_on_second_column = 96;
constexpr double k_lcd_repeated_on_fractional_host_physical_x = 0.5;
constexpr qreal k_lcd_repeated_on_live_app_font_size = 14.0;
constexpr qreal k_lcd_repeated_on_live_app_base_host_x = 6.0;
constexpr qreal k_lcd_repeated_on_live_app_base_host_y = 32.0;
constexpr int k_lcd_ascii_panel_representative_row = 1;
constexpr int k_lcd_ascii_panel_representative_column = 1;
constexpr int k_lcd_ascii_panel_representative_column_stride = 2;
constexpr int k_lcd_ascii_panel_dense_row = 2;
constexpr int k_lcd_ascii_panel_dense_column = 1;
constexpr int k_lcd_ascii_panel_cursor_row = 3;
constexpr int k_lcd_ascii_panel_cursor_column = 1;
constexpr int k_lcd_ascii_panel_user_text_row = 4;
constexpr int k_lcd_ascii_panel_user_text_column = 1;
constexpr double k_lcd_visible_strong_energy_threshold = 0.50;
constexpr quint32 k_lcd_w_probe_foreground_rgba = 0xffe6edf3U;
constexpr quint32 k_lcd_w_probe_background_rgba = 0xff101820U;

#if defined(_MSC_VER)
constexpr const char* k_lcd_probe_compiler = "msvc";
#elif defined(__clang__)
constexpr const char* k_lcd_probe_compiler = "clang";
#elif defined(__GNUC__)
constexpr const char* k_lcd_probe_compiler = "gcc";
#else
constexpr const char* k_lcd_probe_compiler = "unknown";
#endif

#if VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE
constexpr int k_msdf_single_w_atlas_texture_size = 2048;
constexpr double k_msdf_single_w_min_atlas_font_size = 48.0;
constexpr float k_msdf_single_w_atlas_px_range = 10.0f;
constexpr float k_msdf_single_w_sharpness_bias = 2.5f;
constexpr char32_t k_msdf_single_w_codepoint = U'W';
constexpr const char* k_terminal_monospace_font_resource =
    ":/vnm_terminal_surface/fonts/vnm_framework_monospace.ttf";
#endif

} // namespace

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

#if VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE
namespace msdf = vnm::msdf_text;
#endif

#ifndef VNM_TERMINAL_QSG_ATLAS_SOURCE_PATH
#define VNM_TERMINAL_QSG_ATLAS_SOURCE_PATH ""
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

void set_test_environment_if_empty(const char* name, const QByteArray& value)
{
    if (qEnvironmentVariableIsEmpty(name)) {
        qputenv(name, value);
    }
}

bool platform_plugins_support_offscreen(const QString& path)
{
    const QDir platforms(path);
    if (!platforms.exists()) {
        return false;
    }

#if defined(Q_OS_WIN)
    return
        platforms.exists(QStringLiteral("qoffscreen.dll")) ||
        platforms.exists(QStringLiteral("qoffscreend.dll"));
#elif defined(Q_OS_MACOS)
    return
        platforms.exists(QStringLiteral("libqoffscreen.dylib")) ||
        platforms.exists(QStringLiteral("libqoffscreend.dylib"));
#else
    return
        platforms.exists(QStringLiteral("libqoffscreen.so")) ||
        platforms.exists(QStringLiteral("libqoffscreend.so"));
#endif
}

void append_platform_plugin_candidate(
    std::vector<QString>& candidates,
    const QString&        path)
{
    if (!path.isEmpty()) {
        candidates.push_back(QDir::cleanPath(path));
    }
}

QString offscreen_platform_plugin_path()
{
    std::vector<QString> candidates;
    const QDir plugin_root(QLibraryInfo::path(QLibraryInfo::PluginsPath));
    append_platform_plugin_candidate(
        candidates,
        plugin_root.filePath(QStringLiteral("platforms")));

    const QByteArray qt_dir_value = qgetenv("QTDIR");
    if (!qt_dir_value.isEmpty()) {
        const QDir qt_dir(QString::fromLocal8Bit(qt_dir_value));
        append_platform_plugin_candidate(
            candidates,
            qt_dir.filePath(QStringLiteral("plugins/platforms")));
#if defined(Q_OS_WIN) && defined(_MSC_VER)
        append_platform_plugin_candidate(
            candidates,
            qt_dir.filePath(QStringLiteral("msvc2022_64/plugins/platforms")));
        append_platform_plugin_candidate(
            candidates,
            qt_dir.filePath(QStringLiteral("msvc2019_64/plugins/platforms")));
#endif
    }

    for (const QString& path : candidates) {
        if (platform_plugins_support_offscreen(path)) {
            return path;
        }
    }

    return QString();
}

void configure_unit_test_qt_environment()
{
    set_test_environment_if_empty("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH")) {
        const QString platform_plugins = offscreen_platform_plugin_path();
        if (!platform_plugins.isEmpty()) {
            qputenv(
                "QT_QPA_PLATFORM_PLUGIN_PATH",
                QFile::encodeName(platform_plugins));
        }
    }
    set_test_environment_if_empty("QT_SCALE_FACTOR", QByteArrayLiteral("1"));
    set_test_environment_if_empty("QT_SCREEN_SCALE_FACTORS", QByteArray());
    set_test_environment_if_empty("QT_AUTO_SCREEN_SCALE_FACTOR", QByteArrayLiteral("0"));
    set_test_environment_if_empty("QT_DEVICE_PIXEL_RATIO", QByteArray());
    set_test_environment_if_empty(
        "QT_SCALE_FACTOR_ROUNDING_POLICY",
        QByteArrayLiteral("PassThrough"));
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

term::Glyph_lcd_order test_lcd_order_for_kind(term::Glyph_coverage_kind kind)
{
    switch (kind) {
        case term::Glyph_coverage_kind::LCD_RGB_MASK:
            return term::Glyph_lcd_order::RGB;
        case term::Glyph_coverage_kind::LCD_BGR_MASK:
            return term::Glyph_lcd_order::BGR;
        case term::Glyph_coverage_kind::UNKNOWN:
        case term::Glyph_coverage_kind::GRAYSCALE_MASK:
        case term::Glyph_coverage_kind::COLOR_IMAGE:
        case term::Glyph_coverage_kind::AMBIGUOUS:
        case term::Glyph_coverage_kind::UNSUPPORTED:
            break;
    }
    return term::Glyph_lcd_order::UNKNOWN;
}

term::Glyph_rgba_tile test_rgba_tile(
    QSize                     size,
    int                       bytes_per_line,
    std::initializer_list<int> bytes,
    term::Glyph_coverage_kind kind = term::Glyph_coverage_kind::GRAYSCALE_MASK)
{
    term::Glyph_rgba_tile tile;
    tile.coverage_kind   = kind;
    tile.lcd_order       = test_lcd_order_for_kind(kind);
    tile.size            = size;
    tile.bytes_per_line  = bytes_per_line;
    tile.source_format   = QImage::Format_RGBA8888;
    tile.bytes           = byte_array(bytes);
    return tile;
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

QSGRendererInterface::GraphicsApi expected_graphics_api_for_backend(
    const char* backend)
{
    if (std::strcmp(backend, "d3d11") == 0) {
        return QSGRendererInterface::Direct3D11Rhi;
    }
    if (std::strcmp(backend, "d3d12") == 0) {
        return QSGRendererInterface::Direct3D12;
    }
    if (std::strcmp(backend, "vulkan") == 0) {
        return QSGRendererInterface::VulkanRhi;
    }
    if (std::strcmp(backend, "opengl") == 0) {
        return QSGRendererInterface::OpenGLRhi;
    }

    return QSGRendererInterface::Unknown;
}

QString graphics_api_name(QSGRendererInterface::GraphicsApi api)
{
    if (api == QSGRendererInterface::Unknown) {
        return QStringLiteral("unknown");
    }
    if (api == QSGRendererInterface::Software) {
        return QStringLiteral("software");
    }
    if (api == QSGRendererInterface::OpenVG) {
        return QStringLiteral("openvg");
    }
    if (api == QSGRendererInterface::Direct3D12) {
        return QStringLiteral("d3d12");
    }
    if (api == QSGRendererInterface::Direct3D11Rhi) {
        return QStringLiteral("d3d11");
    }
    if (api == QSGRendererInterface::VulkanRhi) {
        return QStringLiteral("vulkan");
    }
    if (api == QSGRendererInterface::MetalRhi) {
        return QStringLiteral("metal");
    }
    if (api == QSGRendererInterface::OpenGLRhi ||
        api == QSGRendererInterface::OpenGL)
    {
        return QStringLiteral("opengl");
    }
    if (api == QSGRendererInterface::Null) {
        return QStringLiteral("null");
    }

    return QStringLiteral("unrecognized");
}

QString driver_device_type_name(QRhiDriverInfo::DeviceType type)
{
    switch (type) {
        case QRhiDriverInfo::UnknownDevice:
            return QStringLiteral("unknown");
        case QRhiDriverInfo::IntegratedDevice:
            return QStringLiteral("integrated");
        case QRhiDriverInfo::DiscreteDevice:
            return QStringLiteral("discrete");
        case QRhiDriverInfo::ExternalDevice:
            return QStringLiteral("external");
        case QRhiDriverInfo::VirtualDevice:
            return QStringLiteral("virtual");
        case QRhiDriverInfo::CpuDevice:
            return QStringLiteral("cpu");
    }

    return QStringLiteral("unknown");
}

int verify_requested_backend(
    QGuiApplication& app,
    const char*      backend,
    const char*      test_name)
{
    const QSGRendererInterface::GraphicsApi expected =
        expected_graphics_api_for_backend(backend);
    if (expected == QSGRendererInterface::Unknown) {
        std::cerr << "FAIL: " << test_name << " requested unknown backend "
            << backend << '\n';
        return 1;
    }

    QQuickWindow probe;
    probe.resize(64, 64);
    probe.show();
    app.processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);
    app.processEvents(QEventLoop::AllEvents, 50);

    const QSGRendererInterface::GraphicsApi active =
        probe.rendererInterface() != nullptr
            ? probe.rendererInterface()->graphicsApi()
            : QSGRendererInterface::Unknown;
    probe.hide();
    app.processEvents(QEventLoop::AllEvents, 50);

    if (active == expected) {
        return 0;
    }

    std::cerr << "SKIP: " << test_name << " requested backend " << backend
        << " but Qt activated " << graphics_api_name(active).toUtf8().constData()
        << '\n';
    return k_unsupported_backend_skip_return_code;
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

struct Pixel_diff_stats
{
    int compared_pixels = 0;
    int diff_pixels     = 0;
    int max_delta       = 0;
};

struct Pixel_glyph_mask_stats
{
    int compared_pixels      = 0;
    int diff_pixels          = 0;
    int max_delta            = 0;
    int reference_ink_pixels = 0;
    int atlas_ink_pixels     = 0;
    int perimeter_pixels     = 0;
};

struct Pixel_glyph_stats
{
    int compared_pixels      = 0;
    int diff_pixels          = 0;
    int max_delta            = 0;
    int reference_ink_pixels = 0;
    int atlas_ink_pixels     = 0;
    int perimeter_pixels     = 0;
    std::vector<Pixel_glyph_mask_stats>
        masks;
};

struct Pixel_decorative_ink_stats
{
    QRect  bbox;
    double center_x   = 0.0;
    double center_y   = 0.0;
    int    ink_pixels = 0;

    bool has_ink() const { return ink_pixels > 0; }
};

struct Pixel_image_ink_stats
{
    QRect  bbox;
    double center_x   = 0.0;
    double center_y   = 0.0;
    int    ink_pixels = 0;

    bool has_ink() const { return ink_pixels > 0; }
};

struct Lcd_first_ink_column_sample
{
    int    y = 0;
    QColor color;
};

struct Lcd_first_ink_column_stats
{
    QRect                                    bbox;
    int                                      ink_pixels = 0;
    int                                      first_ink_x = -1;
    int                                      first_column_rows = 0;
    int                                      distinct_first_column_rgb_colors = 0;
    int                                      adjacent_equal_first_column_pairs = 0;
    int                                      longest_equal_first_column_run = 0;
    std::vector<Lcd_first_ink_column_sample> first_column;

    bool has_ink() const { return ink_pixels > 0; }
};

struct Pixel_aa_budget
{
    int    base_compared_pixels       = 0;
    int    max_delta                  = 0;
    int    diff_pixels                = 0;
    double ink_delta_perimeter_factor = 0.0;
};

struct Pixel_exact_mask_class
{
    std::string         name;
    std::vector<QRectF> masks;
    int                 max_delta = 0;
};

struct Pixel_glyph_mask
{
    QRectF rect;
    QColor background;
    bool   require_ink = true;
    QColor foreground;
};

struct Lcd_horizontal_edge_transition_stats
{
    QRect  bbox;
    int    sampled_rows             = 0;
    int    rows_with_transitions    = 0;
    int    transition_count         = 0;
    int    max_transition_width     = 0;
    double average_transition_width = 0.0;
};

struct Lcd_visible_glyph_metrics
{
    QRect               bbox;
    int                 strong_pixels = 0;
    int                 visible_left_x = -1;
    int                 visible_right_x = -1;
    int                 visible_top_y = -1;
    int                 visible_baseline_y = -1;
    int                 relative_visible_left_x = 0;
    int                 relative_visible_right_x = 0;
    int                 relative_visible_top_y = 0;
    int                 relative_visible_baseline_y = 0;
    double              energy_sum = 0.0;
    double              weighted_center_x = 0.0;
    double              weighted_center_y = 0.0;
    double              relative_weighted_center_x = 0.0;
    double              relative_weighted_center_y = 0.0;
    std::vector<double> row_energy;
    std::vector<double> column_energy;

    bool has_visible_ink() const { return strong_pixels > 0; }
};

struct Lcd_repeated_w_cell_stats
{
    QRect cell_pixels;
    QRect ink_bbox;
    int   ink_pixels = 0;
    int   first_ink_x = -1;
    int   relative_first_ink_x = 0;
    int   relative_bbox_x = 0;
    int   relative_bbox_y = 0;
    int   relative_bbox_width = 0;
    int   relative_bbox_height = 0;
};

struct Lcd_repeated_w_stability_stats
{
    int    cell_count = 0;
    double physical_cell_width = 0.0;
    double physical_cell_height = 0.0;
    double physical_cell_ascent = 0.0;
    double cell_width_integer_delta = 0.0;
    double cell_height_integer_delta = 0.0;
    double cell_ascent_integer_delta = 0.0;
    bool   cell_width_integer = false;
    bool   cell_height_integer = false;
    bool   cell_ascent_integer = false;
    int    compared_cells = 0;
    int    compared_pixels = 0;
    int    diff_pixels = 0;
    int    max_delta = 0;
    int    max_diff_pixels_per_cell = 0;
    int    ink_mask_diff_pixels = 0;
    int    max_ink_mask_diff_pixels_per_cell = 0;
    bool   all_cells_have_ink = false;
    int    max_ink_pixels_delta = 0;
    int    max_relative_first_ink_x_delta = 0;
    int    max_relative_bbox_x_delta = 0;
    int    max_relative_bbox_y_delta = 0;
    int    max_relative_bbox_width_delta = 0;
    int    max_relative_bbox_height_delta = 0;
    std::vector<Lcd_repeated_w_cell_stats> cells;
};

struct Lcd_intensity_x_row_stability
{
    int                            row_index = 0;
    int                            row = 0;
    char                           glyph = 'X';
    quint32                        foreground_rgba = 0U;
    Lcd_repeated_w_stability_stats stability;
};

struct Lcd_repeated_on_glyph_stats
{
    QString                   label;
    int                       row = 0;
    int                       column = 0;
    QRect                     cell_pixels;
    Lcd_visible_glyph_metrics visible;
};

struct Lcd_repeated_on_stability_stats
{
    double physical_cell_width = 0.0;
    double physical_cell_height = 0.0;
    double physical_cell_ascent = 0.0;
    int    expected_glyphs = 0;
    int    visible_glyphs = 0;
    int    compared_same_glyph_pairs = 0;
    int    first_word_gutter = 0;
    int    second_word_gutter = 0;
    int    gutter_delta = 0;
    int    max_visible_left_delta = 0;
    int    max_visible_right_delta = 0;
    int    max_visible_top_delta = 0;
    int    max_visible_baseline_delta = 0;
    double max_weighted_center_x_delta = 0.0;
    double max_weighted_center_y_delta = 0.0;
    bool   all_glyphs_visible = false;
    std::vector<Lcd_repeated_on_glyph_stats> glyphs;
};

struct Lcd_ascii_panel_cell_spec
{
    QString label;
    QString text;
    int     row = 0;
    int     column = 0;
    int     display_width = 1;
    bool    cursor_text = false;
};

struct Lcd_ascii_panel_cell_stats
{
    QString                              label;
    QString                              text;
    int                                  row = 0;
    int                                  column = 0;
    int                                  display_width = 1;
    bool                                 cursor_text = false;
    QRect                                cell_pixels;
    QRect                                ink_bbox;
    int                                  ink_pixels = 0;
    int                                  first_ink_x = -1;
    int                                  relative_first_ink_x = 0;
    int                                  relative_bbox_x = 0;
    int                                  relative_bbox_y = 0;
    int                                  relative_bbox_width = 0;
    int                                  relative_bbox_height = 0;
    double                               relative_bbox_center_x = 0.0;
    double                               relative_bbox_center_y = 0.0;
    Lcd_visible_glyph_metrics            visible;
    int                                  left_half_ink_pixels = 0;
    int                                  right_half_ink_pixels = 0;
    int                                  left_third_ink_pixels = 0;
    int                                  center_third_ink_pixels = 0;
    int                                  right_third_ink_pixels = 0;
    int                                  top_half_ink_pixels = 0;
    int                                  bottom_half_ink_pixels = 0;
    Lcd_horizontal_edge_transition_stats edge_transitions;

    bool has_ink() const { return ink_pixels > 0; }
};

struct Lcd_ascii_panel_probe_stats
{
    int    expected_drawable_glyphs = 0;
    int    probed_cell_count = 0;
    double physical_cell_width = 0.0;
    double physical_cell_height = 0.0;
    double physical_cell_ascent = 0.0;
    double cell_width_integer_delta = 0.0;
    double cell_height_integer_delta = 0.0;
    double cell_ascent_integer_delta = 0.0;
    bool   cell_width_integer = false;
    bool   cell_height_integer = false;
    bool   cell_ascent_integer = false;
    bool   all_probe_cells_have_ink = false;
    bool   all_probe_cells_have_visible_ink = false;
    int    min_ink_pixels = 0;
    int    min_visible_strong_pixels = 0;
    int    key_glyph_transition_count = 0;
    int    max_key_glyph_transition_width = 0;
    double max_key_glyph_average_transition_width = 0.0;
    std::vector<Lcd_ascii_panel_cell_stats> cells;
};

struct Lcd_ascii_panel_placement_delta
{
    QString label;
    bool    reference_has_ink = false;
    bool    candidate_has_ink = false;
    bool    reference_has_visible_ink = false;
    bool    candidate_has_visible_ink = false;
    int     left_delta = 0;
    int     right_delta = 0;
    int     top_delta = 0;
    int     bottom_delta = 0;
    int     first_ink_x_delta = 0;
    int     visible_left_delta = 0;
    int     visible_right_delta = 0;
    int     visible_top_delta = 0;
    int     visible_baseline_delta = 0;
    double  center_x_delta = 0.0;
    double  center_y_delta = 0.0;
    double  visible_weighted_center_x_delta = 0.0;
    double  visible_weighted_center_y_delta = 0.0;
    int     signed_top_delta = 0;
    int     signed_bottom_delta = 0;
    double  signed_center_y_delta = 0.0;
    int     signed_visible_top_delta = 0;
    int     signed_visible_baseline_delta = 0;
    double  signed_visible_weighted_center_y_delta = 0.0;
};

struct Lcd_ascii_panel_placement_stats
{
    int    expected_cells = 0;
    int    compared_cells = 0;
    bool   all_cells_have_ink = false;
    bool   all_cells_have_visible_ink = false;
    int    max_left_delta = 0;
    int    max_right_delta = 0;
    int    max_top_delta = 0;
    int    max_bottom_delta = 0;
    int    max_first_ink_x_delta = 0;
    int    max_visible_left_delta = 0;
    int    max_visible_right_delta = 0;
    int    max_visible_top_delta = 0;
    int    max_visible_baseline_delta = 0;
    double max_center_x_delta = 0.0;
    double max_center_y_delta = 0.0;
    double max_visible_weighted_center_x_delta = 0.0;
    double max_visible_weighted_center_y_delta = 0.0;
    int    min_signed_top_delta = 0;
    int    max_signed_top_delta = 0;
    int    signed_top_delta_range = 0;
    int    min_signed_bottom_delta = 0;
    int    max_signed_bottom_delta = 0;
    int    signed_bottom_delta_range = 0;
    double min_signed_center_y_delta = 0.0;
    double max_signed_center_y_delta = 0.0;
    double signed_center_y_delta_range = 0.0;
    int    min_signed_visible_top_delta = 0;
    int    max_signed_visible_top_delta = 0;
    int    signed_visible_top_delta_range = 0;
    int    min_signed_visible_baseline_delta = 0;
    int    max_signed_visible_baseline_delta = 0;
    int    signed_visible_baseline_delta_range = 0;
    double min_signed_visible_weighted_center_y_delta = 0.0;
    double max_signed_visible_weighted_center_y_delta = 0.0;
    double signed_visible_weighted_center_y_delta_range = 0.0;
    std::vector<Lcd_ascii_panel_placement_delta> cells;
};

struct Lcd_ascii_panel_gutter_pair_stats
{
    QString left_label;
    QString right_label;
    bool    reference_visible = false;
    bool    candidate_visible = false;
    int     reference_gutter = 0;
    int     candidate_gutter = 0;
    int     gutter_delta = 0;
};

struct Lcd_ascii_panel_gutter_stats
{
    int    expected_pairs = 0;
    int    compared_pairs = 0;
    bool   all_pairs_visible = false;
    int    max_gutter_delta = 0;
    std::vector<Lcd_ascii_panel_gutter_pair_stats> pairs;
};

struct Pixel_decorative_primitive
{
    std::string name;
    QRectF      rect;
    QColor      color;
};

struct Pixel_parity_fixture
{
    std::string                         name;
    bool                                layout_parity = false;
    term::Terminal_render_snapshot      snapshot;
    term::terminal_cell_metrics_t       cell_metrics;
    QSizeF                              logical_size;
    qreal                               device_pixel_ratio = 1.0;
    std::vector<Pixel_exact_mask_class>
                                        exact_mask_classes;
    std::vector<Pixel_decorative_primitive>
                                        decorative_primitives;
    std::vector<Pixel_glyph_mask>      glyph_masks;
};

struct Pixel_render_result
{
    QImage                              image;
    term::Qsg_atlas_frame_report        atlas_report;
    qreal                               device_pixel_ratio = 1.0;
    qreal                               image_device_pixel_ratio = 1.0;
    QSize                               window_logical_size;
    QSGRendererInterface::GraphicsApi   graphics_api =
        QSGRendererInterface::Unknown;
    bool                                driver_info_available = false;
    QString                             driver_device_name;
    quint64                             driver_device_id = 0U;
    quint64                             driver_vendor_id = 0U;
    QString                             driver_device_type;
    bool                                software_renderer = false;
    bool                                ready = false;
};

struct Msdf_single_w_probe_result
{
    Pixel_render_result render;
    bool                compiled    = false;
    bool                atlas_built = false;
    QString             message;
    QString             font_source;
    int                 font_data_bytes    = 0;
    int                 pixel_height       = 0;
    int                 atlas_size         = 0;
    float               px_range           = 0.0f;
    float               advance_x          = 0.0f;
    float               baseline_offset_px = 0.0f;
    QRectF              plane_rect;
    QRectF              draw_rect;
};

QRhi* window_rhi(QQuickWindow& window)
{
    QSGRendererInterface* const renderer_interface =
        window.rendererInterface();
    if (renderer_interface == nullptr) {
        return nullptr;
    }

    return static_cast<QRhi*>(
        renderer_interface->getResource(
            &window,
            QSGRendererInterface::RhiResource));
}

// The renderer refuses MSDF text ownership on known software adapters (see
// qsg_atlas_rhi_supports_msdf_text_ownership), so tests that assert on the
// MSDF path must treat such adapters (e.g. the WARP D3D11 device on
// GitHub-hosted Windows runners) as unsupported backends instead of failing.
bool window_uses_known_software_adapter(QQuickWindow& window)
{
    QRhi* const rhi = window_rhi(window);
    if (rhi == nullptr) {
        return false;
    }

    return term::qsg_atlas_driver_info_is_known_software_renderer(
        rhi->driverInfo());
}

void capture_window_driver_info(
    QQuickWindow&         window,
    Pixel_render_result&  result)
{
    QRhi* const rhi = window_rhi(window);
    if (rhi == nullptr) {
        return;
    }

    const QRhiDriverInfo driver_info = rhi->driverInfo();
    result.driver_info_available = true;
    result.driver_device_name    =
        QString::fromUtf8(driver_info.deviceName);
    result.driver_device_id      = driver_info.deviceId;
    result.driver_vendor_id      = driver_info.vendorId;
    result.driver_device_type    =
        driver_device_type_name(driver_info.deviceType);
}

struct Dense_grid_axis_stats
{
    double min_delta = std::numeric_limits<double>::infinity();
    double max_delta = -std::numeric_limits<double>::infinity();
    int    samples   = 0;

    double spread() const
    {
        return samples > 0 ? max_delta - min_delta : 0.0;
    }
};

struct Dense_grid_cell_measurement
{
    double x      = 0.0;
    double y      = 0.0;
    double weight = 0.0;
    int    left   = std::numeric_limits<int>::max();
    int    right  = std::numeric_limits<int>::min();
    int    top    = std::numeric_limits<int>::max();
    int    bottom = std::numeric_limits<int>::min();

    bool has_ink() const { return weight > 0.0; }
};

int pixel_delta(const QColor& left, const QColor& right);
QRect logical_rect_to_pixels(const QRectF& rect, qreal dpr);

qreal pixel_normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return device_pixel_ratio;
}

bool pixel_device_pixel_ratios_match(qreal left, qreal right)
{
    return std::abs(left - right) <= 0.001;
}

qreal pixel_window_device_pixel_ratio(const QQuickWindow& window)
{
    return pixel_normalized_device_pixel_ratio(window.effectiveDevicePixelRatio());
}

qreal pixel_probe_render_window_device_pixel_ratio(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);
    const qreal device_pixel_ratio = pixel_window_device_pixel_ratio(window);
    window.hide();
    app.processEvents(QEventLoop::AllEvents, 50);
    return device_pixel_ratio;
}

term::terminal_cell_metrics_t pixel_metrics(
    qreal device_pixel_ratio,
    qreal font_size,
    QString font_family = QString())
{
    term::Qt_grid_metrics_provider provider(
        term::vnm_terminal_font(std::move(font_family), font_size),
        pixel_normalized_device_pixel_ratio(device_pixel_ratio));
    return provider.cell_metrics();
}

term::terminal_cell_metrics_t pixel_metrics(qreal device_pixel_ratio)
{
    return pixel_metrics(device_pixel_ratio, 18.0);
}

QSizeF pixel_logical_size(
    term::terminal_grid_size_t     grid_size,
    term::terminal_cell_metrics_t  metrics)
{
    return QSizeF(
        metrics.width  * static_cast<qreal>(grid_size.columns),
        metrics.height * static_cast<qreal>(grid_size.rows));
}

QRectF pixel_cell_rect(
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

term::Terminal_render_options pixel_render_options()
{
    return term::Terminal_render_options{};
}

term::Terminal_render_frame pixel_expected_frame(
    const Pixel_parity_fixture& fixture)
{
    return term::build_terminal_render_frame(
        &fixture.snapshot,
        fixture.logical_size,
        fixture.cell_metrics,
        pixel_render_options(),
        true,
        &fixture.snapshot.ime_preedit);
}

void append_qsg_reference_rect(
    QSGNode& parent,
    QRectF   rect,
    QColor   color)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
        return;
    }

    parent.appendChildNode(new QSGSimpleRectNode(rect, color));
}

void append_qsg_reference_rects(
    QSGNode&                                    parent,
    const std::vector<term::Terminal_render_rect>& rects)
{
    for (const term::Terminal_render_rect& rect : rects) {
        append_qsg_reference_rect(parent, rect.rect, rect.color);
    }
}

void append_qsg_reference_decorations(
    QSGNode&                                            parent,
    const std::vector<term::Terminal_render_decoration>& decorations)
{
    for (const term::Terminal_render_decoration& decoration : decorations) {
        append_qsg_reference_rect(parent, decoration.rect, decoration.color);
    }
}

void append_qsg_reference_cursors(
    QSGNode&                                                  parent,
    const std::vector<term::Terminal_render_cursor_primitive>& cursors)
{
    for (const term::Terminal_render_cursor_primitive& cursor : cursors) {
        append_qsg_reference_rect(parent, cursor.rect, cursor.color);
    }
}

bool append_qsg_reference_text_run(
    QSGNode&                              parent,
    QQuickWindow&                         window,
    const term::Terminal_render_text_run& run,
    const QFont&                          font,
    QRectF                                viewport)
{
    if (run.text.isEmpty()) {
        return true;
    }

    QTextLayout layout(run.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(1024.0 * 1024.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
    if (!line.isValid()) {
        return true;
    }

    std::unique_ptr<QSGTextNode> text_node(window.createTextNode());
    if (text_node == nullptr) {
        return false;
    }

    text_node->setColor(run.foreground);
    text_node->setRenderType(QSGTextNode::QtRendering);
    text_node->setViewport(viewport);
    text_node->addTextLayout(
        QPointF(
            run.baseline_origin.x(),
            run.baseline_origin.y() - line.ascent()),
        &layout);
    parent.appendChildNode(text_node.release());
    return true;
}

bool append_qsg_reference_text_runs(
    QSGNode&                                         parent,
    QQuickWindow&                                    window,
    const std::vector<term::Terminal_render_text_run>& runs,
    const QFont&                                     font,
    QRectF                                           viewport)
{
    bool ok = true;
    for (const term::Terminal_render_text_run& run : runs) {
        ok &= append_qsg_reference_text_run(
            parent,
            window,
            run,
            font,
            viewport);
    }
    return ok;
}

class Qsg_text_reference_item final : public QQuickItem
{
public:
    explicit Qsg_text_reference_item(Pixel_parity_fixture fixture)
    :
        m_fixture(std::move(fixture)),
        m_font(term::vnm_terminal_font(QString(), 18.0))
    {
        setFlag(QQuickItem::ItemHasContents, true);
        setSize(m_fixture.logical_size);
    }

    bool text_node_available() const
    {
        return m_text_node_available.load(std::memory_order_acquire);
    }

protected:
    QSGNode* updatePaintNode(
        QSGNode*           old_node,
        UpdatePaintNodeData*) override
    {
        delete old_node;

        std::unique_ptr<QSGNode> root(new QSGNode);
        QQuickWindow* const current_window = window();
        if (current_window == nullptr) {
            return root.release();
        }

        const term::Terminal_render_frame frame = pixel_expected_frame(m_fixture);
        const QRectF viewport(QPointF(0.0, 0.0), frame.logical_size);

        append_qsg_reference_rects(*root, frame.background_rects);
        append_qsg_reference_rects(*root, frame.selection_rects);
        append_qsg_reference_rects(*root, frame.graphic_rects);
        if (!append_qsg_reference_text_runs(
                *root,
                *current_window,
                frame.text_runs,
                m_font,
                viewport))
        {
            m_text_node_available.store(false, std::memory_order_release);
        }
        append_qsg_reference_decorations(*root, frame.decorations);
        append_qsg_reference_cursors(*root, frame.cursors);
        if (!append_qsg_reference_text_runs(
                *root,
                *current_window,
                frame.cursor_text_runs,
                m_font,
                viewport))
        {
            m_text_node_available.store(false, std::memory_order_release);
        }
        append_qsg_reference_rects(*root, frame.overlay_rects);
        return root.release();
    }

private:
    Pixel_parity_fixture m_fixture;
    QFont                m_font;
    std::atomic_bool     m_text_node_available{true};
};

QSize pixel_window_logical_pixel_size(QSizeF logical_size)
{
    return QSize(
        static_cast<int>(std::ceil(logical_size.width())),
        static_cast<int>(std::ceil(logical_size.height())));
}

QSize pixel_window_physical_pixel_size(
    QSizeF logical_size,
    qreal  device_pixel_ratio)
{
    const QSize logical_pixels = pixel_window_logical_pixel_size(logical_size);
    const qreal dpr =
        pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    return QSize(
        std::max(1, static_cast<int>(std::lround(
            static_cast<qreal>(logical_pixels.width()) * dpr))),
        std::max(1, static_cast<int>(std::lround(
            static_cast<qreal>(logical_pixels.height()) * dpr))));
}

qreal pixel_logical_pixel_size(qreal device_pixel_ratio)
{
    return 1.0 / std::max<qreal>(
        1.0,
        pixel_normalized_device_pixel_ratio(device_pixel_ratio));
}

qreal pixel_antialiased_rect_pixel_coverage(
    const term::Terminal_render_rect& rect,
    QPointF                          point)
{
    const QRectF& shape = rect.rect;
    if (shape.width() >= shape.height()) {
        if (point.x() < shape.left() || point.x() > shape.right()) {
            return 0.0;
        }

        const qreal distance = std::abs(point.y() - shape.center().y());
        return std::clamp(
            shape.height() * 0.5 + term::k_terminal_graphic_antialias_feather -
                distance,
            0.0,
            1.0);
    }

    if (point.y() < shape.top() || point.y() > shape.bottom()) {
        return 0.0;
    }

    const qreal distance = std::abs(point.x() - shape.center().x());
    return std::clamp(
        shape.width() * 0.5 + term::k_terminal_graphic_antialias_feather -
            distance,
        0.0,
        1.0);
}

void blend_pixel_reference_pixel(
    QImage& image,
    int     x,
    int     y,
    QColor  color)
{
    if (color.alpha() <= 0 || !image.rect().contains(x, y)) {
        return;
    }

    if (color.alpha() >= 255) {
        color.setAlpha(255);
        image.setPixelColor(x, y, color);
        return;
    }

    const QColor destination = image.pixelColor(x, y);
    const qreal source_alpha_ratio = std::clamp<qreal>(
        color.alphaF(),
        0.0,
        1.0);
    const int source_alpha =
        static_cast<int>(std::lround(source_alpha_ratio * 255.0));
    const int destination_alpha = destination.alpha();
    const int inverse_source_alpha = 255 - source_alpha;
    const int out_alpha =
        source_alpha +
        (destination_alpha * inverse_source_alpha + 127) / 255;
    if (out_alpha <= 0) {
        image.setPixelColor(x, y, QColor(0, 0, 0, 0));
        return;
    }

    const auto blend_component = [&](int source, int destination_component) {
        const int source_premultiplied =
            static_cast<int>(std::lround(
                static_cast<qreal>(source) * source_alpha_ratio));
        const int destination_premultiplied =
            (destination_component * destination_alpha + 127) / 255;
        const int out_premultiplied =
            source_premultiplied +
            (destination_premultiplied * inverse_source_alpha + 127) / 255;
        if (out_alpha >= 255) {
            return std::clamp(out_premultiplied, 0, 255);
        }
        return std::clamp(
            static_cast<int>(std::lround(
                static_cast<qreal>(out_premultiplied) * 255.0 /
                    static_cast<qreal>(out_alpha))),
            0,
            255);
    };

    image.setPixelColor(
        x,
        y,
        QColor(
            blend_component(color.red(), destination.red()),
            blend_component(color.green(), destination.green()),
            blend_component(color.blue(), destination.blue()),
            out_alpha));
}

void paint_pixel_reference_pixel_center_rect(
    QImage& image,
    QRectF  rect,
    QColor  color,
    qreal   device_pixel_ratio)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
        return;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QRect pixels = logical_rect_to_pixels(rect, dpr).intersected(image.rect());
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const QPointF center(
                (static_cast<qreal>(x) + 0.5) / dpr,
                (static_cast<qreal>(y) + 0.5) / dpr);
            if (center.x() < rect.left()  ||
                center.x() >= rect.right() ||
                center.y() < rect.top()   ||
                center.y() >= rect.bottom())
            {
                continue;
            }
            blend_pixel_reference_pixel(image, x, y, color);
        }
    }
}

void paint_pixel_reference_qsg_decoration_rect(
    QImage& image,
    QRectF  rect,
    QColor  color,
    qreal   device_pixel_ratio)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
        return;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QRect pixels = logical_rect_to_pixels(rect, dpr).intersected(image.rect());
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const QPointF sample(
                (static_cast<qreal>(x) - 0.5) / dpr,
                (static_cast<qreal>(y) - 0.5) / dpr);
            if (sample.x() < rect.left()  ||
                sample.x() >= rect.right() ||
                sample.y() < rect.top()   ||
                sample.y() >= rect.bottom())
            {
                continue;
            }
            blend_pixel_reference_pixel(image, x, y, color);
        }
    }
}

void paint_pixel_reference_rect(
    QImage&                           image,
    const term::Terminal_render_rect& rect,
    qreal                             device_pixel_ratio)
{
    if (rect.rect.width() <= 0.0 ||
        rect.rect.height() <= 0.0 ||
        rect.color.alpha() <= 0)
    {
        return;
    }

    if (!rect.antialias) {
        paint_pixel_reference_pixel_center_rect(
            image,
            rect.rect,
            rect.color,
            device_pixel_ratio);
        return;
    }

    const QRectF bounds = rect.rect.adjusted(
        -term::k_terminal_graphic_antialias_feather,
        -term::k_terminal_graphic_antialias_feather,
        term::k_terminal_graphic_antialias_feather,
        term::k_terminal_graphic_antialias_feather);
    const qreal pixel = pixel_logical_pixel_size(device_pixel_ratio);
    const int left   = static_cast<int>(std::floor(bounds.left()   / pixel));
    const int top    = static_cast<int>(std::floor(bounds.top()    / pixel));
    const int right  = static_cast<int>(std::ceil(bounds.right()   / pixel));
    const int bottom = static_cast<int>(std::ceil(bounds.bottom()  / pixel));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const QPointF center(
                (static_cast<qreal>(x) + 0.5) * pixel,
                (static_cast<qreal>(y) + 0.5) * pixel);
            const qreal coverage =
                pixel_antialiased_rect_pixel_coverage(rect, center);
            if (coverage <= 0.0) {
                continue;
            }
            blend_pixel_reference_pixel(
                image,
                x,
                y,
                term::terminal_render_coverage_color(rect.color, coverage));
        }
    }
}

void paint_pixel_reference_rects(
    QImage&                                    image,
    const std::vector<term::Terminal_render_rect>& rects,
    qreal                                      device_pixel_ratio)
{
    for (const term::Terminal_render_rect& rect : rects) {
        paint_pixel_reference_rect(image, rect, device_pixel_ratio);
    }
}

void paint_pixel_reference_decorations(
    QImage&                                            image,
    const std::vector<term::Terminal_render_decoration>& decorations,
    qreal                                              device_pixel_ratio)
{
    for (const term::Terminal_render_decoration& decoration : decorations) {
        paint_pixel_reference_qsg_decoration_rect(
            image,
            decoration.rect,
            decoration.color,
            device_pixel_ratio);
    }
}

void paint_pixel_reference_cursors(
    QImage&                                                  image,
    const std::vector<term::Terminal_render_cursor_primitive>& cursors,
    qreal                                                    device_pixel_ratio)
{
    for (const term::Terminal_render_cursor_primitive& cursor : cursors) {
        paint_pixel_reference_pixel_center_rect(
            image,
            cursor.rect,
            cursor.color,
            device_pixel_ratio);
    }
}

void paint_pixel_reference_arc(
    QImage&                         image,
    const term::Terminal_render_arc& arc,
    qreal                           device_pixel_ratio)
{
    if (arc.rect.width() <= 0.0 ||
        arc.rect.height() <= 0.0 ||
        arc.color.alpha() <= 0 ||
        arc.stroke <= 0.0)
    {
        return;
    }

    const term::terminal_render_arc_geometry_t arc_spec =
        term::terminal_render_arc_geometry(arc);
    const qreal pixel = pixel_logical_pixel_size(device_pixel_ratio);
    const int left   = static_cast<int>(std::floor(arc.rect.left()   / pixel));
    const int top    = static_cast<int>(std::floor(arc.rect.top()    / pixel));
    const int right  = static_cast<int>(std::ceil(arc.rect.right()   / pixel));
    const int bottom = static_cast<int>(std::ceil(arc.rect.bottom()  / pixel));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const QPointF center(
                (static_cast<qreal>(x) + 0.5) * pixel,
                (static_cast<qreal>(y) + 0.5) * pixel);
            const qreal coverage =
                term::terminal_render_arc_pixel_coverage(arc, arc_spec, center);
            if (coverage <= 0.0) {
                continue;
            }
            blend_pixel_reference_pixel(
                image,
                x,
                y,
                term::terminal_render_coverage_color(arc.color, coverage));
        }
    }
}

void paint_pixel_reference_arcs(
    QImage&                                     image,
    const std::vector<term::Terminal_render_arc>& arcs,
    qreal                                      device_pixel_ratio)
{
    for (const term::Terminal_render_arc& arc : arcs) {
        paint_pixel_reference_arc(image, arc, device_pixel_ratio);
    }
}

QImage pixel_tinted_glyph_image(
    const term::Glyph_coverage_tile& tile,
    QColor                           color)
{
    QImage glyph(tile.size, QImage::Format_ARGB32_Premultiplied);
    glyph.fill(Qt::transparent);
    for (int y = 0; y < tile.size.height(); ++y) {
        const uchar* const row =
            reinterpret_cast<const uchar*>(tile.bytes.constData()) +
            static_cast<std::ptrdiff_t>(y * tile.bytes_per_line);
        for (int x = 0; x < tile.size.width(); ++x) {
            const int coverage = row[x];
            if (coverage == 0) {
                continue;
            }
            QColor pixel = color;
            pixel.setAlpha(std::clamp(
                static_cast<int>(
                    std::lround(
                        static_cast<qreal>(color.alpha()) *
                            static_cast<qreal>(coverage) / 255.0)),
                0,
                255));
            glyph.setPixelColor(x, y, pixel);
        }
    }
    return glyph;
}

void paint_pixel_reference_glyph(
    QPainter&                              painter,
    quint32                                glyph_index,
    QPointF                                glyph_origin,
    QRawFont&                              raster_font,
    const term::Terminal_render_text_run&  run,
    qreal                                  device_pixel_ratio)
{
    if (glyph_index == 0U) {
        return;
    }

    const QImage alpha_map = raster_font.alphaMapForGlyph(
        glyph_index,
        QRawFont::PixelAntialiasing);
    const term::Glyph_coverage_tile tile =
        term::qsg_atlas_coverage_tile_from_image(alpha_map);
    if (!tile.is_valid()) {
        return;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QPoint physical_offset =
        term::qsg_atlas_glyph_physical_offset_for_raster_font(
            raster_font,
            glyph_index,
            term::Glyph_image_presentation::UNKNOWN);
    const QRectF glyph_rect =
        term::qsg_atlas_snapped_glyph_draw_rect(
            glyph_origin,
            physical_offset,
            tile.size,
            dpr);
    if (run.clip_rect.isValid() && !glyph_rect.intersects(run.clip_rect)) {
        return;
    }

    painter.save();
    if (run.clip_rect.isValid()) {
        painter.setClipRect(run.clip_rect);
    }
    painter.drawImage(glyph_rect, pixel_tinted_glyph_image(tile, run.foreground));
    painter.restore();
}

void paint_pixel_reference_layout_text_run(
    QPainter&                              painter,
    const term::Terminal_render_text_run&  run,
    const QFont&                           font,
    term::terminal_cell_metrics_t          cell_metrics,
    qreal                                  device_pixel_ratio)
{
    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(
            run,
            font,
            cell_metrics,
            device_pixel_ratio);
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(record.physical_pixel_size);
        paint_pixel_reference_glyph(
            painter,
            record.glyph_index,
            record.glyph_origin,
            raster_font,
            run,
            device_pixel_ratio);
    }
}

void paint_pixel_reference_text_runs(
    QPainter&                                      painter,
    const std::vector<term::Terminal_render_text_run>& runs,
    const QFont&                                   font,
    term::terminal_cell_metrics_t                  cell_metrics,
    qreal                                          device_pixel_ratio)
{
    for (const term::Terminal_render_text_run& run : runs) {
        if (run.text.isEmpty()) {
            continue;
        }
        paint_pixel_reference_layout_text_run(
            painter,
            run,
            font,
            cell_metrics,
            device_pixel_ratio);
    }
}

void append_exact_class(
    Pixel_parity_fixture& fixture,
    std::string            name,
    std::vector<QRectF>    masks,
    int                    max_delta = 0)
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
        max_delta,
    });
}

void append_exact_fill_class(
    Pixel_parity_fixture& fixture,
    std::string            name,
    std::vector<QRectF>    masks)
{
    append_exact_class(
        fixture,
        std::move(name),
        std::move(masks),
        1);
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

void append_decorative_primitives(
    Pixel_parity_fixture&                  fixture,
    std::string                             name,
    const term::Terminal_render_frame&      frame,
    term::Terminal_render_decoration_kind   kind)
{
    for (const term::Terminal_render_decoration& decoration : frame.decorations) {
        if (decoration.kind != kind) {
            continue;
        }

        fixture.decorative_primitives.push_back({
            name,
            decoration.rect,
            decoration.color,
        });
    }
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
    Pixel_parity_fixture&              fixture,
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
            run.foreground,
        });
    }
}

term::Terminal_color_state pixel_color_state()
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

term::Terminal_render_cell make_pixel_cell(
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

term::Terminal_render_cell make_pixel_continuation_cell(
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

void set_pixel_visible_line_provenance(
    term::Terminal_render_snapshot& snapshot,
    std::uint64_t                   retained_line_id_base = 1U,
    std::uint64_t                   content_generation = 1U)
{
    snapshot.visible_line_provenance.clear();
    snapshot.visible_line_provenance.reserve(
        static_cast<std::size_t>(std::max(0, snapshot.grid_size.rows)));
    const int first_logical_row =
        static_cast<int>(term::render_snapshot_first_visible_logical_row(snapshot));
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        snapshot.visible_line_provenance.push_back({
            first_logical_row + row,
            retained_line_id_base + static_cast<std::uint64_t>(row),
            content_generation,
        });
    }
}

term::Terminal_render_snapshot make_pixel_base_snapshot(
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
    snapshot.color_state      = pixel_color_state();
    snapshot.dirty_row_ranges = {{0, grid_size.rows}};
    snapshot.cursor.position  = {0, 0};
    snapshot.cursor.visible   = false;
    set_pixel_visible_line_provenance(snapshot);
    return snapshot;
}

Pixel_parity_fixture make_pixel_parity_base_fixture(
    std::string                   name,
    bool                          layout_parity,
    term::terminal_grid_size_t    grid_size,
    std::uint64_t                 sequence,
    term::terminal_cell_metrics_t metrics,
    qreal                         device_pixel_ratio)
{
    Pixel_parity_fixture fixture;
    fixture.name               = std::move(name);
    fixture.layout_parity      = layout_parity;
    fixture.snapshot           = make_pixel_base_snapshot(grid_size, sequence);
    fixture.cell_metrics       = metrics;
    fixture.logical_size       = pixel_logical_size(grid_size, metrics);
    fixture.device_pixel_ratio =
        pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    return fixture;
}

std::vector<Pixel_parity_fixture> make_pixel_parity_fixtures(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture fills = make_pixel_parity_base_fixture(
        "fills_decorations_cursor",
        false,
        {4, 12},
        710U,
        metrics,
        dpr);
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
        make_pixel_cell(1, 1, QStringLiteral(" "), 1, red_background));
    fills.snapshot.cells.push_back(
        make_pixel_cell(2, 2, QStringLiteral(" "), 1, underline));
    fills.snapshot.cells.push_back(
        make_pixel_cell(2, 4, QStringLiteral(" "), 1, strike));
    const term::Terminal_render_frame fills_frame = pixel_expected_frame(fills);
    append_exact_fill_class(
        fills,
        "selection_fill",
        {inset_rect(pixel_cell_rect(0, 1, 4, metrics), 2.0, 2.0)});
    append_exact_class(
        fills,
        "style_background_fill",
        {inset_rect(pixel_cell_rect(1, 1, 1, metrics), 2.0, 2.0)});
    append_exact_fill_class(
        fills,
        "preedit_background_fill",
        {inset_rect(pixel_cell_rect(1, 6, 1, metrics), 2.0, 2.0)});
    append_decorative_primitives(
        fills,
        "underline_decoration",
        fills_frame,
        term::Terminal_render_decoration_kind::UNDERLINE);
    append_decorative_primitives(
        fills,
        "strike_decoration",
        fills_frame,
        term::Terminal_render_decoration_kind::STRIKE);
    append_decorative_primitives(
        fills,
        "preedit_caret",
        fills_frame,
        term::Terminal_render_decoration_kind::PREEDIT_CARET);

    Pixel_parity_fixture glyphs = make_pixel_parity_base_fixture(
        "ascii_bmp_attributes",
        false,
        {3, 14},
        711U,
        metrics,
        dpr);
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
        make_pixel_cell(0, 1, QStringLiteral("Atlas"), 5, normal));
    glyphs.snapshot.cells.push_back(
        make_pixel_cell(1, 1, QString::fromUtf8("\xce\xa9"), 1, normal));
    glyphs.snapshot.cells.push_back(
        make_pixel_cell(1, 3, QString::fromUtf8("\xe7\x95\x8c"), 2, normal));
    glyphs.snapshot.cells.push_back(make_pixel_continuation_cell(1, 4, normal));
    glyphs.snapshot.cells.push_back(
        make_pixel_cell(2, 1, QStringLiteral("F"), 1, faint));
    glyphs.snapshot.cells.push_back(
        make_pixel_cell(2, 3, QStringLiteral("I"), 1, inverse));
    glyphs.snapshot.cells.push_back(
        make_pixel_cell(2, 5, QStringLiteral("X"), 1, invisible));
    append_text_glyph_masks(
        glyphs,
        pixel_expected_frame(glyphs).text_runs);

    Pixel_parity_fixture cursor = make_pixel_parity_base_fixture(
        "block_cursor_text",
        false,
        {1, 10},
        712U,
        metrics,
        dpr);
    cursor.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    cursor.snapshot.cursor.position      = {0, 2};
    cursor.snapshot.cursor.visible       = true;
    cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    cursor.snapshot.cursor.blink_enabled = false;
    cursor.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QStringLiteral("."), 1, normal));
    const term::Terminal_render_frame cursor_frame = pixel_expected_frame(cursor);
    append_exact_class(
        cursor,
        "block_cursor_fill_outside_glyph",
        cursor_text_fill_masks(pixel_cell_rect(0, 2, 1, metrics)));
    append_text_glyph_masks(
        cursor,
        cursor_frame.cursor_text_runs,
        pixel_render_options().cursor_color);

    Pixel_parity_fixture empty_cursor = make_pixel_parity_base_fixture(
        "block_cursor_empty_fill",
        false,
        {1, 8},
        713U,
        metrics,
        dpr);
    empty_cursor.snapshot.cursor.position      = {0, 3};
    empty_cursor.snapshot.cursor.visible       = true;
    empty_cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    empty_cursor.snapshot.cursor.blink_enabled = false;
    append_exact_class(
        empty_cursor,
        "empty_block_cursor_fill",
        {inset_rect(pixel_cell_rect(0, 3, 1, metrics), 2.0, 2.0)});

    Pixel_parity_fixture graphic_cursor = make_pixel_parity_base_fixture(
        "block_cursor_block_element_text",
        false,
        {1, 8},
        714U,
        metrics,
        dpr);
    graphic_cursor.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    graphic_cursor.snapshot.cursor.position      = {0, 3};
    graphic_cursor.snapshot.cursor.visible       = true;
    graphic_cursor.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    graphic_cursor.snapshot.cursor.blink_enabled = false;
    graphic_cursor.snapshot.cells.push_back(
        make_pixel_cell(0, 3, QString::fromUtf8("\xe2\x96\x8c"), 1, normal));
    const term::Terminal_render_frame graphic_frame =
        pixel_expected_frame(graphic_cursor);
    append_exact_class(
        graphic_cursor,
        "block_element_cursor_fill_outside_glyph",
        cursor_text_fill_masks(pixel_cell_rect(0, 3, 1, metrics)));
    append_text_glyph_masks(
        graphic_cursor,
        graphic_frame.cursor_text_runs,
        pixel_render_options().cursor_color);

    return {
        std::move(fills),
        std::move(glyphs),
        std::move(cursor),
        std::move(empty_cursor),
        std::move(graphic_cursor),
    };
}

Pixel_parity_fixture make_layout_parity_base_fixture(
    std::string                  name,
    term::terminal_grid_size_t   grid_size,
    std::uint64_t                sequence,
    term::terminal_cell_metrics_t metrics,
    qreal                         device_pixel_ratio)
{
    return make_pixel_parity_base_fixture(
        std::move(name),
        true,
        grid_size,
        sequence,
        metrics,
        device_pixel_ratio);
}

std::vector<Pixel_parity_fixture> make_layout_parity_fixtures(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture blocks = make_layout_parity_base_fixture(
        "graphics_supported_unsupported_blocks",
        {2, 12},
        730U,
        metrics,
        dpr);
    const term::Terminal_style_id block_accent = 1U;
    const term::Terminal_style_id shade_accent = 2U;
    blocks.snapshot.styles.push_back(rgb_style(0xff55e6c1U, 0xff101820U));
    blocks.snapshot.styles.push_back(rgb_style(0xffffd166U, 0xff101820U));
    blocks.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("\u2588"), 1, block_accent));
    blocks.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QStringLiteral("\u258c"), 1, block_accent));
    blocks.snapshot.cells.push_back(
        make_pixel_cell(0, 4, QStringLiteral("\u2599"), 1, block_accent));
    blocks.snapshot.cells.push_back(
        make_pixel_cell(1, 0, QStringLiteral("\u2591"), 1, shade_accent));
    const term::Terminal_render_frame block_frame = pixel_expected_frame(blocks);
    append_text_glyph_masks(blocks, block_frame.text_runs);

    Pixel_parity_fixture arcs = make_layout_parity_base_fixture(
        "box_arcs",
        {1, 8},
        731U,
        metrics,
        dpr);
    arcs.snapshot.styles.push_back(rgb_style(0xff8be9fdU, 0xff101820U));
    arcs.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("\u256d"), 1, block_accent));
    arcs.snapshot.cells.push_back(
        make_pixel_cell(0, 1, QStringLiteral("\u256e"), 1, block_accent));
    arcs.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QStringLiteral("\u256f"), 1, block_accent));
    arcs.snapshot.cells.push_back(
        make_pixel_cell(0, 3, QStringLiteral("\u2570"), 1, block_accent));
    append_text_glyph_masks(
        arcs,
        pixel_expected_frame(arcs).text_runs);

    Pixel_parity_fixture cursor_box = make_layout_parity_base_fixture(
        "block_cursor_antialiased_box",
        {1, 8},
        737U,
        metrics,
        dpr);
    cursor_box.snapshot.styles.push_back(rgb_style(0xff8be9fdU, 0xff101820U));
    cursor_box.snapshot.cursor.position      = {0, 2};
    cursor_box.snapshot.cursor.visible       = true;
    cursor_box.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    cursor_box.snapshot.cursor.blink_enabled = false;
    cursor_box.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QStringLiteral("\u2500"), 1, block_accent));
    const term::Terminal_render_frame cursor_box_frame =
        pixel_expected_frame(cursor_box);
    append_exact_class(
        cursor_box,
        "box_cursor_fill_outside_glyph",
        cursor_text_fill_masks(pixel_cell_rect(0, 2, 1, metrics)));
    append_text_glyph_masks(
        cursor_box,
        cursor_box_frame.cursor_text_runs,
        pixel_render_options().cursor_color);

    Pixel_parity_fixture cursor_vertical_box = make_layout_parity_base_fixture(
        "block_cursor_antialiased_vertical_box",
        {1, 8},
        739U,
        metrics,
        dpr);
    cursor_vertical_box.snapshot.styles.push_back(rgb_style(0xff8be9fdU, 0xff101820U));
    cursor_vertical_box.snapshot.cursor.position      = {0, 2};
    cursor_vertical_box.snapshot.cursor.visible       = true;
    cursor_vertical_box.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    cursor_vertical_box.snapshot.cursor.blink_enabled = false;
    cursor_vertical_box.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QStringLiteral("\u2502"), 1, block_accent));
    const term::Terminal_render_frame cursor_vertical_box_frame =
        pixel_expected_frame(cursor_vertical_box);
    append_exact_class(
        cursor_vertical_box,
        "vertical_box_cursor_fill_outside_glyph",
        cursor_text_fill_masks(pixel_cell_rect(0, 2, 1, metrics)));
    append_text_glyph_masks(
        cursor_vertical_box,
        cursor_vertical_box_frame.cursor_text_runs,
        pixel_render_options().cursor_color);

    Pixel_parity_fixture scrollback = make_layout_parity_base_fixture(
        "viewport_scrollback_selection",
        {3, 8},
        732U,
        metrics,
        dpr);
    scrollback.snapshot.viewport.follow_tail      = false;
    scrollback.snapshot.viewport.scrollback_rows  = 8;
    scrollback.snapshot.viewport.offset_from_tail = 2;
    set_pixel_visible_line_provenance(scrollback.snapshot, 900U, 77U);
    scrollback.snapshot.selection_spans.push_back({
        {{6, 4}, {6, 7}, term::Terminal_selection_mode::NORMAL},
        0,
        4,
        3,
    });
    scrollback.snapshot.cells.push_back(
        make_pixel_cell(0, 1, QStringLiteral("row"), 3, term::k_default_terminal_style_id));
    append_exact_fill_class(
        scrollback,
        "scrollback_selection",
        {inset_rect(pixel_cell_rect(0, 4, 3, metrics), 4.0, 2.0)});
    append_text_glyph_masks(
        scrollback,
        pixel_expected_frame(scrollback).text_runs);

    Pixel_parity_fixture alternate = make_layout_parity_base_fixture(
        "alternate_viewport",
        {2, 8},
        733U,
        metrics,
        dpr);
    alternate.snapshot.viewport.active_buffer    = term::Terminal_buffer_id::ALTERNATE;
    alternate.snapshot.viewport.follow_tail      = true;
    alternate.snapshot.viewport.scrollback_rows  = 0;
    alternate.snapshot.viewport.offset_from_tail = 0;
    alternate.snapshot.styles.push_back(rgb_style(0xff55e6c1U, 0xff101820U));
    set_pixel_visible_line_provenance(alternate.snapshot, 950U, 5U);
    alternate.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("\u2588"), 1, block_accent));
    append_text_glyph_masks(
        alternate,
        pixel_expected_frame(alternate).text_runs);

    Pixel_parity_fixture public_scroll = make_layout_parity_base_fixture(
        "public_projection_scroll_full_repaint",
        {2, 6},
        734U,
        metrics,
        dpr);
    public_scroll.snapshot.basis =
        term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    public_scroll.snapshot.purpose =
        term::Terminal_render_snapshot_purpose::SCROLL;
    append_exact_class(
        public_scroll,
        "full_repaint_background",
        {inset_rect(QRectF(QPointF(0.0, 0.0), public_scroll.logical_size), 2.0, 2.0)});

    Pixel_parity_fixture suppressed = make_layout_parity_base_fixture(
        "selection_provenance_suppressed",
        {2, 8},
        735U,
        metrics,
        dpr);
    suppressed.snapshot.visible_line_provenance.clear();
    suppressed.snapshot.selection_spans.push_back({
        {{0, 1}, {0, 5}, term::Terminal_selection_mode::NORMAL},
        0,
        1,
        4,
    });
    term::suppress_selection_spans_without_valid_line_provenance(suppressed.snapshot);
    append_exact_class(
        suppressed,
        "suppressed_selection_background",
        {inset_rect(QRectF(QPointF(0.0, 0.0), suppressed.logical_size), 2.0, 2.0)});

    Pixel_parity_fixture fallback = make_layout_parity_base_fixture(
        "fallback_fonts",
        {2, 10},
        736U,
        metrics,
        dpr);
    fallback.snapshot.styles.push_back(rgb_style(0xfff78c6cU, 0xff101820U));
    fallback.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, block_accent));
    fallback.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QString::fromUtf8("\xe7\x95\x8c"), 2, block_accent));
    fallback.snapshot.cells.push_back(make_pixel_continuation_cell(0, 3, block_accent));
    fallback.snapshot.cells.push_back(
        make_pixel_cell(1, 0, QStringLiteral("B"), 1, block_accent));
    append_text_glyph_masks(
        fallback,
        pixel_expected_frame(fallback).text_runs);

    Pixel_parity_fixture emoji = make_layout_parity_base_fixture(
        "emoji_policy",
        {1, 8},
        738U,
        metrics,
        dpr);
    emoji.snapshot.styles.push_back(rgb_style(0xfff78c6cU, 0xff101820U));
    emoji.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("E"), 1, block_accent));
    emoji.snapshot.cells.push_back(
        make_pixel_cell(0, 2, QString::fromUcs4(U"\U0001F600"), 2, block_accent));
    emoji.snapshot.cells.push_back(make_pixel_continuation_cell(0, 3, block_accent));
    append_text_glyph_masks(
        emoji,
        pixel_expected_frame(emoji).text_runs);

    return {
        std::move(blocks),
        std::move(arcs),
        std::move(cursor_box),
        std::move(cursor_vertical_box),
        std::move(scrollback),
        std::move(alternate),
        std::move(public_scroll),
        std::move(suppressed),
        std::move(fallback),
        std::move(emoji),
    };
}

Pixel_aa_budget pixel_budget_for_backend(
    const char*         backend,
    const std::string&  fixture_name)
{
    if (fixture_name == "graphics_supported_unsupported_blocks") {
        if (std::strcmp(backend, "opengl") == 0) {
            return {300, 216, 276, 0.90};
        }
        return {300, 215, 276, 0.90};
    }

    if (fixture_name == "box_arcs") {
        return {1152, 255, 260, 0.0};
    }

    if (fixture_name == "block_cursor_antialiased_box") {
        if (std::strcmp(backend, "opengl") == 0) {
            return {288, 130, 60, 0.0};
        }
        return {288, 120, 60, 0.0};
    }

    if (fixture_name == "block_cursor_antialiased_vertical_box") {
        return {288, 200, 75, 0.0};
    }

    if (fixture_name == "block_cursor_block_element_text") {
        return {288, 200, 75, 0.0};
    }

    if (fixture_name == "viewport_scrollback_selection") {
        return {816, 210, 300, 0.35};
    }

    if (fixture_name == "alternate_viewport") {
        return {288, 220, 260, 0.0};
    }

    if (fixture_name == "fallback_fonts") {
        return {1140, 235, 650, 0.40};
    }

    if (fixture_name == "emoji_policy") {
        return {840, 245, 560, 0.75};
    }

    if (fixture_name == "ascii_bmp_attributes") {
        if (std::strcmp(backend, "d3d11") == 0) {
            return {3132, 230, 1350, 0.45};
        }
        if (std::strcmp(backend, "d3d12") == 0) {
            return {3132, 230, 1350, 0.45};
        }
        if (std::strcmp(backend, "vulkan") == 0) {
            return {3132, 230, 1350, 0.45};
        }
        if (std::strcmp(backend, "opengl") == 0) {
            return {3132, 230, 1350, 0.45};
        }
        return {3132, 230, 1350, 0.45};
    }

    if (fixture_name == "block_cursor_text") {
        // MSDF cursor text clips the glyph through the cursor cell. QRhi
        // backends differ from the CPU reference by a few edge pixels while
        // preserving shape and placement.
        if (std::strcmp(backend, "d3d11") == 0) {
            return {288, 230, 120, 0.08};
        }
        if (std::strcmp(backend, "d3d12") == 0) {
            return {288, 230, 120, 0.08};
        }
        if (std::strcmp(backend, "vulkan") == 0) {
            return {288, 230, 120, 0.08};
        }
        if (std::strcmp(backend, "opengl") == 0) {
            return {288, 230, 120, 0.08};
        }
        return {288, 230, 120, 0.08};
    }

    return {};
}

Pixel_aa_budget rgba_reference_budget_for_coverage(
    const term::Glyph_coverage_counts& coverage)
{
    const bool has_lcd =
        coverage.lcd_rgb_masks > 0 ||
        coverage.lcd_bgr_masks > 0;
    const bool has_color = coverage.color_images > 0;

    // The RGBA reference is the same atlas-tile compositing model rendered on
    // the CPU. A few LCD fringe edge pixels can differ sharply after QRhi
    // readback, so visible divergence is bounded by the scaled diff-pixel
    // budget while the max-delta rail stays high enough for isolated edges.
    Pixel_aa_budget budget;
    budget.base_compared_pixels = 1000;
    budget.diff_pixels          = has_lcd || has_color ? 80 : 60;
    budget.max_delta            = 230;
    if (has_lcd) {
        budget.max_delta = 240;
    }
    if (has_color) {
        budget.max_delta = 245;
    }
    budget.ink_delta_perimeter_factor =
        has_lcd || has_color ? 0.35 : 0.25;
    return budget;
}

bool glyph_coverage_has_lcd(const term::Glyph_coverage_counts& coverage)
{
    return
        coverage.lcd_rgb_masks > 0 ||
        coverage.lcd_bgr_masks > 0;
}

int scaled_pixel_budget_count(
    int                     base_count,
    const Pixel_aa_budget& budget,
    int                     compared_pixels)
{
    if (base_count <= 0 ||
        budget.base_compared_pixels <= 0 ||
        compared_pixels <= 0)
    {
        return base_count;
    }

    const double scale =
        static_cast<double>(compared_pixels) /
        static_cast<double>(budget.base_compared_pixels);
    return static_cast<int>(
        std::ceil(static_cast<double>(base_count) * scale));
}

int glyph_mask_physical_perimeter_pixels(
    const Pixel_glyph_mask& mask,
    qreal                    device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QRect pixels = logical_rect_to_pixels(mask.rect, dpr);
    if (pixels.width() <= 0 || pixels.height() <= 0) {
        return 0;
    }
    return 2 * (pixels.width() + pixels.height());
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

double lcd_visible_pixel_energy(
    const QColor& pixel,
    QColor        background,
    QColor        foreground)
{
    const int foreground_delta = pixel_delta(foreground, background);
    if (foreground_delta <= 0) {
        return 0.0;
    }

    return std::clamp(
        static_cast<double>(pixel_delta(pixel, background)) /
            static_cast<double>(foreground_delta),
        0.0,
        1.0);
}

Lcd_visible_glyph_metrics measure_lcd_visible_glyph_metrics_pixels(
    const QImage& image,
    QRect         region,
    QColor        background,
    QColor        foreground)
{
    constexpr int k_visible_noise_delta_threshold = 8;

    Lcd_visible_glyph_metrics metrics;
    region = region.intersected(image.rect());
    if (image.isNull() || region.isEmpty()) {
        return metrics;
    }

    metrics.row_energy.assign(
        static_cast<std::size_t>(region.height()),
        0.0);
    metrics.column_energy.assign(
        static_cast<std::size_t>(region.width()),
        0.0);

    int left   = std::numeric_limits<int>::max();
    int top    = std::numeric_limits<int>::max();
    int right  = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    double weighted_x_sum = 0.0;
    double weighted_y_sum = 0.0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel_delta(pixel, background) <=
                k_visible_noise_delta_threshold)
            {
                continue;
            }

            const double energy =
                lcd_visible_pixel_energy(pixel, background, foreground);
            if (energy <= 0.0) {
                continue;
            }

            metrics.energy_sum += energy;
            weighted_x_sum += (static_cast<double>(x) + 0.5) * energy;
            weighted_y_sum += (static_cast<double>(y) + 0.5) * energy;
            metrics.row_energy[
                static_cast<std::size_t>(y - region.top())] += energy;
            metrics.column_energy[
                static_cast<std::size_t>(x - region.left())] += energy;

            if (energy < k_lcd_visible_strong_energy_threshold) {
                continue;
            }

            left   = std::min(left, x);
            top    = std::min(top, y);
            right  = std::max(right, x);
            bottom = std::max(bottom, y);
            ++metrics.strong_pixels;
        }
    }

    if (metrics.energy_sum > 0.0) {
        metrics.weighted_center_x = weighted_x_sum / metrics.energy_sum;
        metrics.weighted_center_y = weighted_y_sum / metrics.energy_sum;
        metrics.relative_weighted_center_x =
            metrics.weighted_center_x - static_cast<double>(region.left());
        metrics.relative_weighted_center_y =
            metrics.weighted_center_y - static_cast<double>(region.top());
    }

    if (metrics.strong_pixels <= 0) {
        return metrics;
    }

    metrics.bbox = QRect(QPoint(left, top), QPoint(right, bottom));
    metrics.visible_left_x = left;
    metrics.visible_right_x = right;
    metrics.visible_top_y = top;
    metrics.visible_baseline_y = bottom;
    metrics.relative_visible_left_x = left - region.left();
    metrics.relative_visible_right_x = right - region.left();
    metrics.relative_visible_top_y = top - region.top();
    metrics.relative_visible_baseline_y = bottom - region.top();
    return metrics;
}

Pixel_image_ink_stats measure_image_ink(
    const QImage& image,
    QColor        background,
    QRectF        logical_region,
    qreal         device_pixel_ratio)
{
    constexpr int k_ink_delta_threshold = 8;

    Pixel_image_ink_stats stats;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QRect region = logical_rect_to_pixels(logical_region, dpr)
        .intersected(image.rect());

    int left   = std::numeric_limits<int>::max();
    int top    = std::numeric_limits<int>::max();
    int right  = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    double x_sum = 0.0;
    double y_sum = 0.0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (pixel_delta(image.pixelColor(x, y), background) <=
                k_ink_delta_threshold)
            {
                continue;
            }

            left   = std::min(left, x);
            top    = std::min(top, y);
            right  = std::max(right, x);
            bottom = std::max(bottom, y);
            x_sum += static_cast<double>(x) + 0.5;
            y_sum += static_cast<double>(y) + 0.5;
            ++stats.ink_pixels;
        }
    }

    if (stats.ink_pixels <= 0) {
        return stats;
    }

    stats.bbox = QRect(QPoint(left, top), QPoint(right, bottom));
    stats.center_x = x_sum / static_cast<double>(stats.ink_pixels);
    stats.center_y = y_sum / static_cast<double>(stats.ink_pixels);
    return stats;
}

Lcd_first_ink_column_stats measure_lcd_first_ink_column(
    const QImage&                         image,
    const std::vector<Pixel_glyph_mask>&  glyph_masks,
    qreal                                 device_pixel_ratio)
{
    constexpr int k_ink_delta_threshold = 8;

    Lcd_first_ink_column_stats stats;
    if (image.isNull() || glyph_masks.empty()) {
        return stats;
    }

    struct Pixel_region
    {
        QRect  rect;
        QColor background;
    };

    std::vector<Pixel_region> regions;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    for (const Pixel_glyph_mask& mask : glyph_masks) {
        const QRect region = logical_rect_to_pixels(mask.rect, dpr)
            .intersected(image.rect());
        if (!region.isEmpty()) {
            regions.push_back({region, mask.background});
        }
    }
    if (regions.empty()) {
        return stats;
    }

    auto is_ink = [&](int x, int y) {
        for (const Pixel_region& region : regions) {
            if (region.rect.contains(x, y) &&
                pixel_delta(image.pixelColor(x, y), region.background) >
                    k_ink_delta_threshold)
            {
                return true;
            }
        }
        return false;
    };

    int left   = std::numeric_limits<int>::max();
    int top    = std::numeric_limits<int>::max();
    int right  = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    for (const Pixel_region& region : regions) {
        for (int y = region.rect.top(); y <= region.rect.bottom(); ++y) {
            for (int x = region.rect.left(); x <= region.rect.right(); ++x) {
                if (!is_ink(x, y)) {
                    continue;
                }

                left   = std::min(left, x);
                top    = std::min(top, y);
                right  = std::max(right, x);
                bottom = std::max(bottom, y);
                ++stats.ink_pixels;
            }
        }
    }
    if (stats.ink_pixels <= 0) {
        return stats;
    }

    stats.bbox = QRect(QPoint(left, top), QPoint(right, bottom));
    stats.first_ink_x = left;

    std::set<QRgb> distinct_first_column_rgb_colors;
    QRgb previous_rgb = 0U;
    bool previous_was_ink = false;
    int current_equal_run = 0;
    for (int y = top; y <= bottom; ++y) {
        if (!is_ink(left, y)) {
            current_equal_run = 0;
            previous_was_ink = false;
            continue;
        }

        const QColor color = image.pixelColor(left, y);
        const QRgb rgb = qRgb(color.red(), color.green(), color.blue());
        distinct_first_column_rgb_colors.insert(rgb);
        stats.first_column.push_back({y, color});
        ++stats.first_column_rows;

        if (!previous_was_ink || rgb != previous_rgb) {
            current_equal_run = 1;
        }
        else {
            ++stats.adjacent_equal_first_column_pairs;
            ++current_equal_run;
        }
        stats.longest_equal_first_column_run = std::max(
            stats.longest_equal_first_column_run,
            current_equal_run);
        previous_rgb = rgb;
        previous_was_ink = true;
    }

    stats.distinct_first_column_rgb_colors =
        static_cast<int>(distinct_first_column_rgb_colors.size());
    return stats;
}

Lcd_first_ink_column_stats measure_lcd_first_ink_column_pixels(
    const QImage& image,
    QRect         region,
    QColor        background)
{
    constexpr int k_ink_delta_threshold = 8;

    Lcd_first_ink_column_stats stats;
    region = region.intersected(image.rect());
    if (image.isNull() || region.isEmpty()) {
        return stats;
    }

    auto is_ink = [&](int x, int y) {
        return pixel_delta(image.pixelColor(x, y), background) >
            k_ink_delta_threshold;
    };

    int left   = std::numeric_limits<int>::max();
    int top    = std::numeric_limits<int>::max();
    int right  = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (!is_ink(x, y)) {
                continue;
            }

            left   = std::min(left, x);
            top    = std::min(top, y);
            right  = std::max(right, x);
            bottom = std::max(bottom, y);
            ++stats.ink_pixels;
        }
    }
    if (stats.ink_pixels <= 0) {
        return stats;
    }

    stats.bbox = QRect(QPoint(left, top), QPoint(right, bottom));
    stats.first_ink_x = left;

    std::set<QRgb> distinct_first_column_rgb_colors;
    QRgb previous_rgb = 0U;
    bool previous_was_ink = false;
    int current_equal_run = 0;
    for (int y = top; y <= bottom; ++y) {
        if (!is_ink(left, y)) {
            current_equal_run = 0;
            previous_was_ink = false;
            continue;
        }

        const QColor color = image.pixelColor(left, y);
        const QRgb rgb = qRgb(color.red(), color.green(), color.blue());
        distinct_first_column_rgb_colors.insert(rgb);
        stats.first_column.push_back({y, color});
        ++stats.first_column_rows;

        if (!previous_was_ink || rgb != previous_rgb) {
            current_equal_run = 1;
        }
        else {
            ++stats.adjacent_equal_first_column_pairs;
            ++current_equal_run;
        }
        stats.longest_equal_first_column_run = std::max(
            stats.longest_equal_first_column_run,
            current_equal_run);
        previous_rgb = rgb;
        previous_was_ink = true;
    }

    stats.distinct_first_column_rgb_colors =
        static_cast<int>(distinct_first_column_rgb_colors.size());
    return stats;
}

int lcd_edge_low_delta_threshold(const Pixel_glyph_mask& mask)
{
    const int foreground_delta = mask.foreground.isValid()
        ? pixel_delta(mask.foreground, mask.background)
        : 0;
    if (foreground_delta <= 0) {
        return 8;
    }
    return std::max(8, static_cast<int>(std::lround(
        static_cast<double>(foreground_delta) * 0.08)));
}

int lcd_edge_strong_delta_threshold(const Pixel_glyph_mask& mask)
{
    const int foreground_delta = mask.foreground.isValid()
        ? pixel_delta(mask.foreground, mask.background)
        : 0;
    if (foreground_delta <= 0) {
        return 96;
    }
    const int low_threshold = lcd_edge_low_delta_threshold(mask);
    return std::max(low_threshold + 1, static_cast<int>(std::lround(
        static_cast<double>(foreground_delta) * 0.65)));
}

Lcd_horizontal_edge_transition_stats measure_lcd_horizontal_edge_transitions(
    const QImage&                         image,
    const std::vector<Pixel_glyph_mask>&  glyph_masks,
    qreal                                 device_pixel_ratio,
    const Lcd_first_ink_column_stats&     ink_stats)
{
    Lcd_horizontal_edge_transition_stats stats;
    if (image.isNull() || glyph_masks.empty() || !ink_stats.has_ink()) {
        return stats;
    }

    stats.bbox = ink_stats.bbox.intersected(image.rect());
    if (stats.bbox.isEmpty()) {
        return stats;
    }

    struct Pixel_region
    {
        QRect  rect;
        QColor background;
        int    low_delta = 0;
        int    strong_delta = 0;
    };

    std::vector<Pixel_region> regions;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    for (const Pixel_glyph_mask& mask : glyph_masks) {
        if (!mask.require_ink) {
            continue;
        }
        const QRect region = logical_rect_to_pixels(mask.rect, dpr)
            .intersected(stats.bbox)
            .intersected(image.rect());
        if (!region.isEmpty()) {
            regions.push_back({
                region,
                mask.background,
                lcd_edge_low_delta_threshold(mask),
                lcd_edge_strong_delta_threshold(mask),
            });
        }
    }
    if (regions.empty()) {
        return stats;
    }

    int transition_width_sum = 0;
    auto record_transition_width = [&](int width) {
        const int clamped_width = std::max(0, width);
        stats.max_transition_width =
            std::max(stats.max_transition_width, clamped_width);
        transition_width_sum += clamped_width;
        ++stats.transition_count;
    };

    for (const Pixel_region& region : regions) {
        for (int y = region.rect.top(); y <= region.rect.bottom(); ++y) {
            bool row_has_ink = false;
            bool row_has_transition = false;
            int x = region.rect.left();
            while (x <= region.rect.right()) {
                const int delta = pixel_delta(
                    image.pixelColor(x, y),
                    region.background);
                if (delta <= region.low_delta) {
                    ++x;
                    continue;
                }

                row_has_ink = true;
                const int run_left = x;
                int run_right = x;
                int first_solid = -1;
                int last_solid = -1;
                while (x <= region.rect.right()) {
                    const int run_delta = pixel_delta(
                        image.pixelColor(x, y),
                        region.background);
                    if (run_delta <= region.low_delta) {
                        break;
                    }
                    run_right = x;
                    if (run_delta >= region.strong_delta) {
                        if (first_solid < 0) {
                            first_solid = x;
                        }
                        last_solid = x;
                    }
                    ++x;
                }

                if (first_solid >= 0 && last_solid >= first_solid) {
                    record_transition_width(first_solid - run_left);
                    record_transition_width(run_right - last_solid);
                    row_has_transition = true;
                }
            }

            if (row_has_ink) {
                ++stats.sampled_rows;
            }
            if (row_has_transition) {
                ++stats.rows_with_transitions;
            }
        }
    }

    if (stats.transition_count > 0) {
        stats.average_transition_width =
            static_cast<double>(transition_width_sum) /
            static_cast<double>(stats.transition_count);
    }
    return stats;
}

Lcd_horizontal_edge_transition_stats
measure_lcd_horizontal_edge_transitions_pixels(
    const QImage&                      image,
    QRect                              region,
    QColor                             background,
    QColor                             foreground,
    const Lcd_first_ink_column_stats&  ink_stats)
{
    Lcd_horizontal_edge_transition_stats stats;
    if (image.isNull() || !ink_stats.has_ink()) {
        return stats;
    }

    stats.bbox = ink_stats.bbox
        .intersected(region)
        .intersected(image.rect());
    if (stats.bbox.isEmpty()) {
        return stats;
    }

    const Pixel_glyph_mask mask = {
        QRectF(),
        background,
        true,
        foreground,
    };
    const int low_delta = lcd_edge_low_delta_threshold(mask);
    const int strong_delta = lcd_edge_strong_delta_threshold(mask);

    int transition_width_sum = 0;
    auto record_transition_width = [&](int width) {
        const int clamped_width = std::max(0, width);
        stats.max_transition_width =
            std::max(stats.max_transition_width, clamped_width);
        transition_width_sum += clamped_width;
        ++stats.transition_count;
    };

    for (int y = stats.bbox.top(); y <= stats.bbox.bottom(); ++y) {
        int first_ink = -1;
        int last_ink = -1;
        int first_solid = -1;
        int last_solid = -1;
        for (int x = stats.bbox.left(); x <= stats.bbox.right(); ++x) {
            const int delta = pixel_delta(image.pixelColor(x, y), background);
            if (delta <= low_delta) {
                continue;
            }

            if (first_ink < 0) {
                first_ink = x;
            }
            last_ink = x;
            if (delta >= strong_delta) {
                if (first_solid < 0) {
                    first_solid = x;
                }
                last_solid = x;
            }
        }

        if (first_ink >= 0) {
            ++stats.sampled_rows;
        }
        if (first_solid >= 0 && last_solid >= first_solid) {
            record_transition_width(first_solid - first_ink);
            record_transition_width(last_ink - last_solid);
            ++stats.rows_with_transitions;
        }
    }

    if (stats.transition_count > 0) {
        stats.average_transition_width =
            static_cast<double>(transition_width_sum) /
            static_cast<double>(stats.transition_count);
    }
    return stats;
}

bool lcd_rect_components_within(QRect actual, QRect expected, int tolerance)
{
    return
        std::abs(actual.x()      - expected.x())      <= tolerance &&
        std::abs(actual.y()      - expected.y())      <= tolerance &&
        std::abs(actual.width()  - expected.width())  <= tolerance &&
        std::abs(actual.height() - expected.height()) <= tolerance;
}

double lcd_ink_pixel_ratio(int actual_ink_pixels, int expected_ink_pixels)
{
    if (expected_ink_pixels <= 0) {
        return 0.0;
    }
    return static_cast<double>(actual_ink_pixels) /
        static_cast<double>(expected_ink_pixels);
}

double lcd_nearest_integer_delta(double value)
{
    if (!std::isfinite(value)) {
        return std::numeric_limits<double>::infinity();
    }
    return std::abs(value - std::round(value));
}

bool lcd_is_near_integer(double value, double epsilon)
{
    return lcd_nearest_integer_delta(value) <= epsilon;
}

QString lcd_ascii_panel_representative_text()
{
    return QStringLiteral("WMeaom#@|1.LFPgjp");
}

QString lcd_ascii_panel_dense_text()
{
    return QStringLiteral("AaMmWwEeOo#@|1.[]{}+-=*/");
}

QString lcd_ascii_panel_cursor_text()
{
    return QStringLiteral("C");
}

QString lcd_ascii_panel_user_text()
{
    return QStringLiteral("build_msdf");
}

QPointF lcd_repeated_on_fractional_host(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    return QPointF(
        k_lcd_repeated_on_fractional_host_physical_x / dpr,
        0.0);
}

QPointF lcd_repeated_on_live_app_host(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    return QPointF(
        k_lcd_repeated_on_live_app_base_host_x + 1.0 / dpr,
        k_lcd_repeated_on_live_app_base_host_y + 1.0 / dpr);
}

QString lcd_ascii_panel_user_text_cell_label(int index, QChar character)
{
    return QStringLiteral("user_%1_%2")
        .arg(index, 2, 10, QLatin1Char('0'))
        .arg(character);
}

int lcd_ascii_panel_drawable_glyph_count(const QString& text)
{
    int count = 0;
    for (const QChar character : text) {
        const ushort unicode = character.unicode();
        if (unicode >= 0x21U && unicode <= 0x7eU) {
            ++count;
        }
    }
    return count;
}

int lcd_ascii_panel_expected_drawable_glyphs(
    const Pixel_parity_fixture& fixture)
{
    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    int count = 0;
    for (const term::Terminal_render_text_run& run : frame.text_runs) {
        count += lcd_ascii_panel_drawable_glyph_count(run.text);
    }
    for (const term::Terminal_render_text_run& run : frame.cursor_text_runs) {
        count += lcd_ascii_panel_drawable_glyph_count(run.text);
    }
    return count;
}

std::vector<Lcd_ascii_panel_cell_spec> lcd_ascii_panel_probe_cell_specs()
{
    const QString representative = lcd_ascii_panel_representative_text();
    std::vector<Lcd_ascii_panel_cell_spec> specs;
    specs.reserve(static_cast<std::size_t>(representative.size() + 1));

    for (int index = 0; index < representative.size(); ++index) {
        const QString text(representative.at(index));
        specs.push_back({
            text,
            text,
            k_lcd_ascii_panel_representative_row,
            k_lcd_ascii_panel_representative_column +
                index * k_lcd_ascii_panel_representative_column_stride,
            1,
            false,
        });
    }

    specs.push_back({
        QStringLiteral("cursor_text"),
        lcd_ascii_panel_cursor_text(),
        k_lcd_ascii_panel_cursor_row,
        k_lcd_ascii_panel_cursor_column,
        1,
        true,
    });

    const QString user_text = lcd_ascii_panel_user_text();
    for (int index = 0; index < user_text.size(); ++index) {
        const QChar character = user_text.at(index);
        specs.push_back({
            lcd_ascii_panel_user_text_cell_label(index, character),
            QString(character),
            k_lcd_ascii_panel_user_text_row,
            k_lcd_ascii_panel_user_text_column + index,
            1,
            false,
        });
    }
    return specs;
}

QRect lcd_physical_cell_rect(
    const Pixel_parity_fixture& fixture,
    int                         row,
    int                         column,
    int                         display_width,
    QPointF                     logical_origin = QPointF())
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(
        fixture.device_pixel_ratio);
    const double physical_cell_width =
        static_cast<double>(fixture.cell_metrics.width * dpr);
    const double physical_cell_height =
        static_cast<double>(fixture.cell_metrics.height * dpr);
    const QPoint physical_origin(
        static_cast<int>(std::lround(logical_origin.x() * dpr)),
        static_cast<int>(std::lround(logical_origin.y() * dpr)));
    return QRect(
        QPoint(
            physical_origin.x() +
                static_cast<int>(std::lround(
                    static_cast<double>(column) * physical_cell_width)),
            physical_origin.y() +
                static_cast<int>(std::lround(
                    static_cast<double>(row) * physical_cell_height))),
        QSize(
            static_cast<int>(std::lround(
                static_cast<double>(display_width) * physical_cell_width)),
            static_cast<int>(std::lround(physical_cell_height))));
}

Lcd_ascii_panel_probe_stats measure_lcd_ascii_panel_probe(
    const Pixel_parity_fixture& fixture,
    const QImage&               image)
{
    constexpr double k_integer_metric_epsilon = 0.001;

    Lcd_ascii_panel_probe_stats stats;
    stats.expected_drawable_glyphs =
        lcd_ascii_panel_expected_drawable_glyphs(fixture);
    const qreal dpr = pixel_normalized_device_pixel_ratio(
        fixture.device_pixel_ratio);
    stats.physical_cell_width =
        static_cast<double>(fixture.cell_metrics.width * dpr);
    stats.physical_cell_height =
        static_cast<double>(fixture.cell_metrics.height * dpr);
    stats.physical_cell_ascent =
        static_cast<double>(fixture.cell_metrics.ascent * dpr);
    stats.cell_width_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_width);
    stats.cell_height_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_height);
    stats.cell_ascent_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_ascent);
    stats.cell_width_integer =
        lcd_is_near_integer(stats.physical_cell_width, k_integer_metric_epsilon);
    stats.cell_height_integer =
        lcd_is_near_integer(stats.physical_cell_height, k_integer_metric_epsilon);
    stats.cell_ascent_integer =
        lcd_is_near_integer(stats.physical_cell_ascent, k_integer_metric_epsilon);

    const std::vector<Lcd_ascii_panel_cell_spec> specs =
        lcd_ascii_panel_probe_cell_specs();
    stats.probed_cell_count = static_cast<int>(specs.size());
    stats.cells.reserve(specs.size());
    if (image.isNull()) {
        return stats;
    }

    const QColor normal_background =
        QColor::fromRgba(k_lcd_w_probe_background_rgba);
    const QColor normal_foreground =
        QColor::fromRgba(k_lcd_w_probe_foreground_rgba);
    const QColor cursor_background = pixel_render_options().cursor_color;
    const QColor cursor_foreground =
        QColor::fromRgba(k_lcd_w_probe_background_rgba);
    bool all_cells_have_ink = true;
    bool all_cells_have_visible_ink = true;
    int min_ink_pixels = std::numeric_limits<int>::max();
    int min_visible_strong_pixels = std::numeric_limits<int>::max();
    for (const Lcd_ascii_panel_cell_spec& spec : specs) {
        const QColor background =
            spec.cursor_text ? cursor_background : normal_background;
        const QColor foreground =
            spec.cursor_text ? cursor_foreground : normal_foreground;
        const QRect cell_pixels = lcd_physical_cell_rect(
            fixture,
            spec.row,
            spec.column,
            spec.display_width)
            .intersected(image.rect());
        const Lcd_first_ink_column_stats ink =
            measure_lcd_first_ink_column_pixels(
                image,
                cell_pixels,
                background);
        const Lcd_visible_glyph_metrics visible =
            measure_lcd_visible_glyph_metrics_pixels(
                image,
                cell_pixels,
                background,
                foreground);

        Lcd_ascii_panel_cell_stats cell;
        cell.label = spec.label;
        cell.text = spec.text;
        cell.row = spec.row;
        cell.column = spec.column;
        cell.display_width = spec.display_width;
        cell.cursor_text = spec.cursor_text;
        cell.cell_pixels = cell_pixels;
        cell.ink_bbox = ink.bbox;
        cell.ink_pixels = ink.ink_pixels;
        cell.first_ink_x = ink.first_ink_x;
        cell.visible = visible;
        if (ink.has_ink()) {
            cell.relative_first_ink_x = ink.first_ink_x - cell_pixels.left();
            cell.relative_bbox_x = ink.bbox.x() - cell_pixels.left();
            cell.relative_bbox_y = ink.bbox.y() - cell_pixels.top();
            cell.relative_bbox_width = ink.bbox.width();
            cell.relative_bbox_height = ink.bbox.height();
            cell.relative_bbox_center_x =
                static_cast<double>(
                    ink.bbox.left() + ink.bbox.right()) * 0.5 -
                static_cast<double>(cell_pixels.left());
            cell.relative_bbox_center_y =
                static_cast<double>(
                    ink.bbox.top() + ink.bbox.bottom()) * 0.5 -
                static_cast<double>(cell_pixels.top());
            min_ink_pixels = std::min(min_ink_pixels, ink.ink_pixels);
        }
        else {
            all_cells_have_ink = false;
        }
        if (visible.has_visible_ink()) {
            min_visible_strong_pixels = std::min(
                min_visible_strong_pixels,
                visible.strong_pixels);
        }
        else {
            all_cells_have_visible_ink = false;
        }
        const int half_x = cell_pixels.left() + cell_pixels.width() / 2;
        const int half_y = cell_pixels.top() + cell_pixels.height() / 2;
        const int first_third_x =
            cell_pixels.left() + cell_pixels.width() / 3;
        const int second_third_x =
            cell_pixels.left() + (cell_pixels.width() * 2) / 3;
        for (int y = cell_pixels.top(); y <= cell_pixels.bottom(); ++y) {
            for (int x = cell_pixels.left(); x <= cell_pixels.right(); ++x) {
                if (pixel_delta(image.pixelColor(x, y), background) <= 8) {
                    continue;
                }
                if (x < half_x) {
                    ++cell.left_half_ink_pixels;
                }
                else {
                    ++cell.right_half_ink_pixels;
                }
                if (x < first_third_x) {
                    ++cell.left_third_ink_pixels;
                }
                else
                if (x < second_third_x) {
                    ++cell.center_third_ink_pixels;
                }
                else {
                    ++cell.right_third_ink_pixels;
                }
                if (y < half_y) {
                    ++cell.top_half_ink_pixels;
                }
                else {
                    ++cell.bottom_half_ink_pixels;
                }
            }
        }
        cell.edge_transitions =
            measure_lcd_horizontal_edge_transitions_pixels(
                image,
                cell_pixels,
                background,
                foreground,
                ink);
        if (cell.label == QStringLiteral("M") ||
            cell.label == QStringLiteral("e"))
        {
            stats.key_glyph_transition_count +=
                cell.edge_transitions.transition_count;
            stats.max_key_glyph_transition_width = std::max(
                stats.max_key_glyph_transition_width,
                cell.edge_transitions.max_transition_width);
            stats.max_key_glyph_average_transition_width = std::max(
                stats.max_key_glyph_average_transition_width,
                cell.edge_transitions.average_transition_width);
        }
        stats.cells.push_back(cell);
    }

    stats.all_probe_cells_have_ink =
        all_cells_have_ink &&
        static_cast<int>(stats.cells.size()) == stats.probed_cell_count;
    stats.all_probe_cells_have_visible_ink =
        all_cells_have_visible_ink &&
        static_cast<int>(stats.cells.size()) == stats.probed_cell_count;
    stats.min_ink_pixels =
        min_ink_pixels == std::numeric_limits<int>::max()
            ? 0
            : min_ink_pixels;
    stats.min_visible_strong_pixels =
        min_visible_strong_pixels == std::numeric_limits<int>::max()
            ? 0
            : min_visible_strong_pixels;
    return stats;
}

const Lcd_ascii_panel_cell_stats* lcd_ascii_panel_cell(
    const Lcd_ascii_panel_probe_stats& stats,
    const QString&                     label)
{
    const auto it = std::find_if(
        stats.cells.begin(),
        stats.cells.end(),
        [&](const Lcd_ascii_panel_cell_stats& cell) {
            return cell.label == label;
    });
    return it == stats.cells.end() ? nullptr : &*it;
}

std::vector<QString> lcd_ascii_panel_placement_probe_labels()
{
    const std::vector<Lcd_ascii_panel_cell_spec> specs =
        lcd_ascii_panel_probe_cell_specs();
    std::vector<QString> labels;
    labels.reserve(specs.size());
    for (const Lcd_ascii_panel_cell_spec& spec : specs) {
        if (!spec.cursor_text) {
            labels.push_back(spec.label);
        }
    }
    return labels;
}

std::vector<QString> lcd_ascii_panel_cursor_text_placement_probe_labels()
{
    return {QStringLiteral("cursor_text")};
}

std::vector<QString> lcd_ascii_panel_user_text_placement_probe_labels()
{
    const QString user_text = lcd_ascii_panel_user_text();
    std::vector<QString> labels;
    labels.reserve(static_cast<std::size_t>(user_text.size()));
    for (int index = 0; index < user_text.size(); ++index) {
        labels.push_back(
            lcd_ascii_panel_user_text_cell_label(
                index,
                user_text.at(index)));
    }
    return labels;
}

std::vector<QString> lcd_ascii_panel_user_text_visible_baseline_probe_labels()
{
    const QString user_text = lcd_ascii_panel_user_text();
    std::vector<QString> labels;
    labels.reserve(static_cast<std::size_t>(user_text.size()));
    for (int index = 0; index < user_text.size(); ++index) {
        const QChar character = user_text.at(index);
        if (character == QLatin1Char('_')) {
            continue;
        }
        labels.push_back(
            lcd_ascii_panel_user_text_cell_label(index, character));
    }
    return labels;
}

std::vector<std::pair<QString, QString>>
lcd_ascii_panel_user_text_targeted_visible_gutter_pairs()
{
    const QString user_text = lcd_ascii_panel_user_text();
    auto label_at = [&](int index) {
        return lcd_ascii_panel_user_text_cell_label(index, user_text.at(index));
    };
    return {
        {label_at(1), label_at(2)},
        {label_at(2), label_at(3)},
        {label_at(3), label_at(4)},
        {label_at(8), label_at(9)},
    };
}

Lcd_ascii_panel_placement_stats compare_lcd_ascii_panel_cell_placement(
    const Lcd_ascii_panel_probe_stats& reference,
    const Lcd_ascii_panel_probe_stats& candidate,
    const std::vector<QString>&        labels)
{
    Lcd_ascii_panel_placement_stats stats;
    stats.expected_cells = static_cast<int>(labels.size());
    stats.cells.reserve(labels.size());
    bool all_cells_have_ink = true;
    bool all_cells_have_visible_ink = true;
    bool has_signed_delta = false;
    bool has_visible_signed_delta = false;
    for (const QString& label : labels) {
        const Lcd_ascii_panel_cell_stats* const reference_cell =
            lcd_ascii_panel_cell(reference, label);
        const Lcd_ascii_panel_cell_stats* const candidate_cell =
            lcd_ascii_panel_cell(candidate, label);
        if (reference_cell == nullptr || candidate_cell == nullptr) {
            all_cells_have_ink = false;
            continue;
        }

        Lcd_ascii_panel_placement_delta delta;
        delta.label = label;
        delta.reference_has_ink = reference_cell->has_ink();
        delta.candidate_has_ink = candidate_cell->has_ink();
        delta.reference_has_visible_ink =
            reference_cell->visible.has_visible_ink();
        delta.candidate_has_visible_ink =
            candidate_cell->visible.has_visible_ink();
        if (!delta.reference_has_ink || !delta.candidate_has_ink) {
            all_cells_have_ink = false;
            stats.cells.push_back(delta);
            continue;
        }

        delta.left_delta = std::abs(
            candidate_cell->relative_bbox_x -
            reference_cell->relative_bbox_x);
        delta.right_delta = std::abs(
            candidate_cell->relative_bbox_x +
                candidate_cell->relative_bbox_width -
            reference_cell->relative_bbox_x -
                reference_cell->relative_bbox_width);
        delta.top_delta = std::abs(
            candidate_cell->relative_bbox_y -
            reference_cell->relative_bbox_y);
        delta.bottom_delta = std::abs(
            candidate_cell->relative_bbox_y +
                candidate_cell->relative_bbox_height -
            reference_cell->relative_bbox_y -
                reference_cell->relative_bbox_height);
        delta.first_ink_x_delta = std::abs(
            candidate_cell->relative_first_ink_x -
            reference_cell->relative_first_ink_x);
        delta.center_x_delta = std::abs(
            candidate_cell->relative_bbox_center_x -
            reference_cell->relative_bbox_center_x);
        delta.center_y_delta = std::abs(
            candidate_cell->relative_bbox_center_y -
            reference_cell->relative_bbox_center_y);
        if (!delta.reference_has_visible_ink ||
            !delta.candidate_has_visible_ink)
        {
            all_cells_have_visible_ink = false;
        }
        else {
            delta.visible_left_delta = std::abs(
                candidate_cell->visible.relative_visible_left_x -
                reference_cell->visible.relative_visible_left_x);
            delta.visible_right_delta = std::abs(
                candidate_cell->visible.relative_visible_right_x -
                reference_cell->visible.relative_visible_right_x);
            delta.visible_top_delta = std::abs(
                candidate_cell->visible.relative_visible_top_y -
                reference_cell->visible.relative_visible_top_y);
            delta.visible_baseline_delta = std::abs(
                candidate_cell->visible.relative_visible_baseline_y -
                reference_cell->visible.relative_visible_baseline_y);
            delta.visible_weighted_center_x_delta = std::abs(
                candidate_cell->visible.relative_weighted_center_x -
                reference_cell->visible.relative_weighted_center_x);
            delta.visible_weighted_center_y_delta = std::abs(
                candidate_cell->visible.relative_weighted_center_y -
                reference_cell->visible.relative_weighted_center_y);
            delta.signed_visible_top_delta =
                candidate_cell->visible.relative_visible_top_y -
                reference_cell->visible.relative_visible_top_y;
            delta.signed_visible_baseline_delta =
                candidate_cell->visible.relative_visible_baseline_y -
                reference_cell->visible.relative_visible_baseline_y;
            delta.signed_visible_weighted_center_y_delta =
                candidate_cell->visible.relative_weighted_center_y -
                reference_cell->visible.relative_weighted_center_y;
        }
        delta.signed_top_delta =
            candidate_cell->relative_bbox_y -
            reference_cell->relative_bbox_y;
        delta.signed_bottom_delta =
            candidate_cell->relative_bbox_y +
                candidate_cell->relative_bbox_height -
            reference_cell->relative_bbox_y -
                reference_cell->relative_bbox_height;
        delta.signed_center_y_delta =
            candidate_cell->relative_bbox_center_y -
            reference_cell->relative_bbox_center_y;

        stats.max_left_delta = std::max(
            stats.max_left_delta,
            delta.left_delta);
        stats.max_right_delta = std::max(
            stats.max_right_delta,
            delta.right_delta);
        stats.max_top_delta = std::max(
            stats.max_top_delta,
            delta.top_delta);
        stats.max_bottom_delta = std::max(
            stats.max_bottom_delta,
            delta.bottom_delta);
        stats.max_first_ink_x_delta = std::max(
            stats.max_first_ink_x_delta,
            delta.first_ink_x_delta);
        stats.max_visible_left_delta = std::max(
            stats.max_visible_left_delta,
            delta.visible_left_delta);
        stats.max_visible_right_delta = std::max(
            stats.max_visible_right_delta,
            delta.visible_right_delta);
        stats.max_visible_top_delta = std::max(
            stats.max_visible_top_delta,
            delta.visible_top_delta);
        stats.max_visible_baseline_delta = std::max(
            stats.max_visible_baseline_delta,
            delta.visible_baseline_delta);
        stats.max_center_x_delta = std::max(
            stats.max_center_x_delta,
            delta.center_x_delta);
        stats.max_center_y_delta = std::max(
            stats.max_center_y_delta,
            delta.center_y_delta);
        stats.max_visible_weighted_center_x_delta = std::max(
            stats.max_visible_weighted_center_x_delta,
            delta.visible_weighted_center_x_delta);
        stats.max_visible_weighted_center_y_delta = std::max(
            stats.max_visible_weighted_center_y_delta,
            delta.visible_weighted_center_y_delta);
        if (!has_signed_delta) {
            stats.min_signed_top_delta = delta.signed_top_delta;
            stats.max_signed_top_delta = delta.signed_top_delta;
            stats.min_signed_bottom_delta = delta.signed_bottom_delta;
            stats.max_signed_bottom_delta = delta.signed_bottom_delta;
            stats.min_signed_center_y_delta = delta.signed_center_y_delta;
            stats.max_signed_center_y_delta = delta.signed_center_y_delta;
            has_signed_delta = true;
        }
        else {
            stats.min_signed_top_delta = std::min(
                stats.min_signed_top_delta,
                delta.signed_top_delta);
            stats.max_signed_top_delta = std::max(
                stats.max_signed_top_delta,
                delta.signed_top_delta);
            stats.min_signed_bottom_delta = std::min(
                stats.min_signed_bottom_delta,
                delta.signed_bottom_delta);
            stats.max_signed_bottom_delta = std::max(
                stats.max_signed_bottom_delta,
                delta.signed_bottom_delta);
            stats.min_signed_center_y_delta = std::min(
                stats.min_signed_center_y_delta,
                delta.signed_center_y_delta);
            stats.max_signed_center_y_delta = std::max(
                stats.max_signed_center_y_delta,
                delta.signed_center_y_delta);
        }
        if (delta.reference_has_visible_ink &&
            delta.candidate_has_visible_ink)
        {
            if (!has_visible_signed_delta) {
                stats.min_signed_visible_top_delta =
                    delta.signed_visible_top_delta;
                stats.max_signed_visible_top_delta =
                    delta.signed_visible_top_delta;
                stats.min_signed_visible_baseline_delta =
                    delta.signed_visible_baseline_delta;
                stats.max_signed_visible_baseline_delta =
                    delta.signed_visible_baseline_delta;
                stats.min_signed_visible_weighted_center_y_delta =
                    delta.signed_visible_weighted_center_y_delta;
                stats.max_signed_visible_weighted_center_y_delta =
                    delta.signed_visible_weighted_center_y_delta;
                has_visible_signed_delta = true;
            }
            else {
                stats.min_signed_visible_top_delta = std::min(
                    stats.min_signed_visible_top_delta,
                    delta.signed_visible_top_delta);
                stats.max_signed_visible_top_delta = std::max(
                    stats.max_signed_visible_top_delta,
                    delta.signed_visible_top_delta);
                stats.min_signed_visible_baseline_delta = std::min(
                    stats.min_signed_visible_baseline_delta,
                    delta.signed_visible_baseline_delta);
                stats.max_signed_visible_baseline_delta = std::max(
                    stats.max_signed_visible_baseline_delta,
                    delta.signed_visible_baseline_delta);
                stats.min_signed_visible_weighted_center_y_delta = std::min(
                    stats.min_signed_visible_weighted_center_y_delta,
                    delta.signed_visible_weighted_center_y_delta);
                stats.max_signed_visible_weighted_center_y_delta = std::max(
                    stats.max_signed_visible_weighted_center_y_delta,
                    delta.signed_visible_weighted_center_y_delta);
            }
        }
        ++stats.compared_cells;
        stats.cells.push_back(delta);
    }

    stats.signed_top_delta_range =
        stats.max_signed_top_delta - stats.min_signed_top_delta;
    stats.signed_bottom_delta_range =
        stats.max_signed_bottom_delta - stats.min_signed_bottom_delta;
    stats.signed_center_y_delta_range =
        stats.max_signed_center_y_delta - stats.min_signed_center_y_delta;
    stats.signed_visible_top_delta_range =
        stats.max_signed_visible_top_delta -
        stats.min_signed_visible_top_delta;
    stats.signed_visible_baseline_delta_range =
        stats.max_signed_visible_baseline_delta -
        stats.min_signed_visible_baseline_delta;
    stats.signed_visible_weighted_center_y_delta_range =
        stats.max_signed_visible_weighted_center_y_delta -
        stats.min_signed_visible_weighted_center_y_delta;
    stats.all_cells_have_ink =
        all_cells_have_ink &&
        stats.compared_cells == stats.expected_cells;
    stats.all_cells_have_visible_ink =
        all_cells_have_visible_ink &&
        stats.compared_cells == stats.expected_cells;
    return stats;
}

Lcd_ascii_panel_gutter_stats compare_lcd_ascii_panel_visible_gutters(
    const Lcd_ascii_panel_probe_stats&              reference,
    const Lcd_ascii_panel_probe_stats&              candidate,
    const std::vector<std::pair<QString, QString>>& pairs)
{
    Lcd_ascii_panel_gutter_stats stats;
    stats.expected_pairs = static_cast<int>(pairs.size());
    stats.pairs.reserve(pairs.size());
    bool all_pairs_visible = true;
    for (const auto& pair : pairs) {
        const Lcd_ascii_panel_cell_stats* const reference_left =
            lcd_ascii_panel_cell(reference, pair.first);
        const Lcd_ascii_panel_cell_stats* const reference_right =
            lcd_ascii_panel_cell(reference, pair.second);
        const Lcd_ascii_panel_cell_stats* const candidate_left =
            lcd_ascii_panel_cell(candidate, pair.first);
        const Lcd_ascii_panel_cell_stats* const candidate_right =
            lcd_ascii_panel_cell(candidate, pair.second);

        Lcd_ascii_panel_gutter_pair_stats pair_stats;
        pair_stats.left_label = pair.first;
        pair_stats.right_label = pair.second;
        pair_stats.reference_visible =
            reference_left != nullptr &&
            reference_right != nullptr &&
            reference_left->visible.has_visible_ink() &&
            reference_right->visible.has_visible_ink();
        pair_stats.candidate_visible =
            candidate_left != nullptr &&
            candidate_right != nullptr &&
            candidate_left->visible.has_visible_ink() &&
            candidate_right->visible.has_visible_ink();
        if (!pair_stats.reference_visible || !pair_stats.candidate_visible) {
            all_pairs_visible = false;
            stats.pairs.push_back(pair_stats);
            continue;
        }

        pair_stats.reference_gutter =
            reference_right->visible.visible_left_x -
            reference_left->visible.visible_right_x - 1;
        pair_stats.candidate_gutter =
            candidate_right->visible.visible_left_x -
            candidate_left->visible.visible_right_x - 1;
        pair_stats.gutter_delta = std::abs(
            pair_stats.candidate_gutter - pair_stats.reference_gutter);
        stats.max_gutter_delta = std::max(
            stats.max_gutter_delta,
            pair_stats.gutter_delta);
        ++stats.compared_pairs;
        stats.pairs.push_back(pair_stats);
    }

    stats.all_pairs_visible =
        all_pairs_visible &&
        stats.compared_pairs == stats.expected_pairs;
    return stats;
}

QColor lcd_repeated_w_background(const Pixel_parity_fixture& fixture)
{
    if (!fixture.glyph_masks.empty()) {
        return fixture.glyph_masks.front().background;
    }
    return QColor::fromRgba(k_lcd_w_probe_background_rgba);
}

char lcd_intensity_x_row_glyph(int row_index)
{
    return row_index < k_lcd_intensity_x_uppercase_rows ? 'X' : 'x';
}

Lcd_repeated_on_stability_stats measure_lcd_repeated_on_stability(
    const Pixel_parity_fixture& fixture,
    const QImage&               image,
    QPointF                     logical_origin = QPointF())
{
    Lcd_repeated_on_stability_stats stats;
    const qreal dpr = pixel_normalized_device_pixel_ratio(
        fixture.device_pixel_ratio);
    stats.physical_cell_width =
        static_cast<double>(fixture.cell_metrics.width * dpr);
    stats.physical_cell_height =
        static_cast<double>(fixture.cell_metrics.height * dpr);
    stats.physical_cell_ascent =
        static_cast<double>(fixture.cell_metrics.ascent * dpr);

    const std::array<Lcd_repeated_on_glyph_stats, 4> expected = {{
        {QStringLiteral("first_o"),  k_lcd_repeated_on_row, k_lcd_repeated_on_first_column},
        {QStringLiteral("first_n"),  k_lcd_repeated_on_row, k_lcd_repeated_on_first_column + 1},
        {QStringLiteral("second_o"), k_lcd_repeated_on_row, k_lcd_repeated_on_second_column},
        {QStringLiteral("second_n"), k_lcd_repeated_on_row, k_lcd_repeated_on_second_column + 1},
    }};
    stats.expected_glyphs = static_cast<int>(expected.size());
    stats.glyphs.reserve(expected.size());
    if (image.isNull()) {
        return stats;
    }

    const QColor background = lcd_repeated_w_background(fixture);
    const QColor foreground = QColor::fromRgba(k_lcd_w_probe_foreground_rgba);
    for (const Lcd_repeated_on_glyph_stats& expected_glyph : expected) {
        Lcd_repeated_on_glyph_stats glyph = expected_glyph;
        glyph.cell_pixels = lcd_physical_cell_rect(
            fixture,
            glyph.row,
            glyph.column,
            1,
            logical_origin)
            .intersected(image.rect());
        glyph.visible = measure_lcd_visible_glyph_metrics_pixels(
            image,
            glyph.cell_pixels,
            background,
            foreground);
        if (glyph.visible.has_visible_ink()) {
            ++stats.visible_glyphs;
        }
        stats.glyphs.push_back(glyph);
    }

    stats.all_glyphs_visible =
        stats.visible_glyphs == stats.expected_glyphs;
    if (!stats.all_glyphs_visible || stats.glyphs.size() != expected.size()) {
        return stats;
    }

    auto compare_same_glyph = [&](std::size_t left_index, std::size_t right_index) {
        const Lcd_visible_glyph_metrics& left =
            stats.glyphs.at(left_index).visible;
        const Lcd_visible_glyph_metrics& right =
            stats.glyphs.at(right_index).visible;
        stats.max_visible_left_delta = std::max(
            stats.max_visible_left_delta,
            std::abs(
                left.relative_visible_left_x -
                right.relative_visible_left_x));
        stats.max_visible_right_delta = std::max(
            stats.max_visible_right_delta,
            std::abs(
                left.relative_visible_right_x -
                right.relative_visible_right_x));
        stats.max_visible_top_delta = std::max(
            stats.max_visible_top_delta,
            std::abs(
                left.relative_visible_top_y -
                right.relative_visible_top_y));
        stats.max_visible_baseline_delta = std::max(
            stats.max_visible_baseline_delta,
            std::abs(
                left.relative_visible_baseline_y -
                right.relative_visible_baseline_y));
        stats.max_weighted_center_x_delta = std::max(
            stats.max_weighted_center_x_delta,
            std::abs(
                left.relative_weighted_center_x -
                right.relative_weighted_center_x));
        stats.max_weighted_center_y_delta = std::max(
            stats.max_weighted_center_y_delta,
            std::abs(
                left.relative_weighted_center_y -
                right.relative_weighted_center_y));
        ++stats.compared_same_glyph_pairs;
    };
    compare_same_glyph(0U, 2U);
    compare_same_glyph(1U, 3U);

    stats.first_word_gutter =
        stats.glyphs.at(1U).visible.visible_left_x -
        stats.glyphs.at(0U).visible.visible_right_x - 1;
    stats.second_word_gutter =
        stats.glyphs.at(3U).visible.visible_left_x -
        stats.glyphs.at(2U).visible.visible_right_x - 1;
    stats.gutter_delta =
        std::abs(stats.second_word_gutter - stats.first_word_gutter);
    return stats;
}

Lcd_repeated_w_stability_stats measure_lcd_repeated_w_stability(
    const Pixel_parity_fixture& fixture,
    const QImage&               image,
    int                         row,
    int                         first_column,
    int                         column_stride,
    int                         cell_count,
    QPointF                     logical_origin = QPointF())
{
    constexpr double k_integer_metric_epsilon = 0.001;

    Lcd_repeated_w_stability_stats stats;
    stats.cell_count = cell_count;
    const qreal dpr = pixel_normalized_device_pixel_ratio(
        fixture.device_pixel_ratio);
    stats.physical_cell_width =
        static_cast<double>(fixture.cell_metrics.width * dpr);
    stats.physical_cell_height =
        static_cast<double>(fixture.cell_metrics.height * dpr);
    stats.physical_cell_ascent =
        static_cast<double>(fixture.cell_metrics.ascent * dpr);
    stats.cell_width_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_width);
    stats.cell_height_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_height);
    stats.cell_ascent_integer_delta =
        lcd_nearest_integer_delta(stats.physical_cell_ascent);
    stats.cell_width_integer =
        lcd_is_near_integer(stats.physical_cell_width, k_integer_metric_epsilon);
    stats.cell_height_integer =
        lcd_is_near_integer(stats.physical_cell_height, k_integer_metric_epsilon);
    stats.cell_ascent_integer =
        lcd_is_near_integer(stats.physical_cell_ascent, k_integer_metric_epsilon);

    if (image.isNull() || cell_count <= 0) {
        return stats;
    }

    const QColor background = lcd_repeated_w_background(fixture);
    stats.cells.reserve(static_cast<std::size_t>(cell_count));
    const int cell_width_pixels = static_cast<int>(
        std::lround(stats.physical_cell_width));
    const int cell_height_pixels = static_cast<int>(
        std::lround(stats.physical_cell_height));
    const QPoint physical_origin(
        static_cast<int>(std::lround(logical_origin.x() * dpr)),
        static_cast<int>(std::lround(logical_origin.y() * dpr)));
    bool all_cells_have_ink = true;
    for (int cell_index = 0; cell_index < cell_count; ++cell_index) {
        const int column = first_column + cell_index * column_stride;
        const QRect cell_pixels = QRect(
            QPoint(
                physical_origin.x() +
                static_cast<int>(std::lround(
                    static_cast<double>(column) *
                    stats.physical_cell_width)),
                physical_origin.y() +
                static_cast<int>(std::lround(
                    static_cast<double>(row) *
                    stats.physical_cell_height))),
            QSize(cell_width_pixels, cell_height_pixels))
            .intersected(image.rect());
        const Lcd_first_ink_column_stats ink =
            measure_lcd_first_ink_column_pixels(
                image,
                cell_pixels,
                background);

        Lcd_repeated_w_cell_stats cell;
        cell.cell_pixels = cell_pixels;
        cell.ink_bbox = ink.bbox;
        cell.ink_pixels = ink.ink_pixels;
        cell.first_ink_x = ink.first_ink_x;
        if (ink.has_ink()) {
            cell.relative_first_ink_x = ink.first_ink_x - cell_pixels.left();
            cell.relative_bbox_x = ink.bbox.x() - cell_pixels.left();
            cell.relative_bbox_y = ink.bbox.y() - cell_pixels.top();
            cell.relative_bbox_width = ink.bbox.width();
            cell.relative_bbox_height = ink.bbox.height();
        }
        else {
            all_cells_have_ink = false;
        }
        stats.cells.push_back(cell);
    }
    stats.all_cells_have_ink =
        all_cells_have_ink &&
        static_cast<int>(stats.cells.size()) == cell_count;

    if (stats.cells.empty()) {
        return stats;
    }

    const Lcd_repeated_w_cell_stats& reference_cell = stats.cells.front();
    for (std::size_t cell_index = 1; cell_index < stats.cells.size(); ++cell_index) {
        const Lcd_repeated_w_cell_stats& cell = stats.cells[cell_index];
        const int width = std::min(
            reference_cell.cell_pixels.width(),
            cell.cell_pixels.width());
        const int height = std::min(
            reference_cell.cell_pixels.height(),
            cell.cell_pixels.height());
        if (width <= 0 || height <= 0) {
            continue;
        }

        int cell_diff_pixels = 0;
        int cell_ink_mask_diff_pixels = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                ++stats.compared_pixels;
                const QColor reference_pixel = image.pixelColor(
                    reference_cell.cell_pixels.left() + x,
                    reference_cell.cell_pixels.top() + y);
                const QColor cell_pixel = image.pixelColor(
                    cell.cell_pixels.left() + x,
                    cell.cell_pixels.top() + y);
                const int delta = pixel_delta(
                    reference_pixel,
                    cell_pixel);
                stats.max_delta = std::max(stats.max_delta, delta);
                if (delta > 0) {
                    ++stats.diff_pixels;
                    ++cell_diff_pixels;
                }
                const bool reference_ink =
                    pixel_delta(reference_pixel, background) > 8;
                const bool cell_ink =
                    pixel_delta(cell_pixel, background) > 8;
                if (reference_ink != cell_ink) {
                    ++stats.ink_mask_diff_pixels;
                    ++cell_ink_mask_diff_pixels;
                }
            }
        }
        stats.max_diff_pixels_per_cell =
            std::max(stats.max_diff_pixels_per_cell, cell_diff_pixels);
        stats.max_ink_mask_diff_pixels_per_cell = std::max(
            stats.max_ink_mask_diff_pixels_per_cell,
            cell_ink_mask_diff_pixels);
        ++stats.compared_cells;
    }

    if (stats.all_cells_have_ink) {
        for (std::size_t cell_index = 1; cell_index < stats.cells.size(); ++cell_index) {
            const Lcd_repeated_w_cell_stats& cell = stats.cells[cell_index];
            stats.max_ink_pixels_delta = std::max(
                stats.max_ink_pixels_delta,
                std::abs(cell.ink_pixels - reference_cell.ink_pixels));
            stats.max_relative_first_ink_x_delta = std::max(
                stats.max_relative_first_ink_x_delta,
                std::abs(
                    cell.relative_first_ink_x -
                    reference_cell.relative_first_ink_x));
            stats.max_relative_bbox_x_delta = std::max(
                stats.max_relative_bbox_x_delta,
                std::abs(cell.relative_bbox_x - reference_cell.relative_bbox_x));
            stats.max_relative_bbox_y_delta = std::max(
                stats.max_relative_bbox_y_delta,
                std::abs(cell.relative_bbox_y - reference_cell.relative_bbox_y));
            stats.max_relative_bbox_width_delta = std::max(
                stats.max_relative_bbox_width_delta,
                std::abs(
                    cell.relative_bbox_width -
                    reference_cell.relative_bbox_width));
            stats.max_relative_bbox_height_delta = std::max(
                stats.max_relative_bbox_height_delta,
                std::abs(
                    cell.relative_bbox_height -
                    reference_cell.relative_bbox_height));
        }
    }

    return stats;
}

std::vector<Lcd_intensity_x_row_stability> measure_lcd_intensity_x_stability(
    const Pixel_parity_fixture& fixture,
    const QImage&               image)
{
    std::vector<Lcd_intensity_x_row_stability> rows;
    rows.reserve(static_cast<std::size_t>(k_lcd_intensity_x_rows));
    for (int row_index = 0; row_index < k_lcd_intensity_x_rows; ++row_index) {
        const int row = k_lcd_intensity_x_first_row + row_index;

        Lcd_intensity_x_row_stability item;
        item.row_index       = row_index;
        item.row             = row;
        item.glyph           = lcd_intensity_x_row_glyph(row_index);
        item.foreground_rgba = k_lcd_intensity_x_foreground_rgba[
            static_cast<std::size_t>(row_index)];
        item.stability = measure_lcd_repeated_w_stability(
            fixture,
            image,
            row,
            k_lcd_intensity_x_first_column,
            k_lcd_intensity_x_column_stride,
            k_lcd_intensity_x_columns);
        rows.push_back(std::move(item));
    }
    return rows;
}

Lcd_repeated_w_stability_stats compare_lcd_repeated_w_corresponding_cells(
    const Pixel_parity_fixture&            fixture,
    const QImage&                          reference_image,
    const Lcd_repeated_w_stability_stats&  reference_stats,
    const QImage&                          candidate_image,
    const Lcd_repeated_w_stability_stats&  candidate_stats)
{
    Lcd_repeated_w_stability_stats stats;
    stats.cell_count = std::min(
        static_cast<int>(reference_stats.cells.size()),
        static_cast<int>(candidate_stats.cells.size()));
    stats.physical_cell_width       = reference_stats.physical_cell_width;
    stats.physical_cell_height      = reference_stats.physical_cell_height;
    stats.physical_cell_ascent      = reference_stats.physical_cell_ascent;
    stats.cell_width_integer_delta  = reference_stats.cell_width_integer_delta;
    stats.cell_height_integer_delta = reference_stats.cell_height_integer_delta;
    stats.cell_ascent_integer_delta = reference_stats.cell_ascent_integer_delta;
    stats.cell_width_integer        = reference_stats.cell_width_integer;
    stats.cell_height_integer       = reference_stats.cell_height_integer;
    stats.cell_ascent_integer       = reference_stats.cell_ascent_integer;
    stats.all_cells_have_ink        = stats.cell_count > 0;

    if (reference_image.isNull() ||
        candidate_image.isNull() ||
        stats.cell_count <= 0)
    {
        stats.all_cells_have_ink = false;
        return stats;
    }

    const QColor background = lcd_repeated_w_background(fixture);
    stats.cells.reserve(static_cast<std::size_t>(stats.cell_count));
    for (int cell_index = 0; cell_index < stats.cell_count; ++cell_index) {
        const Lcd_repeated_w_cell_stats& reference_cell =
            reference_stats.cells[static_cast<std::size_t>(cell_index)];
        const Lcd_repeated_w_cell_stats& candidate_cell =
            candidate_stats.cells[static_cast<std::size_t>(cell_index)];
        stats.cells.push_back(candidate_cell);

        const int width = std::min(
            reference_cell.cell_pixels.width(),
            candidate_cell.cell_pixels.width());
        const int height = std::min(
            reference_cell.cell_pixels.height(),
            candidate_cell.cell_pixels.height());
        if (width <= 0 || height <= 0) {
            stats.all_cells_have_ink = false;
            continue;
        }

        int cell_diff_pixels = 0;
        int cell_ink_mask_diff_pixels = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                ++stats.compared_pixels;
                const QColor reference_pixel = reference_image.pixelColor(
                    reference_cell.cell_pixels.left() + x,
                    reference_cell.cell_pixels.top() + y);
                const QColor candidate_pixel = candidate_image.pixelColor(
                    candidate_cell.cell_pixels.left() + x,
                    candidate_cell.cell_pixels.top() + y);
                const int delta = pixel_delta(
                    reference_pixel,
                    candidate_pixel);
                stats.max_delta = std::max(stats.max_delta, delta);
                if (delta > 0) {
                    ++stats.diff_pixels;
                    ++cell_diff_pixels;
                }

                const bool reference_ink =
                    pixel_delta(reference_pixel, background) > 8;
                const bool candidate_ink =
                    pixel_delta(candidate_pixel, background) > 8;
                if (reference_ink != candidate_ink) {
                    ++stats.ink_mask_diff_pixels;
                    ++cell_ink_mask_diff_pixels;
                }
            }
        }

        stats.max_diff_pixels_per_cell =
            std::max(stats.max_diff_pixels_per_cell, cell_diff_pixels);
        stats.max_ink_mask_diff_pixels_per_cell = std::max(
            stats.max_ink_mask_diff_pixels_per_cell,
            cell_ink_mask_diff_pixels);
        ++stats.compared_cells;

        const bool cells_have_ink =
            reference_cell.ink_pixels > 0 &&
            candidate_cell.ink_pixels > 0;
        stats.all_cells_have_ink = stats.all_cells_have_ink && cells_have_ink;
        if (cells_have_ink) {
            stats.max_ink_pixels_delta = std::max(
                stats.max_ink_pixels_delta,
                std::abs(
                    candidate_cell.ink_pixels -
                    reference_cell.ink_pixels));
            stats.max_relative_first_ink_x_delta = std::max(
                stats.max_relative_first_ink_x_delta,
                std::abs(
                    candidate_cell.relative_first_ink_x -
                    reference_cell.relative_first_ink_x));
            stats.max_relative_bbox_x_delta = std::max(
                stats.max_relative_bbox_x_delta,
                std::abs(
                    candidate_cell.relative_bbox_x -
                    reference_cell.relative_bbox_x));
            stats.max_relative_bbox_y_delta = std::max(
                stats.max_relative_bbox_y_delta,
                std::abs(
                    candidate_cell.relative_bbox_y -
                    reference_cell.relative_bbox_y));
            stats.max_relative_bbox_width_delta = std::max(
                stats.max_relative_bbox_width_delta,
                std::abs(
                    candidate_cell.relative_bbox_width -
                    reference_cell.relative_bbox_width));
            stats.max_relative_bbox_height_delta = std::max(
                stats.max_relative_bbox_height_delta,
                std::abs(
                    candidate_cell.relative_bbox_height -
                    reference_cell.relative_bbox_height));
        }
    }

    return stats;
}

Pixel_diff_stats compare_regions(
    const QImage&              left,
    const QImage&              right,
    const std::vector<QRectF>& logical_regions,
    qreal                      device_pixel_ratio)
{
    Pixel_diff_stats stats;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
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
    qreal                      device_pixel_ratio,
    QPoint&                    out_position,
    QColor&                    out_left,
    QColor&                    out_right)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
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

Pixel_decorative_ink_stats measure_decorative_ink(
    const QImage&                       image,
    const Pixel_decorative_primitive&  primitive,
    qreal                               device_pixel_ratio)
{
    constexpr int k_color_delta_tolerance = 2;

    Pixel_decorative_ink_stats stats;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const qreal inflate = 2.0 / dpr;
    const QRect region = logical_rect_to_pixels(
        primitive.rect.adjusted(-inflate, -inflate, inflate, inflate),
        dpr).intersected(image.rect());

    int left   = std::numeric_limits<int>::max();
    int top    = std::numeric_limits<int>::max();
    int right  = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    double x_sum = 0.0;
    double y_sum = 0.0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (pixel_delta(image.pixelColor(x, y), primitive.color) >
                k_color_delta_tolerance)
            {
                continue;
            }

            left   = std::min(left, x);
            top    = std::min(top, y);
            right  = std::max(right, x);
            bottom = std::max(bottom, y);
            x_sum += static_cast<double>(x) + 0.5;
            y_sum += static_cast<double>(y) + 0.5;
            ++stats.ink_pixels;
        }
    }

    if (stats.ink_pixels <= 0) {
        return stats;
    }

    stats.bbox = QRect(
        QPoint(left, top),
        QPoint(right, bottom));
    stats.center_x = x_sum / static_cast<double>(stats.ink_pixels);
    stats.center_y = y_sum / static_cast<double>(stats.ink_pixels);
    return stats;
}

bool decorative_primitive_is_horizontal(
    const Pixel_decorative_primitive& primitive)
{
    return primitive.rect.width() >= primitive.rect.height();
}

double decorative_expected_centerline(
    const Pixel_decorative_primitive& primitive,
    qreal                              device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    return decorative_primitive_is_horizontal(primitive)
        ? primitive.rect.center().y() * dpr
        : primitive.rect.center().x() * dpr;
}

double decorative_measured_centerline(
    const Pixel_decorative_primitive& primitive,
    const Pixel_decorative_ink_stats& stats)
{
    return decorative_primitive_is_horizontal(primitive)
        ? stats.center_y
        : stats.center_x;
}

bool compare_decorative_primitives(
    const Pixel_parity_fixture& fixture,
    const QImage&                reference,
    const QImage&                atlas,
    qreal                        device_pixel_ratio,
    const char*                  parity_mode)
{
    constexpr int    k_bbox_tolerance_pixels = 1;
    constexpr double k_center_tolerance_pixels = 1.0;

    bool ok = true;
    for (const Pixel_decorative_primitive& primitive :
        fixture.decorative_primitives)
    {
        const Pixel_decorative_ink_stats reference_ink =
            measure_decorative_ink(reference, primitive, device_pixel_ratio);
        const Pixel_decorative_ink_stats atlas_ink =
            measure_decorative_ink(atlas, primitive, device_pixel_ratio);
        if (!reference_ink.has_ink() || !atlas_ink.has_ink()) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " decorative primitive " << primitive.name
                << " produced zero ink"
                << " reference_ink=" << reference_ink.ink_pixels
                << " atlas_ink=" << atlas_ink.ink_pixels
                << " render_dpr=" << device_pixel_ratio << '\n';
            ok = false;
            continue;
        }

        const double reference_centerline =
            decorative_measured_centerline(primitive, reference_ink);
        const double atlas_centerline =
            decorative_measured_centerline(primitive, atlas_ink);
        const double expected_centerline =
            decorative_expected_centerline(primitive, device_pixel_ratio);
        const bool bbox_matches =
            std::abs(reference_ink.bbox.left() - atlas_ink.bbox.left()) <=
                k_bbox_tolerance_pixels &&
            std::abs(reference_ink.bbox.right() - atlas_ink.bbox.right()) <=
                k_bbox_tolerance_pixels &&
            std::abs(reference_ink.bbox.top() - atlas_ink.bbox.top()) <=
                k_bbox_tolerance_pixels &&
            std::abs(reference_ink.bbox.bottom() - atlas_ink.bbox.bottom()) <=
                k_bbox_tolerance_pixels;
        const bool centerline_matches =
            std::abs(reference_centerline - atlas_centerline) <=
                k_center_tolerance_pixels &&
            std::abs(reference_centerline - expected_centerline) <=
                k_center_tolerance_pixels &&
            std::abs(atlas_centerline - expected_centerline) <=
                k_center_tolerance_pixels;
        if (!bbox_matches || !centerline_matches) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " decorative primitive " << primitive.name
                << " placement drift"
                << " reference_bbox=" << reference_ink.bbox.x() << ','
                << reference_ink.bbox.y() << ' '
                << reference_ink.bbox.width() << 'x'
                << reference_ink.bbox.height()
                << " atlas_bbox=" << atlas_ink.bbox.x() << ','
                << atlas_ink.bbox.y() << ' '
                << atlas_ink.bbox.width() << 'x'
                << atlas_ink.bbox.height()
                << " reference_centerline=" << reference_centerline
                << " atlas_centerline=" << atlas_centerline
                << " expected_centerline=" << expected_centerline
                << " render_dpr=" << device_pixel_ratio << '\n';
            ok = false;
        }
    }
    return ok;
}

Pixel_glyph_stats compare_glyph_regions(
    const QImage&                         reference,
    const QImage&                         atlas,
    const std::vector<Pixel_glyph_mask>& logical_regions,
    qreal                                 device_pixel_ratio)
{
    constexpr int k_ink_delta_threshold = 8;

    Pixel_glyph_stats stats;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    for (const Pixel_glyph_mask& logical_region : logical_regions) {
        Pixel_glyph_mask_stats mask_stats;
        mask_stats.perimeter_pixels =
            glyph_mask_physical_perimeter_pixels(
                logical_region,
                device_pixel_ratio);
        const QRect region = logical_rect_to_pixels(logical_region.rect, dpr)
            .intersected(reference.rect())
            .intersected(atlas.rect());
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                ++mask_stats.compared_pixels;
                const QColor reference_pixel = reference.pixelColor(x, y);
                const QColor atlas_pixel     = atlas.pixelColor(x, y);
                const int delta = pixel_delta(reference_pixel, atlas_pixel);
                mask_stats.max_delta = std::max(mask_stats.max_delta, delta);
                if (delta != 0) {
                    ++mask_stats.diff_pixels;
                }
                if (logical_region.require_ink) {
                    if (pixel_delta(reference_pixel, logical_region.background) >
                        k_ink_delta_threshold)
                    {
                        ++mask_stats.reference_ink_pixels;
                    }
                    if (pixel_delta(atlas_pixel, logical_region.background) >
                        k_ink_delta_threshold)
                    {
                        ++mask_stats.atlas_ink_pixels;
                    }
                }
            }
        }
        stats.compared_pixels      += mask_stats.compared_pixels;
        stats.diff_pixels          += mask_stats.diff_pixels;
        stats.max_delta             = std::max(stats.max_delta, mask_stats.max_delta);
        stats.reference_ink_pixels += mask_stats.reference_ink_pixels;
        stats.atlas_ink_pixels     += mask_stats.atlas_ink_pixels;
        stats.perimeter_pixels     += mask_stats.perimeter_pixels;
        stats.masks.push_back(mask_stats);
    }
    return stats;
}

bool pump_pixel_atlas_surface(
    QGuiApplication&     app,
    QQuickWindow&        window,
    VNM_TerminalSurface& surface,
    Pixel_render_result& out)
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
        out.image_device_pixel_ratio =
            pixel_normalized_device_pixel_ratio(out.image.devicePixelRatio());

        out.atlas_report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (out.atlas_report.render_count > 0U && out.atlas_report.drew) {
            out.ready = true;
            return true;
        }
    }

    return false;
}

QRect logical_rect_to_pixels_for_dpr(const QRectF& rect, qreal dpr)
{
    const qreal normalized_dpr = std::max<qreal>(1.0, dpr);
    const int   left =
        static_cast<int>(std::round(rect.left() * normalized_dpr));
    const int top =
        static_cast<int>(std::round(rect.top() * normalized_dpr));
    const int right =
        static_cast<int>(std::round(rect.right() * normalized_dpr));
    const int bottom =
        static_cast<int>(std::round(rect.bottom() * normalized_dpr));
    return QRect(
        QPoint(left, top),
        QSize(std::max(0, right - left), std::max(0, bottom - top)));
}

double dense_grid_ink_weight(QColor pixel, QColor background)
{
    constexpr int k_ink_delta_threshold = 12;
    // LCD masks carry colored subpixel fringes whose channel maxima can move
    // horizontally by subpixel phase. Luminance keeps the dense-grid center
    // measurement tied to visible glyph energy instead of over-weighting one
    // fringe channel.
    const int     delta                 = std::abs(
        qGray(pixel.rgb()) - qGray(background.rgb()));
    return delta > k_ink_delta_threshold ? static_cast<double>(delta) : 0.0;
}

bool measure_dense_grid_cell(
    const QImage&                 image,
    QRect                         region,
    QColor                        background,
    Dense_grid_cell_measurement&  out)
{
    region = region.intersected(image.rect());
    double weight_sum = 0.0;
    double x_sum      = 0.0;
    double y_sum      = 0.0;
    for (int y = region.top(); y < region.top() + region.height(); ++y) {
        for (int x = region.left(); x < region.left() + region.width(); ++x) {
            const double weight =
                dense_grid_ink_weight(image.pixelColor(x, y), background);
            if (weight <= 0.0) {
                continue;
            }
            weight_sum += weight;
            x_sum      += weight * (static_cast<double>(x) + 0.5);
            y_sum      += weight * (static_cast<double>(y) + 0.5);
            out.left   = std::min(out.left, x);
            out.right  = std::max(out.right, x);
            out.top    = std::min(out.top, y);
            out.bottom = std::max(out.bottom, y);
        }
    }

    if (weight_sum <= 0.0) {
        return false;
    }

    out.x      = x_sum / weight_sum;
    out.y      = y_sum / weight_sum;
    out.weight = weight_sum;
    return true;
}

Dense_grid_axis_stats dense_grid_axis_stats(const std::vector<double>& centers)
{
    Dense_grid_axis_stats stats;
    for (std::size_t index = 1U; index < centers.size(); ++index) {
        const double delta = centers[index] - centers[index - 1U];
        stats.min_delta = std::min(stats.min_delta, delta);
        stats.max_delta = std::max(stats.max_delta, delta);
        ++stats.samples;
    }
    return stats;
}

Dense_grid_axis_stats dense_grid_residual_stats(
    const std::vector<double>& centers,
    double                     expected_delta)
{
    Dense_grid_axis_stats stats;
    if (centers.empty()) {
        return stats;
    }

    const double first_center = centers.front();
    for (std::size_t index = 0U; index < centers.size(); ++index) {
        const double expected =
            first_center + static_cast<double>(index) * expected_delta;
        const double residual = centers[index] - expected;
        stats.min_delta = std::min(stats.min_delta, residual);
        stats.max_delta = std::max(stats.max_delta, residual);
        ++stats.samples;
    }
    return stats;
}

bool check_dense_grid_spacing(
    const QImage&                  image,
    qreal                          dpr,
    term::terminal_cell_metrics_t  metrics,
    int                            rows,
    int                            columns,
    QColor                         background)
{
    std::vector<double> column_x(
        static_cast<std::size_t>(columns),
        0.0);
    std::vector<double> column_weight(
        static_cast<std::size_t>(columns),
        0.0);
    std::vector<double> row_y(
        static_cast<std::size_t>(rows),
        0.0);
    std::vector<double> row_weight(
        static_cast<std::size_t>(rows),
        0.0);
    std::vector<Dense_grid_cell_measurement> cells(
        static_cast<std::size_t>(rows * columns));

    bool ok = true;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const QRectF logical_cell = pixel_cell_rect(row, column, 1, metrics);
            const QRect  physical_cell =
                logical_rect_to_pixels_for_dpr(logical_cell, dpr);
            const std::size_t cell_index =
                static_cast<std::size_t>(row * columns + column);
            Dense_grid_cell_measurement& cell = cells[cell_index];
            if (!measure_dense_grid_cell(
                    image,
                    physical_cell,
                    background,
                    cell))
            {
                std::cerr << "FAIL: dense atlas X grid found no ink at row "
                    << row << " column " << column << '\n';
                ok = false;
                continue;
            }

            const std::size_t row_index    = static_cast<std::size_t>(row);
            const std::size_t column_index = static_cast<std::size_t>(column);
            column_x[column_index]      += cell.x * cell.weight;
            column_weight[column_index] += cell.weight;
            row_y[row_index]            += cell.y * cell.weight;
            row_weight[row_index]       += cell.weight;
        }
    }

    for (int column = 0; column < columns; ++column) {
        const std::size_t index = static_cast<std::size_t>(column);
        if (column_weight[index] <= 0.0) {
            ok = false;
            continue;
        }
        column_x[index] /= column_weight[index];
    }
    for (int row = 0; row < rows; ++row) {
        const std::size_t index = static_cast<std::size_t>(row);
        if (row_weight[index] <= 0.0) {
            ok = false;
            continue;
        }
        row_y[index] /= row_weight[index];
    }

    // Horizontal residuals are measured from luminance-weighted glyph centers,
    // so LCD subpixel fringes can shift the apparent center by more than a
    // grayscale AA edge while still preserving cell gutters. Keep the vertical
    // and gutter checks tight; they are not affected by RGB/BGR fringe phase.
    constexpr double k_horizontal_spacing_spread_tolerance = 1.5;
    constexpr double k_vertical_spacing_spread_tolerance   = 0.95;
    const Dense_grid_axis_stats x_stats = dense_grid_axis_stats(column_x);
    const Dense_grid_axis_stats y_stats = dense_grid_axis_stats(row_y);
    const Dense_grid_axis_stats x_residuals =
        dense_grid_residual_stats(column_x, metrics.width * dpr);
    const Dense_grid_axis_stats y_residuals =
        dense_grid_residual_stats(row_y, metrics.height * dpr);
    Dense_grid_axis_stats horizontal_gutters;
    Dense_grid_axis_stats vertical_gutters;
    for (int row = 0; row < rows; ++row) {
        for (int column = 1; column < columns; ++column) {
            const Dense_grid_cell_measurement& previous =
                cells[static_cast<std::size_t>(row * columns + column - 1)];
            const Dense_grid_cell_measurement& current =
                cells[static_cast<std::size_t>(row * columns + column)];
            if (!previous.has_ink() || !current.has_ink()) {
                continue;
            }

            const double gutter =
                static_cast<double>(current.left - previous.right - 1);
            horizontal_gutters.min_delta =
                std::min(horizontal_gutters.min_delta, gutter);
            horizontal_gutters.max_delta =
                std::max(horizontal_gutters.max_delta, gutter);
            ++horizontal_gutters.samples;
        }
    }
    for (int row = 1; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Dense_grid_cell_measurement& previous =
                cells[static_cast<std::size_t>((row - 1) * columns + column)];
            const Dense_grid_cell_measurement& current =
                cells[static_cast<std::size_t>(row * columns + column)];
            if (!previous.has_ink() || !current.has_ink()) {
                continue;
            }

            const double gutter =
                static_cast<double>(current.top - previous.bottom - 1);
            vertical_gutters.min_delta =
                std::min(vertical_gutters.min_delta, gutter);
            vertical_gutters.max_delta =
                std::max(vertical_gutters.max_delta, gutter);
            ++vertical_gutters.samples;
        }
    }
    constexpr double k_horizontal_gutter_spread_tolerance = 1.0;
    constexpr double k_vertical_gutter_spread_tolerance   = 1.0;
    if (x_residuals.samples == 0 ||
        x_residuals.spread() > k_horizontal_spacing_spread_tolerance)
    {
        std::cerr << "FAIL: dense atlas X grid horizontal spacing drift"
            << " dpr=" << dpr
            << " cell_width_px=" << metrics.width * dpr
            << " image_width=" << image.width()
            << " residual_spread_px=" << x_residuals.spread()
            << " delta_spread_px=" << x_stats.spread()
            << " min_delta_px=" << x_stats.min_delta
            << " max_delta_px=" << x_stats.max_delta
            << " samples=" << x_stats.samples << '\n';
        ok = false;
    }
    if (y_residuals.samples == 0 ||
        y_residuals.spread() > k_vertical_spacing_spread_tolerance)
    {
        std::cerr << "FAIL: dense atlas X grid vertical spacing drift"
            << " dpr=" << dpr
            << " cell_height_px=" << metrics.height * dpr
            << " image_height=" << image.height()
            << " residual_spread_px=" << y_residuals.spread()
            << " delta_spread_px=" << y_stats.spread()
            << " min_delta_px=" << y_stats.min_delta
            << " max_delta_px=" << y_stats.max_delta
            << " samples=" << y_stats.samples << '\n';
        ok = false;
    }
    if (horizontal_gutters.samples == 0 ||
        horizontal_gutters.spread() > k_horizontal_gutter_spread_tolerance)
    {
        std::cerr << "FAIL: dense atlas X grid horizontal ink gutters drift"
            << " dpr=" << dpr
            << " min_gutter_px=" << horizontal_gutters.min_delta
            << " max_gutter_px=" << horizontal_gutters.max_delta
            << " gutter_spread_px=" << horizontal_gutters.spread()
            << " samples=" << horizontal_gutters.samples << '\n';
        ok = false;
    }
    if (vertical_gutters.samples == 0 ||
        vertical_gutters.spread() > k_vertical_gutter_spread_tolerance)
    {
        std::cerr << "FAIL: dense atlas X grid vertical ink gutters drift"
            << " dpr=" << dpr
            << " min_gutter_px=" << vertical_gutters.min_delta
            << " max_gutter_px=" << vertical_gutters.max_delta
            << " gutter_spread_px=" << vertical_gutters.spread()
            << " samples=" << vertical_gutters.samples << '\n';
        ok = false;
    }

    if (ok) {
        std::cout << "PASS: dense atlas X grid spacing"
            << " dpr=" << dpr
            << " horizontal_residual_spread_px=" << x_residuals.spread()
            << " vertical_residual_spread_px=" << y_residuals.spread()
            << " horizontal_delta_spread_px=" << x_stats.spread()
            << " vertical_delta_spread_px=" << y_stats.spread()
            << " horizontal_gutter_spread_px=" << horizontal_gutters.spread()
            << " vertical_gutter_spread_px=" << vertical_gutters.spread()
            << " horizontal_delta_px=[" << x_stats.min_delta
            << ", " << x_stats.max_delta << "]"
            << " vertical_delta_px=[" << y_stats.min_delta
            << ", " << y_stats.max_delta << "]\n";
    }
    return ok;
}

Pixel_render_result render_pixel_reference_fixture(
    const Pixel_parity_fixture& fixture,
    std::optional<QSize>         physical_image_size = std::nullopt)
{
    Pixel_render_result result;
    result.device_pixel_ratio =
        pixel_normalized_device_pixel_ratio(fixture.device_pixel_ratio);
    result.image_device_pixel_ratio = result.device_pixel_ratio;
    result.image = QImage(
        physical_image_size.value_or(
            pixel_window_physical_pixel_size(
                fixture.logical_size,
                result.device_pixel_ratio)),
        QImage::Format_ARGB32_Premultiplied);
    result.image.setDevicePixelRatio(result.device_pixel_ratio);
    result.image.fill(QColor(1, 2, 3));

    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);

    paint_pixel_reference_rects(
        result.image,
        frame.background_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.image,
        frame.selection_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.image,
        frame.graphic_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_arcs(
        result.image,
        frame.graphic_arcs,
        result.device_pixel_ratio);

    {
        QPainter painter(&result.image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setRenderHint(QPainter::TextAntialiasing, false);
    paint_pixel_reference_text_runs(
        painter,
        frame.text_runs,
        font,
        fixture.cell_metrics,
        result.device_pixel_ratio);
    }

    paint_pixel_reference_decorations(
        result.image,
        frame.decorations,
        result.device_pixel_ratio);
    paint_pixel_reference_cursors(
        result.image,
        frame.cursors,
        result.device_pixel_ratio);
    {
        QPainter painter(&result.image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setRenderHint(QPainter::TextAntialiasing, false);
    paint_pixel_reference_text_runs(
        painter,
        frame.cursor_text_runs,
        font,
        fixture.cell_metrics,
        result.device_pixel_ratio);
    }
    paint_pixel_reference_rects(
        result.image,
        frame.overlay_rects,
        result.device_pixel_ratio);

    result.ready = !result.image.isNull();
    return result;
}

int atlas_reference_coverage_with_alpha(int coverage, int alpha)
{
    return std::clamp((coverage * alpha + 127) / 255, 0, 255);
}

int atlas_reference_blend_channel(
    int source,
    int destination,
    int coverage)
{
    return std::clamp(
        (source * coverage + destination * (255 - coverage) + 127) / 255,
        0,
        255);
}

term::Glyph_image_presentation atlas_reference_presentation_for_source_range(
    const QString& text,
    qsizetype      source_start,
    qsizetype      source_end)
{
    if (source_start < 0 ||
        source_end <= source_start ||
        source_start >= text.size())
    {
        return term::Glyph_image_presentation::TEXT;
    }

    const qsizetype bounded_end = std::min(source_end, text.size());
    const QByteArray utf8 = text
        .mid(source_start, bounded_end - source_start)
        .toUtf8();
    const term::Terminal_utf8_width_result width =
        term::measure_utf8_width(QByteArrayView(utf8.constData(), utf8.size()));
    const bool emoji_presentation = std::any_of(
        width.codepoints.begin(),
        width.codepoints.end(),
        [](const term::Terminal_codepoint_width& codepoint) {
            return
                codepoint.width_class ==
                    term::Terminal_unicode_width_class::EMOJI_PRESENTATION ||
                codepoint.presentation ==
                    term::Terminal_unicode_presentation::EMOJI;
        });
    return emoji_presentation
        ? term::Glyph_image_presentation::COLOR
        : term::Glyph_image_presentation::TEXT;
}

bool clip_atlas_reference_glyph(
    QRectF&       glyph_rect,
    QRectF&       source_rect,
    const QRectF& clip_rect)
{
    const QRectF clipped = glyph_rect.intersected(clip_rect);
    if (clipped.width() <= 0.0 || clipped.height() <= 0.0) {
        return false;
    }

    const qreal x_ratio = source_rect.width()  / glyph_rect.width();
    const qreal y_ratio = source_rect.height() / glyph_rect.height();
    source_rect.setLeft(
        source_rect.left() + (clipped.left() - glyph_rect.left()) * x_ratio);
    source_rect.setTop(
        source_rect.top() + (clipped.top() - glyph_rect.top()) * y_ratio);
    source_rect.setWidth(clipped.width() * x_ratio);
    source_rect.setHeight(clipped.height() * y_ratio);
    glyph_rect = clipped;
    return true;
}

void blend_atlas_reference_glyph_pixel(
    QImage&                      image,
    int                          x,
    int                          y,
    const term::Glyph_rgba_tile& tile,
    int                          tile_x,
    int                          tile_y,
    QColor                       foreground)
{
    if (!image.rect().contains(x, y) ||
        tile_x < 0 || tile_x >= tile.size.width() ||
        tile_y < 0 || tile_y >= tile.size.height())
    {
        return;
    }

    const uchar* const pixel =
        reinterpret_cast<const uchar*>(tile.bytes.constData()) +
        static_cast<std::ptrdiff_t>(tile_y * tile.bytes_per_line + tile_x * 4);
    const int inherited_alpha = std::clamp(foreground.alpha(), 0, 255);
    if (inherited_alpha <= 0) {
        return;
    }

    int source_red      = foreground.red();
    int source_green    = foreground.green();
    int source_blue     = foreground.blue();
    int coverage_red    = 0;
    int coverage_green  = 0;
    int coverage_blue   = 0;
    int coverage_alpha  = 0;
    switch (tile.coverage_kind) {
        case term::Glyph_coverage_kind::GRAYSCALE_MASK:
            coverage_alpha = atlas_reference_coverage_with_alpha(
                pixel[3],
                inherited_alpha);
            coverage_red   = coverage_alpha;
            coverage_green = coverage_alpha;
            coverage_blue  = coverage_alpha;
            break;

        case term::Glyph_coverage_kind::LCD_RGB_MASK:
        case term::Glyph_coverage_kind::LCD_BGR_MASK:
            coverage_red = atlas_reference_coverage_with_alpha(
                pixel[0],
                inherited_alpha);
            coverage_green = atlas_reference_coverage_with_alpha(
                pixel[1],
                inherited_alpha);
            coverage_blue = atlas_reference_coverage_with_alpha(
                pixel[2],
                inherited_alpha);
            coverage_alpha = atlas_reference_coverage_with_alpha(
                pixel[3],
                inherited_alpha);
            break;

        case term::Glyph_coverage_kind::COLOR_IMAGE:
            source_red      = pixel[0];
            source_green    = pixel[1];
            source_blue     = pixel[2];
            coverage_alpha  = atlas_reference_coverage_with_alpha(
                pixel[3],
                inherited_alpha);
            coverage_red    = coverage_alpha;
            coverage_green  = coverage_alpha;
            coverage_blue   = coverage_alpha;
            break;

        default:
            return;
    }

    if (coverage_red <= 0 &&
        coverage_green <= 0 &&
        coverage_blue <= 0 &&
        coverage_alpha <= 0)
    {
        return;
    }

    const QColor destination = image.pixelColor(x, y);
    image.setPixelColor(
        x,
        y,
        QColor(
            atlas_reference_blend_channel(
                source_red,
                destination.red(),
                coverage_red),
            atlas_reference_blend_channel(
                source_green,
                destination.green(),
                coverage_green),
            atlas_reference_blend_channel(
                source_blue,
                destination.blue(),
                coverage_blue),
            atlas_reference_blend_channel(
                255,
                destination.alpha(),
                coverage_alpha)));
}

void paint_atlas_rgba_reference_glyph(
    QImage&                                image,
    const term::Qsg_atlas_shaped_glyph_record& record,
    QRawFont&                              raster_font,
    const term::Terminal_render_text_run&  run,
    qreal                                  device_pixel_ratio)
{
    if (record.glyph_index == 0U ||
        record.glyph_bounds.width() <= 0.0 ||
        record.glyph_bounds.height() <= 0.0)
    {
        return;
    }

    const term::Glyph_image_presentation presentation =
        atlas_reference_presentation_for_source_range(
            run.text,
            record.source_string_start,
            record.source_string_end);
    const QRawFont::AntialiasingType antialiasing =
        presentation == term::Glyph_image_presentation::TEXT
            ? QRawFont::SubPixelAntialiasing
            : QRawFont::PixelAntialiasing;
    const QImage alpha_map = raster_font.alphaMapForGlyph(
        record.glyph_index,
        antialiasing);
    const term::Glyph_rgba_tile tile =
        term::qsg_atlas_rgba_tile_from_image(alpha_map, presentation);
    if (!tile.is_valid()) {
        return;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QPoint physical_offset =
        term::qsg_atlas_glyph_physical_offset_for_raster_font(
            raster_font,
            record.glyph_index,
            presentation);
    QRectF glyph_rect =
        term::qsg_atlas_snapped_glyph_draw_rect(
            record.glyph_origin,
            physical_offset,
            tile.size,
            dpr);
    QRectF source_rect(
        0.0,
        0.0,
        static_cast<qreal>(tile.size.width()),
        static_cast<qreal>(tile.size.height()));
    if (run.clip_rect.isValid() &&
        !clip_atlas_reference_glyph(glyph_rect, source_rect, run.clip_rect))
    {
        return;
    }

    const QRect destination_rect =
        logical_rect_to_pixels(glyph_rect, dpr).intersected(image.rect());
    for (int y = destination_rect.top(); y <= destination_rect.bottom(); ++y) {
        for (int x = destination_rect.left(); x <= destination_rect.right(); ++x) {
            const QPointF center(
                (static_cast<qreal>(x) + 0.5) / dpr,
                (static_cast<qreal>(y) + 0.5) / dpr);
            if (center.x() < glyph_rect.left() ||
                center.x() >= glyph_rect.right() ||
                center.y() < glyph_rect.top() ||
                center.y() >= glyph_rect.bottom())
            {
                continue;
            }

            const qreal source_x =
                source_rect.left() +
                (center.x() - glyph_rect.left()) *
                    source_rect.width() / glyph_rect.width();
            const qreal source_y =
                source_rect.top() +
                (center.y() - glyph_rect.top()) *
                    source_rect.height() / glyph_rect.height();
            blend_atlas_reference_glyph_pixel(
                image,
                x,
                y,
                tile,
                std::clamp(
                    static_cast<int>(std::floor(source_x)),
                    0,
                    tile.size.width() - 1),
                std::clamp(
                    static_cast<int>(std::floor(source_y)),
                    0,
                    tile.size.height() - 1),
                run.foreground);
        }
    }
}

void paint_atlas_rgba_reference_text_runs(
    QImage&                                         image,
    const std::vector<term::Terminal_render_text_run>& runs,
    const QFont&                                    font,
    term::terminal_cell_metrics_t                   cell_metrics,
    qreal                                           device_pixel_ratio,
    bool                                            cursor_text_runs)
{
    for (const term::Terminal_render_text_run& run : runs) {
        if (run.text.isEmpty()) {
            continue;
        }

        const term::Qsg_atlas_shaped_text_run_result shaped =
            term::qsg_atlas_shape_text_run(
                run,
                font,
                cell_metrics,
                device_pixel_ratio,
                0,
                cursor_text_runs);
        for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
            QRawFont raster_font = record.raw_font;
            raster_font.setPixelSize(record.physical_pixel_size);
            paint_atlas_rgba_reference_glyph(
                image,
                record,
                raster_font,
                run,
                device_pixel_ratio);
        }
    }
}

Pixel_render_result render_atlas_rgba_reference_fixture(
    const Pixel_parity_fixture& fixture,
    std::optional<QSize>         physical_image_size = std::nullopt)
{
    Pixel_render_result result;
    result.device_pixel_ratio =
        pixel_normalized_device_pixel_ratio(fixture.device_pixel_ratio);
    result.image_device_pixel_ratio = result.device_pixel_ratio;
    result.image = QImage(
        physical_image_size.value_or(
            pixel_window_physical_pixel_size(
                fixture.logical_size,
                result.device_pixel_ratio)),
        QImage::Format_ARGB32_Premultiplied);
    result.image.setDevicePixelRatio(result.device_pixel_ratio);
    result.image.fill(QColor(1, 2, 3));

    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);

    paint_pixel_reference_rects(
        result.image,
        frame.background_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.image,
        frame.selection_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.image,
        frame.graphic_rects,
        result.device_pixel_ratio);
    paint_pixel_reference_arcs(
        result.image,
        frame.graphic_arcs,
        result.device_pixel_ratio);

    paint_atlas_rgba_reference_text_runs(
        result.image,
        frame.text_runs,
        font,
        fixture.cell_metrics,
        result.device_pixel_ratio,
        false);

    paint_pixel_reference_decorations(
        result.image,
        frame.decorations,
        result.device_pixel_ratio);
    paint_pixel_reference_cursors(
        result.image,
        frame.cursors,
        result.device_pixel_ratio);
    paint_atlas_rgba_reference_text_runs(
        result.image,
        frame.cursor_text_runs,
        font,
        fixture.cell_metrics,
        result.device_pixel_ratio,
        true);
    paint_pixel_reference_rects(
        result.image,
        frame.overlay_rects,
        result.device_pixel_ratio);

    result.ready = !result.image.isNull();
    return result;
}

#if VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE
struct msdf_texel_sample_t
{
    float red   = 0.0f;
    float green = 0.0f;
    float blue  = 0.0f;
    float alpha = 0.0f;
};

msdf::options_t msdf_single_w_options()
{
    msdf::options_t options;
    options.atlas_size = k_msdf_single_w_atlas_texture_size;
    options.min_atlas_font_size = k_msdf_single_w_min_atlas_font_size;
    options.atlas_px_range = k_msdf_single_w_atlas_px_range;
    options.sharpness_bias = k_msdf_single_w_sharpness_bias;
    options.build_kerning_table = false;
    return options;
}

QByteArray read_msdf_terminal_font_data(QString& error)
{
    QFile font_file(QString::fromLatin1(k_terminal_monospace_font_resource));
    if (!font_file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("failed to open terminal embedded monospace font: %1")
            .arg(font_file.errorString());
        return {};
    }

    QByteArray data = font_file.readAll();
    if (data.isEmpty()) {
        error = QStringLiteral("terminal embedded monospace font resource is empty");
    }
    return data;
}

QRectF scaled_rect(QRectF rect, qreal scale)
{
    return QRectF(
        rect.left() * scale,
        rect.top() * scale,
        rect.width() * scale,
        rect.height() * scale);
}

QRectF msdf_glyph_plane_rect(const msdf::scaled_glyph_t& glyph)
{
    return QRectF(
        QPointF(glyph.plane_left, glyph.plane_bottom),
        QPointF(glyph.plane_right, glyph.plane_top))
        .normalized();
}

float msdf_median(float red, float green, float blue)
{
    return std::max(std::min(red, green), std::min(std::max(red, green), blue));
}

float msdf_texel_channel(
    const msdf::atlas_t& atlas,
    int                  x,
    int                  y,
    int                  channel)
{
    const int bounded_x = std::clamp(x, 0, atlas.atlas_size - 1);
    const int bounded_y = std::clamp(y, 0, atlas.atlas_size - 1);
    const std::size_t offset =
        (static_cast<std::size_t>(bounded_y) *
            static_cast<std::size_t>(atlas.atlas_size) +
            static_cast<std::size_t>(bounded_x)) *
            4U +
        static_cast<std::size_t>(channel);
    return static_cast<float>(atlas.rgba[offset]) / 255.0f;
}

float msdf_lerp(float left, float right, float ratio)
{
    return left + (right - left) * ratio;
}

msdf_texel_sample_t msdf_sample_atlas(
    const msdf::atlas_t& atlas,
    float                s,
    float                t)
{
    if (atlas.atlas_size <= 0 ||
        !std::isfinite(s) ||
        !std::isfinite(t))
    {
        return {};
    }

    const std::size_t required_bytes =
        static_cast<std::size_t>(atlas.atlas_size) *
        static_cast<std::size_t>(atlas.atlas_size) * 4U;
    if (atlas.rgba.size() < required_bytes) {
        return {};
    }

    const float max_coord = static_cast<float>(atlas.atlas_size - 1);
    const float sample_x = std::clamp(
        s * static_cast<float>(atlas.atlas_size) - 0.5f,
        0.0f,
        max_coord);
    const float sample_y = std::clamp(
        t * static_cast<float>(atlas.atlas_size) - 0.5f,
        0.0f,
        max_coord);
    const int x0 = static_cast<int>(std::floor(sample_x));
    const int y0 = static_cast<int>(std::floor(sample_y));
    const int x1 = std::min(x0 + 1, atlas.atlas_size - 1);
    const int y1 = std::min(y0 + 1, atlas.atlas_size - 1);
    const float x_ratio = sample_x - static_cast<float>(x0);
    const float y_ratio = sample_y - static_cast<float>(y0);

    auto bilinear = [&](int channel) {
        const float top = msdf_lerp(
            msdf_texel_channel(atlas, x0, y0, channel),
            msdf_texel_channel(atlas, x1, y0, channel),
            x_ratio);
        const float bottom = msdf_lerp(
            msdf_texel_channel(atlas, x0, y1, channel),
            msdf_texel_channel(atlas, x1, y1, channel),
            x_ratio);
        return msdf_lerp(top, bottom, y_ratio);
    };

    return {
        bilinear(0),
        bilinear(1),
        bilinear(2),
        bilinear(3),
    };
}

float msdf_glyph_alpha_from_texel(
    const msdf_texel_sample_t& texel,
    float                      px_range)
{
    const float distance =
        (msdf_median(texel.red, texel.green, texel.blue) - 0.5f) * px_range;
    float alpha = std::clamp(distance + 0.5f, 0.0f, 1.0f);
    if (texel.alpha > 0.0f) {
        const float sdf_distance = (texel.alpha - 0.5f) * px_range;
        alpha = std::min(alpha, std::clamp(sdf_distance + 0.5f, 0.0f, 1.0f));
    }
    return alpha;
}

void blend_msdf_glyph_pixel(
    QImage& image,
    int     x,
    int     y,
    QColor  foreground,
    float   alpha)
{
    if (alpha <= 0.0f) {
        return;
    }

    const int source_alpha = std::clamp(
        static_cast<int>(
            std::lround(
                static_cast<float>(foreground.alpha()) *
                    std::clamp(alpha, 0.0f, 1.0f))),
        0,
        255);
    foreground.setAlpha(source_alpha);
    blend_pixel_reference_pixel(image, x, y, foreground);
}

void paint_msdf_quad(
    QImage&                    image,
    const msdf::atlas_t&       atlas,
    int                        draw_pixel_height,
    const msdf::text_vertex_t& top_left,
    const msdf::text_vertex_t& bottom_right,
    QColor                     foreground,
    QRectF                     clip_rect,
    QRectF&                    draw_rect)
{
    if (atlas.atlas_size <= 0) {
        return;
    }
    const float px_range =
        msdf::px_range_for_pixel_height(atlas, draw_pixel_height);

    const float plane_left   = std::min(top_left.x, bottom_right.x);
    const float plane_right  = std::max(top_left.x, bottom_right.x);
    const float plane_top    = std::min(top_left.y, bottom_right.y);
    const float plane_bottom = std::max(top_left.y, bottom_right.y);
    const float left   = plane_left;
    const float top    = plane_top;
    const float right  = plane_right;
    const float bottom = plane_bottom;
    if (right <= left || bottom <= top) {
        return;
    }

    QRectF glyph_rect(
        QPointF(static_cast<qreal>(left), static_cast<qreal>(top)),
        QPointF(static_cast<qreal>(right), static_cast<qreal>(bottom)));
    if (clip_rect.isValid()) {
        glyph_rect = glyph_rect.intersected(clip_rect);
    }
    if (!glyph_rect.isValid()) {
        return;
    }

    const QRect pixels =
        logical_rect_to_pixels(glyph_rect, 1.0).intersected(image.rect());
    if (pixels.isEmpty()) {
        return;
    }

    const float atlas_size = static_cast<float>(atlas.atlas_size);
    const float half_texel = 0.5f / atlas_size;
    const float s_min = top_left.s_min + std::min(
        half_texel,
        (top_left.s_max - top_left.s_min) * 0.499f);
    const float s_max = top_left.s_max - std::min(
        half_texel,
        (top_left.s_max - top_left.s_min) * 0.499f);
    const float t_min = top_left.t_min + std::min(
        half_texel,
        (top_left.t_max - top_left.t_min) * 0.499f);
    const float t_max = top_left.t_max - std::min(
        half_texel,
        (top_left.t_max - top_left.t_min) * 0.499f);

    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const float sample_x = static_cast<float>(x) + 0.5f;
            const float sample_y = static_cast<float>(y) + 0.5f;
            const float x_ratio = std::clamp(
                (sample_x - left) / (right - left),
                0.0f,
                1.0f);
            const float y_ratio = std::clamp(
                (sample_y - top) / (bottom - top),
                0.0f,
                1.0f);
            const float s = std::clamp(
                msdf_lerp(top_left.s, bottom_right.s, x_ratio),
                s_min,
                s_max);
            const float t = std::clamp(
                msdf_lerp(top_left.t, bottom_right.t, y_ratio),
                t_min,
                t_max);
            const msdf_texel_sample_t texel = msdf_sample_atlas(atlas, s, t);
            blend_msdf_glyph_pixel(
                image,
                x,
                y,
                foreground,
                msdf_glyph_alpha_from_texel(texel, px_range));
        }
    }

    draw_rect = draw_rect.isValid()
        ? draw_rect.united(glyph_rect)
        : glyph_rect;
}

void paint_msdf_text_run(
    QImage&                              image,
    const term::Terminal_render_text_run& run,
    const msdf::atlas_t&                 atlas,
    int                                  draw_pixel_height,
    qreal                                device_pixel_ratio,
    QRectF&                              draw_rect)
{
    if (run.text.isEmpty()) {
        return;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QByteArray utf8 = run.text.toUtf8();
    std::vector<msdf::text_vertex_t> vertices;
    const int physical_origin_x = static_cast<int>(
        std::lround(run.baseline_origin.x() * dpr));
    const int physical_origin_y = static_cast<int>(
        std::lround(run.baseline_origin.y() * dpr));
    msdf::append_text_quads(
        atlas,
        draw_pixel_height,
        std::string_view(
            utf8.constData(),
            static_cast<std::size_t>(utf8.size())),
        static_cast<float>(physical_origin_x),
        static_cast<float>(physical_origin_y),
        vertices);
    if (vertices.size() < 4U) {
        return;
    }

    const QRectF clip_rect = run.clip_rect.isValid()
        ? scaled_rect(run.clip_rect, dpr)
        : QRectF();
    for (std::size_t base = 0; base + 3U < vertices.size(); base += 4U) {
        paint_msdf_quad(
            image,
            atlas,
            draw_pixel_height,
            vertices[base],
            vertices[base + 2U],
            run.foreground,
            clip_rect,
            draw_rect);
    }
}

void paint_msdf_text_runs(
    QImage&                                      image,
    const std::vector<term::Terminal_render_text_run>& runs,
    const msdf::atlas_t&                         atlas,
    int                                          draw_pixel_height,
    qreal                                        device_pixel_ratio,
    QRectF&                                      draw_rect)
{
    for (const term::Terminal_render_text_run& run : runs) {
        paint_msdf_text_run(
            image,
            run,
            atlas,
            draw_pixel_height,
            device_pixel_ratio,
            draw_rect);
    }
}
#endif

Msdf_single_w_probe_result render_msdf_single_w_probe_fixture(
    const Pixel_parity_fixture& fixture,
    std::optional<QSize>         physical_image_size = std::nullopt)
{
    Msdf_single_w_probe_result result;
    result.render.device_pixel_ratio =
        pixel_normalized_device_pixel_ratio(fixture.device_pixel_ratio);
    result.render.image_device_pixel_ratio = result.render.device_pixel_ratio;

#if VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE
    result.compiled = true;
    result.font_source =
        QString::fromLatin1(k_terminal_monospace_font_resource);

    QString font_error;
    const QByteArray font_data = read_msdf_terminal_font_data(font_error);
    result.font_data_bytes = static_cast<int>(font_data.size());
    if (!font_error.isEmpty()) {
        result.message = font_error;
        return result;
    }

    result.pixel_height = std::max(
        1,
        static_cast<int>(std::lround(
            fixture.cell_metrics.ascent * result.render.device_pixel_ratio)));

    const std::vector<char32_t> codepoints = {k_msdf_single_w_codepoint};
    const msdf::build_result_t build = msdf::build_font_atlas(
        reinterpret_cast<const std::uint8_t*>(font_data.constData()),
        static_cast<std::size_t>(font_data.size()),
        result.pixel_height,
        std::span<const char32_t>(codepoints.data(), codepoints.size()),
        msdf_single_w_options());
    result.atlas_built = build.status != msdf::Build_status::FAILURE;
    if (!result.atlas_built) {
        result.message = QString::fromStdString(build.message);
        return result;
    }

    const auto glyph = build.atlas.glyphs.find(k_msdf_single_w_codepoint);
    if (glyph == build.atlas.glyphs.end()) {
        result.message = QStringLiteral("MSDF atlas did not contain the W glyph");
        return result;
    }

    result.atlas_size         = build.atlas.atlas_size;
    result.px_range           =
        msdf::px_range_for_pixel_height(build.atlas, result.pixel_height);
    const msdf::scaled_glyph_t scaled_w =
        msdf::scaled_glyph(build.atlas, glyph->second, result.pixel_height);
    result.advance_x          = scaled_w.advance_x;
    result.baseline_offset_px =
        -msdf::scaled_font_metrics(build.atlas, result.pixel_height).descender;
    result.plane_rect         = msdf_glyph_plane_rect(scaled_w);
    result.render.image = QImage(
        physical_image_size.value_or(
            pixel_window_physical_pixel_size(
                fixture.logical_size,
                result.render.device_pixel_ratio)),
        QImage::Format_ARGB32_Premultiplied);
    result.render.image.setDevicePixelRatio(result.render.device_pixel_ratio);
    result.render.image.fill(QColor(1, 2, 3));

    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    paint_pixel_reference_rects(
        result.render.image,
        frame.background_rects,
        result.render.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.render.image,
        frame.selection_rects,
        result.render.device_pixel_ratio);
    paint_pixel_reference_rects(
        result.render.image,
        frame.graphic_rects,
        result.render.device_pixel_ratio);
    paint_pixel_reference_arcs(
        result.render.image,
        frame.graphic_arcs,
        result.render.device_pixel_ratio);
    paint_msdf_text_runs(
        result.render.image,
        frame.text_runs,
        build.atlas,
        result.pixel_height,
        result.render.device_pixel_ratio,
        result.draw_rect);
    paint_pixel_reference_decorations(
        result.render.image,
        frame.decorations,
        result.render.device_pixel_ratio);
    paint_pixel_reference_cursors(
        result.render.image,
        frame.cursors,
        result.render.device_pixel_ratio);
    paint_msdf_text_runs(
        result.render.image,
        frame.cursor_text_runs,
        build.atlas,
        result.pixel_height,
        result.render.device_pixel_ratio,
        result.draw_rect);
    paint_pixel_reference_rects(
        result.render.image,
        frame.overlay_rects,
        result.render.device_pixel_ratio);

    result.render.ready = !result.render.image.isNull() && result.draw_rect.isValid();
    result.message = result.render.ready
        ? QStringLiteral("ok")
        : QStringLiteral("MSDF diagnostic rendered no glyph ink");
#else
    Q_UNUSED(physical_image_size);
    result.message = QStringLiteral(
        "MSDF diagnostic not compiled; enable the qsg atlas MSDF text "
        "test dependency or define VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_DIAGNOSTIC "
        "and link "
        "vnm_msdf_text::vnm_msdf_text to enable it");
#endif

    return result;
}

Pixel_render_result render_pixel_atlas_fixture(
    QGuiApplication&              app,
    const Pixel_parity_fixture&  fixture,
    QPointF                      surface_position = QPointF(),
    qreal                        font_size = 18.0,
    QString                      font_family = QString())
{
    QQuickWindow window;
    window.setColor(QColor(1, 2, 3));
    const QSizeF window_logical_size(
        fixture.logical_size.width() + std::max<qreal>(0.0, surface_position.x()),
        fixture.logical_size.height() + std::max<qreal>(0.0, surface_position.y()));
    window.resize(pixel_window_logical_pixel_size(window_logical_size));

    QQuickItem host;
    host.setParentItem(window.contentItem());
    host.setPosition(surface_position);
    host.setSize(fixture.logical_size);

    VNM_TerminalSurface surface;
    surface.setParentItem(surface_position.isNull()
        ? window.contentItem()
        : &host);
    surface.setSize(fixture.logical_size);
    surface.set_font_family(std::move(font_family));
    surface.set_font_size(font_size);
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
    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);
    Pixel_render_result result;
    result.device_pixel_ratio   = pixel_window_device_pixel_ratio(window);
    result.window_logical_size  = window.size();
    result.graphics_api         = window.rendererInterface() != nullptr
        ? window.rendererInterface()->graphicsApi()
        : QSGRendererInterface::Unknown;
    result.software_renderer    =
        result.graphics_api == QSGRendererInterface::Software;
    pump_pixel_atlas_surface(app, window, surface, result);
    result.device_pixel_ratio  = pixel_window_device_pixel_ratio(window);
    result.window_logical_size = window.size();
    result.graphics_api        = window.rendererInterface() != nullptr
        ? window.rendererInterface()->graphicsApi()
        : result.graphics_api;
    result.software_renderer   =
        result.graphics_api == QSGRendererInterface::Software;
    capture_window_driver_info(window, result);
    window.hide();
    app.processEvents(QEventLoop::AllEvents, 50);
    return result;
}

bool pump_qsg_text_reference_item(
    QGuiApplication&          app,
    QQuickWindow&             window,
    Qsg_text_reference_item&  item,
    Pixel_render_result&      out)
{
    for (int attempt = 0; attempt < 120; ++attempt) {
        item.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out.image = window.grabWindow();
        if (out.image.isNull()) {
            continue;
        }

        out.image_device_pixel_ratio =
            pixel_normalized_device_pixel_ratio(out.image.devicePixelRatio());
        if (item.text_node_available()) {
            out.ready = true;
            return true;
        }
    }

    return false;
}

Pixel_render_result render_qsg_text_reference_fixture(
    QGuiApplication&             app,
    const Pixel_parity_fixture&  fixture)
{
    QQuickWindow window;
    window.setColor(QColor(1, 2, 3));
    window.resize(pixel_window_logical_pixel_size(fixture.logical_size));

    Qsg_text_reference_item item(fixture);
    item.setParentItem(window.contentItem());

    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);

    Pixel_render_result result;
    result.device_pixel_ratio  = pixel_window_device_pixel_ratio(window);
    result.window_logical_size = window.size();
    result.graphics_api        = window.rendererInterface() != nullptr
        ? window.rendererInterface()->graphicsApi()
        : QSGRendererInterface::Unknown;
    result.software_renderer =
        result.graphics_api == QSGRendererInterface::Software;
    pump_qsg_text_reference_item(app, window, item, result);
    result.device_pixel_ratio  = pixel_window_device_pixel_ratio(window);
    result.window_logical_size = window.size();
    result.graphics_api        = window.rendererInterface() != nullptr
        ? window.rendererInterface()->graphicsApi()
        : result.graphics_api;
    result.software_renderer =
        result.graphics_api == QSGRendererInterface::Software;
    capture_window_driver_info(window, result);
    window.hide();
    app.processEvents(QEventLoop::AllEvents, 50);
    return result;
}

bool compare_pixel_fixture(
    const Pixel_parity_fixture&    fixture,
    const Pixel_render_result&     reference,
    const Pixel_render_result&     atlas,
    const char*                    backend,
    std::optional<Pixel_aa_budget> budget_override = std::nullopt,
    const char*                    parity_mode_override = nullptr,
    bool                           compare_glyph_regions_enabled = true)
{
    const char* const parity_mode = parity_mode_override != nullptr
        ? parity_mode_override
        : (fixture.layout_parity ? "atlas layout parity" : "atlas pixel parity");
    if (!reference.ready || !atlas.ready) {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " did not render both backends\n";
        return false;
    }
    if (reference.image.size() != atlas.image.size()) {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " rendered different image sizes"
            << " reference=" << reference.image.width() << 'x'
            << reference.image.height()
            << " atlas=" << atlas.image.width() << 'x'
            << atlas.image.height()
            << " reference_render_dpr=" << reference.device_pixel_ratio
            << " atlas_render_dpr=" << atlas.device_pixel_ratio
            << '\n';
        return false;
    }
    const QSize expected_image_size = pixel_window_physical_pixel_size(
        fixture.logical_size,
        fixture.device_pixel_ratio);
    if (reference.image.width()  < expected_image_size.width()  ||
        reference.image.height() < expected_image_size.height() ||
        atlas.image.width()      < expected_image_size.width()  ||
        atlas.image.height()     < expected_image_size.height())
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " captured an image smaller than the DPR-snapped fixture surface"
            << " fixture_surface=" << expected_image_size.width() << 'x'
            << expected_image_size.height()
            << " reference=" << reference.image.width() << 'x'
            << reference.image.height()
            << " atlas=" << atlas.image.width() << 'x'
            << atlas.image.height()
            << " fixture_dpr=" << fixture.device_pixel_ratio
            << " reference_render_dpr=" << reference.device_pixel_ratio
            << " atlas_render_dpr=" << atlas.device_pixel_ratio
            << " reference_image_dpr=" << reference.image_device_pixel_ratio
            << " atlas_image_dpr=" << atlas.image_device_pixel_ratio
            << '\n';
        return false;
    }
    if (!pixel_device_pixel_ratios_match(
            fixture.device_pixel_ratio,
            reference.device_pixel_ratio) ||
        !pixel_device_pixel_ratios_match(
            fixture.device_pixel_ratio,
            atlas.device_pixel_ratio))
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " rendered non-comparable DPRs"
            << " fixture_dpr=" << fixture.device_pixel_ratio
            << " reference_render_dpr=" << reference.device_pixel_ratio
            << " atlas_render_dpr=" << atlas.device_pixel_ratio
            << " reference_image_dpr=" << reference.image_device_pixel_ratio
            << " atlas_image_dpr=" << atlas.image_device_pixel_ratio
            << '\n';
        return false;
    }
    if (!pixel_device_pixel_ratios_match(
            fixture.device_pixel_ratio,
            reference.image_device_pixel_ratio) ||
        !pixel_device_pixel_ratios_match(
            fixture.device_pixel_ratio,
            atlas.image_device_pixel_ratio))
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " captured non-comparable image DPR metadata"
            << " fixture_dpr=" << fixture.device_pixel_ratio
            << " reference_render_dpr=" << reference.device_pixel_ratio
            << " atlas_render_dpr=" << atlas.device_pixel_ratio
            << " reference_image_dpr=" << reference.image_device_pixel_ratio
            << " atlas_image_dpr=" << atlas.image_device_pixel_ratio
            << " physical_size=" << reference.image.width() << 'x'
            << reference.image.height()
            << '\n';
        return false;
    }

    const qreal render_dpr = fixture.device_pixel_ratio;
    bool ok = true;
    int exact_compared_pixels = 0;
    int exact_diff_pixels     = 0;
    int exact_max_delta       = 0;
    for (const Pixel_exact_mask_class& exact_class : fixture.exact_mask_classes) {
        const Pixel_diff_stats exact = compare_regions(
            reference.image,
            atlas.image,
            exact_class.masks,
            render_dpr);
        exact_compared_pixels += exact.compared_pixels;
        exact_diff_pixels     += exact.diff_pixels;
        exact_max_delta        = std::max(exact_max_delta, exact.max_delta);
        if (exact.compared_pixels == 0) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " exact class " << exact_class.name
                << " compared zero pixels\n";
            ok = false;
        }
        if (exact.max_delta > exact_class.max_delta) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " exact class " << exact_class.name
                << " changed " << exact.diff_pixels
                << " / " << exact.compared_pixels
                << " pixels; max delta " << exact.max_delta
                << " / allowed " << exact_class.max_delta << '\n';
            QPoint position;
            QColor reference_pixel;
            QColor atlas_pixel;
            if (first_region_diff(
                    reference.image,
                    atlas.image,
                    exact_class.masks,
                    render_dpr,
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

    ok &= compare_decorative_primitives(
        fixture,
        reference.image,
        atlas.image,
        render_dpr,
        parity_mode);

    const Pixel_aa_budget budget =
        budget_override.value_or(pixel_budget_for_backend(backend, fixture.name));
    Pixel_glyph_stats glyphs;
    int budget_diff_pixels = 0;
    if (compare_glyph_regions_enabled) {
        glyphs = compare_glyph_regions(
            reference.image,
            atlas.image,
            fixture.glyph_masks,
            render_dpr);
        budget_diff_pixels = scaled_pixel_budget_count(
            budget.diff_pixels,
            budget,
            glyphs.compared_pixels);
    }
    const int glyph_perimeter_pixels = glyphs.perimeter_pixels;
    int max_mask_ink_delta_pixels = 0;
    int max_mask_ink_budget_pixels = 0;
    int ink_checked_masks = 0;
    if (compare_glyph_regions_enabled &&
        !fixture.glyph_masks.empty()  &&
        budget.base_compared_pixels <= 0)
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " has glyph masks but no DPR-aware budget on " << backend
            << '\n';
        ok = false;
    }
    if (compare_glyph_regions_enabled &&
        !fixture.glyph_masks.empty()  &&
        glyphs.compared_pixels == 0)
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " glyph masks compared zero pixels"
            << " render_dpr=" << render_dpr << '\n';
        ok = false;
    }
    if (compare_glyph_regions_enabled &&
        glyphs.masks.size() != fixture.glyph_masks.size())
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " glyph mask stats count mismatch"
            << " masks=" << fixture.glyph_masks.size()
            << " stats=" << glyphs.masks.size()
            << " render_dpr=" << render_dpr << '\n';
        ok = false;
    }
    if (compare_glyph_regions_enabled &&
        !fixture.glyph_masks.empty() &&
        (glyphs.max_delta > budget.max_delta ||
            glyphs.diff_pixels > budget_diff_pixels))
    {
        std::cerr << "FAIL: " << parity_mode << " fixture "
            << fixture.name
            << " glyph budget exceeded on " << backend
            << ": diff pixels " << glyphs.diff_pixels
            << " / " << budget_diff_pixels
            << ", max delta " << glyphs.max_delta
            << " / " << budget.max_delta
            << ", compared pixels " << glyphs.compared_pixels
            << " / base " << budget.base_compared_pixels
            << ", render_dpr=" << render_dpr << '\n';
        ok = false;
    }
    for (std::size_t mask_index = 0U;
        compare_glyph_regions_enabled &&
            mask_index < fixture.glyph_masks.size() &&
            mask_index < glyphs.masks.size();
        ++mask_index)
    {
        const Pixel_glyph_mask& mask = fixture.glyph_masks[mask_index];
        if (!mask.require_ink) {
            continue;
        }

        const Pixel_glyph_mask_stats& mask_stats = glyphs.masks[mask_index];
        if (mask_stats.compared_pixels == 0) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " glyph mask " << mask_index
                << " compared zero pixels on " << backend
                << ", perimeter pixels " << mask_stats.perimeter_pixels
                << ", render_dpr=" << render_dpr << '\n';
            ok = false;
        }
        const int mask_ink_delta_pixels =
            std::abs(
                mask_stats.atlas_ink_pixels -
                    mask_stats.reference_ink_pixels);
        const int mask_ink_budget_pixels = static_cast<int>(std::ceil(
            static_cast<double>(mask_stats.perimeter_pixels) *
                budget.ink_delta_perimeter_factor));
        max_mask_ink_delta_pixels =
            std::max(max_mask_ink_delta_pixels, mask_ink_delta_pixels);
        max_mask_ink_budget_pixels =
            std::max(max_mask_ink_budget_pixels, mask_ink_budget_pixels);
        ++ink_checked_masks;

        if (mask_stats.reference_ink_pixels == 0) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " glyph mask " << mask_index
                << " reference produced zero ink on " << backend
                << ", compared pixels " << mask_stats.compared_pixels
                << ", perimeter pixels " << mask_stats.perimeter_pixels
                << ", render_dpr=" << render_dpr << '\n';
            ok = false;
        }
        if (mask_stats.atlas_ink_pixels == 0) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " glyph mask " << mask_index
                << " atlas produced zero ink on " << backend
                << ", compared pixels " << mask_stats.compared_pixels
                << ", perimeter pixels " << mask_stats.perimeter_pixels
                << ", render_dpr=" << render_dpr << '\n';
            ok = false;
        }
        if (mask_ink_delta_pixels > mask_ink_budget_pixels) {
            std::cerr << "FAIL: " << parity_mode << " fixture "
                << fixture.name
                << " glyph mask " << mask_index
                << " ink budget exceeded on " << backend
                << ": ink delta pixels " << mask_ink_delta_pixels
                << " / " << mask_ink_budget_pixels
                << ", atlas ink pixels " << mask_stats.atlas_ink_pixels
                << ", reference ink pixels "
                << mask_stats.reference_ink_pixels
                << ", perimeter pixels " << mask_stats.perimeter_pixels
                << ", perimeter factor "
                << budget.ink_delta_perimeter_factor
                << ", compared pixels " << mask_stats.compared_pixels
                << ", render_dpr=" << render_dpr << '\n';
            ok = false;
        }
    }

    if (ok) {
        std::cout << "PASS: " << parity_mode << ' ' << fixture.name
            << " backend=" << backend
            << " render_dpr=" << render_dpr
            << " image_dpr=[" << reference.image_device_pixel_ratio
            << ", " << atlas.image_device_pixel_ratio << "]"
            << " exact_diff_pixels=" << exact_diff_pixels
            << " exact_compared_pixels=" << exact_compared_pixels
            << " exact_max_delta=" << exact_max_delta
            << " decorative_primitives="
            << fixture.decorative_primitives.size()
            << " glyph_diff_pixels=" << glyphs.diff_pixels
            << " glyph_compared_pixels=" << glyphs.compared_pixels
            << " glyph_max_delta=" << glyphs.max_delta
            << " glyph_budget_pixels=" << budget_diff_pixels
            << " glyph_budget_max_delta=" << budget.max_delta
            << " glyph_reference_ink_pixels=" << glyphs.reference_ink_pixels
            << " glyph_atlas_ink_pixels=" << glyphs.atlas_ink_pixels
            << " glyph_ink_checked_masks=" << ink_checked_masks
            << " glyph_max_mask_ink_delta_pixels="
            << max_mask_ink_delta_pixels
            << " glyph_max_mask_ink_budget_pixels="
            << max_mask_ink_budget_pixels
            << " glyph_perimeter_pixels=" << glyph_perimeter_pixels
            << " glyph_ink_perimeter_factor="
            << budget.ink_delta_perimeter_factor
            << " glyph_budget_base_compared_pixels="
            << budget.base_compared_pixels << '\n';
    }
    return ok;
}

int test_pixel_parity(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas pixel parity");
    if (backend_status != 0) {
        return backend_status;
    }

    bool ok = true;
    const qreal device_pixel_ratio =
        pixel_probe_render_window_device_pixel_ratio(app);
    const std::vector<Pixel_parity_fixture> fixtures =
        make_pixel_parity_fixtures(device_pixel_ratio);
    for (const Pixel_parity_fixture& fixture : fixtures) {
        const Pixel_render_result atlas =
            render_pixel_atlas_fixture(app, fixture);
        if (!atlas.ready &&
            (atlas.atlas_report.prepare_count == 0U ||
                atlas.atlas_report.render_count == 0U ||
                (atlas.atlas_report.prepare_count > 0U && !atlas.atlas_report.rhi_non_null)))
        {
            std::cerr << "SKIP: atlas pixel parity did not reach usable QRhi render state\n";
            return k_unsupported_backend_skip_return_code;
        }

        const Pixel_render_result reference =
            render_atlas_rgba_reference_fixture(fixture, atlas.image.size());
        ok &= compare_pixel_fixture(fixture, reference, atlas, backend);
    }
    return ok ? 0 : 1;
}

bool check_frame_build_report(
    bool                         condition,
    const Pixel_parity_fixture& fixture,
    const char*                  message)
{
    if (!condition) {
        std::cerr << "FAIL: atlas layout report fixture " << fixture.name
            << ": " << message << '\n';
        return false;
    }

    return true;
}

bool validate_frame_build_report(
    const Pixel_parity_fixture&                 fixture,
    const term::Qsg_atlas_frame_report&  report)
{
    const term::Qsg_atlas_frame_build_summary& frame_build = report.frame_build;
    bool ok = true;
    ok &= check_frame_build_report(
        frame_build.rect_instances + frame_build.glyph_instances > 0,
        fixture,
        "atlas emitted at least one render instance");
    ok &= check_frame_build_report(
        frame_build.glyph_missed_instances == 0 &&
            frame_build.glyph_coverage_failures == 0 &&
            frame_build.glyph_atlas_insert_failures == 0,
        fixture,
        "all expected glyphs reached atlas instances without silent misses");
    ok &= check_frame_build_report(
        frame_build.snapped_origin_failures == 0,
        fixture,
        "all glyph instances arrived with pre-snapped physical origins");

    if (fixture.name == "graphics_supported_unsupported_blocks") {
        ok &= check_frame_build_report(
            frame_build.frame_graphic_rects > 0 &&
                frame_build.frame_graphic_arcs == 0,
            fixture,
            "block and shade elements produced graphic rectangles");
        ok &= check_frame_build_report(
            frame_build.frame_text_runs == 0 && frame_build.glyph_instances == 0,
            fixture,
            "block and shade elements stayed off shaped glyph instances");
    }
    else
    if (fixture.name == "box_arcs") {
        ok &= check_frame_build_report(
            frame_build.frame_graphic_rects == 0 &&
                frame_build.frame_graphic_arcs == 4,
            fixture,
            "rounded box corners produced graphic arcs");
        ok &= check_frame_build_report(
            frame_build.frame_text_runs == 0 && frame_build.glyph_instances == 0,
            fixture,
            "rounded box corners stayed off shaped glyph instances");
    }
    else
    if (fixture.name == "block_cursor_antialiased_box" ||
        fixture.name == "block_cursor_antialiased_vertical_box") {
        ok &= check_frame_build_report(
            frame_build.frame_graphic_rects == 0 &&
                frame_build.glyph_instances > 0,
            fixture,
            "block cursor over box drawing used shaped glyph cursor text");
    }
    else
    if (fixture.name == "viewport_scrollback_selection") {
        ok &= check_frame_build_report(
            frame_build.viewport_active_buffer == term::Terminal_buffer_id::PRIMARY &&
                frame_build.viewport_scrollback_rows == 8 &&
                frame_build.viewport_offset_from_tail == 2,
            fixture,
            "primary scrollback viewport metadata was preserved");
        ok &= check_frame_build_report(
            frame_build.selection_provenance_valid && frame_build.frame_selection_rects == 1,
            fixture,
            "valid provenance allowed the selection span to render");
        ok &= check_frame_build_report(
            frame_build.first_text_logical_row == 6 &&
                frame_build.first_text_retained_line_id == 900U &&
                frame_build.first_text_content_generation == 77U,
            fixture,
            "text row identity follows visible-line provenance");
    }
    else
    if (fixture.name == "alternate_viewport") {
        ok &= check_frame_build_report(
            frame_build.viewport_active_buffer == term::Terminal_buffer_id::ALTERNATE,
            fixture,
            "alternate-screen viewport metadata was preserved");
    }
    else
    if (fixture.name == "public_projection_scroll_full_repaint") {
        ok &= check_frame_build_report(
            frame_build.snapshot_basis ==
                term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
                frame_build.snapshot_purpose ==
                    term::Terminal_render_snapshot_purpose::SCROLL,
            fixture,
            "PUBLIC_PROJECTION/SCROLL snapshot metadata was recorded");
        ok &= check_frame_build_report(
            frame_build.full_dirty_range                 &&
                frame_build.public_projection_full_repaint &&
                frame_build.scroll_full_repaint            &&
                frame_build.full_repaint_fallback,
            fixture,
            "public scroll snapshot used the full-repaint fallback");
    }
    else
    if (fixture.name == "selection_provenance_suppressed") {
        ok &= check_frame_build_report(
            !frame_build.selection_provenance_valid &&
                frame_build.frame_selection_rects == 0,
            fixture,
            "selection was suppressed when visible-line provenance was unavailable");
    }
    else
    if (fixture.name == "fallback_fonts") {
        ok &= check_frame_build_report(
            frame_build.fallback_glyph_faces >= 1,
            fixture,
            "fallback font face ids were observed in shaped glyph runs");
        if (report.render.msdf_text_renderer_active &&
            report.render.msdf_text_glyph_instances > 0)
        {
            ok &= check_frame_build_report(
                frame_build.distinct_glyph_faces >= 1,
                fixture,
                "fallback text produced shaped glyph face records");
        }
        else {
            ok &= check_frame_build_report(
                frame_build.distinct_glyph_faces >= 2,
                fixture,
                "base-font ASCII and fallback text produced distinct glyph faces");
            ok &= check_frame_build_report(
                frame_build.fallback_glyph_faces < frame_build.distinct_glyph_faces,
                fixture,
                "base-font face remains distinct from fallback faces");
        }
    }
    else
    if (fixture.name == "emoji_policy") {
        ok &= check_frame_build_report(
            frame_build.emoji_presentation_runs > 0,
            fixture,
            "emoji presentation text runs were identified");
        ok &= check_frame_build_report(
            frame_build.glyph_instances > 0,
            fixture,
            "emoji policy rendered through monochrome atlas glyph instances");
    }

    if (ok) {
        std::cout << "PASS: atlas layout report " << fixture.name
            << " selection_rects=" << frame_build.frame_selection_rects
            << " distinct_faces=" << frame_build.distinct_glyph_faces
            << " fallback_faces=" << frame_build.fallback_glyph_faces
            << " emoji_runs=" << frame_build.emoji_presentation_runs
            << " glyph_misses=" << frame_build.glyph_missed_instances
            << " rect_instances=" << frame_build.rect_instances
            << " glyph_instances=" << frame_build.glyph_instances << '\n';
    }
    return ok;
}

int test_layout_parity(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas layout parity");
    if (backend_status != 0) {
        return backend_status;
    }

    bool ok = true;
    const qreal device_pixel_ratio =
        pixel_probe_render_window_device_pixel_ratio(app);
    const std::vector<Pixel_parity_fixture> fixtures =
        make_layout_parity_fixtures(device_pixel_ratio);
    for (const Pixel_parity_fixture& fixture : fixtures) {
        const Pixel_render_result atlas =
            render_pixel_atlas_fixture(app, fixture);
        if (!atlas.ready &&
            (atlas.atlas_report.prepare_count == 0U ||
                atlas.atlas_report.render_count == 0U ||
                (atlas.atlas_report.prepare_count > 0U && !atlas.atlas_report.rhi_non_null)))
        {
            std::cerr << "SKIP: atlas layout parity did not reach usable QRhi render state\n";
            return k_unsupported_backend_skip_return_code;
        }

        const Pixel_render_result reference =
            render_atlas_rgba_reference_fixture(fixture, atlas.image.size());
        ok &= compare_pixel_fixture(fixture, reference, atlas, backend);
        ok &= validate_frame_build_report(fixture, atlas.atlas_report);
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
    ok &= check(read_source_file(VNM_TERMINAL_QSG_ATLAS_SOURCE_PATH, atlas_source),
        "atlas source path is readable");
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
            std::cerr << "FAIL: atlas render-node source contains forbidden token "
                << token.constData() << '\n';
            ok = false;
        }
    }

    ok &= check(atlas_source.contains(QByteArrayLiteral("void prepare() override")),
        "atlas source defines QSGRenderNode::prepare");
    ok &= check(atlas_source.contains(QByteArrayLiteral("void render(const RenderState* state) override")),
        "atlas source defines QSGRenderNode::render");
    ok &= check(atlas_source.contains(QByteArrayLiteral("commandBuffer()")) &&
            atlas_source.contains(QByteArrayLiteral("renderTarget()")),
        "atlas prepare/render uses QSGRenderNode render-thread accessors");
    ok &= check(atlas_source.contains(QByteArrayLiteral("m_frame")),
        "atlas prepare/render consumes the captured frame value");
    const qsizetype prepare_pos = atlas_source.indexOf(
        QByteArrayLiteral("void prepare() override"));
    const qsizetype render_pos = atlas_source.indexOf(
        QByteArrayLiteral("void render(const RenderState* state) override"));
    const QByteArray prepare_atlas_instances_call =
        QByteArrayLiteral("prepare_atlas_instances(rhi)");
    const qsizetype prepare_instances_call_pos = atlas_source.indexOf(
        prepare_atlas_instances_call,
        prepare_pos);
    const qsizetype prepare_instances_def_pos = atlas_source.indexOf(
        QByteArrayLiteral("Atlas_prepare_result prepare_atlas_instances("));
    const qsizetype frame_build_pos = atlas_source.indexOf(
        QByteArrayLiteral("build_terminal_render_frame("));
    ok &= check(
        prepare_instances_call_pos > prepare_pos &&
            prepare_instances_call_pos < render_pos,
        "atlas prepare calls atlas instance preparation");
    ok &= check(frame_build_pos > prepare_instances_def_pos,
        "atlas render frame is built from the captured value in prepare helpers");
    ok &= check(atlas_source.contains(QByteArrayLiteral("bool has_glyph_draw_passes() const")) &&
            atlas_source.contains(
                QByteArrayLiteral("!has_glyph_draw_passes() ||")) &&
            atlas_source.contains(
                QByteArrayLiteral("ensure_glyph_resources(rhi, target)")),
        "atlas prepare gates glyph resources on render glyph pass state");
    ok &= check(!atlas_source.contains(
            QByteArrayLiteral("m_glyph_instances.empty() || ensure_glyph_resources")),
        "atlas prepare does not gate glyph resources on logical glyph instances");
    ok &= check(
        atlas_source.contains(QByteArrayLiteral(
            "record_msdf_text_resource_failure(prepare_result)")) &&
            atlas_source.contains(QByteArrayLiteral(
                "m_msdf_text_resource_failure_disabled = true")),
        "atlas prepare retries with shaped glyph fallback after MSDF resource failure");
    const QByteArray msdf_prepare_retry_decision =
        QByteArrayLiteral(
            "qsg_atlas_should_retry_msdf_text_fallback_after_prepare(");
    ok &= check(
        atlas_source.indexOf(msdf_prepare_retry_decision, prepare_pos) >
                prepare_pos &&
            atlas_source.indexOf(msdf_prepare_retry_decision, prepare_pos) <
                render_pos &&
            atlas_source.contains(QByteArrayLiteral(
                "msdf_prepare_resource_attempted = true")) &&
            atlas_source.contains(QByteArrayLiteral(
                "msdf_prepare_resource_failed = !msdf_ready")),
        "atlas prepare retries only after an attempted MSDF prepare resource failure");
    ok &= check(
        atlas_source.contains(QByteArrayLiteral(
            "return !qsg_atlas_driver_info_is_known_software_renderer(rhi->driverInfo())")),
        "MSDF ownership rejects known software drivers without hardware or vendor denylisting");
    const QByteArray msdf_buffer_retry_decision =
        QByteArrayLiteral(
            "qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(");
    ok &= check(
        atlas_source.indexOf(msdf_buffer_retry_decision, prepare_pos) >
                prepare_pos &&
            atlas_source.indexOf(msdf_buffer_retry_decision, prepare_pos) <
                render_pos,
        "atlas prepare uses the typed MSDF buffer fallback retry decision");
    ok &= check(
        atlas_source.contains(QByteArrayLiteral("while (!buffers_ready)")) &&
            atlas_source.contains(QByteArrayLiteral(
                "std::optional<Qsg_atlas_render_summary> msdf_failure_render")) &&
            atlas_source.contains(QByteArrayLiteral(
                "qsg_atlas_merge_msdf_text_failure_diagnostics(")) &&
            atlas_source.contains(QByteArrayLiteral(
                "retry_without_msdf_text_ownership();")),
        "atlas buffer update retry loop preserves MSDF diagnostics across fallback retries");
    ok &= check(
        !atlas_source.contains(QByteArrayLiteral(
            "QByteArray qsg_layer_descriptor_key(")) &&
            !atlas_source.contains(QByteArrayLiteral("keys.qsg_layers")),
        "atlas source does not build unused qsg layer descriptor keys");
    ok &= check(
        atlas_source.contains(QByteArrayLiteral(
            "const bool msdf_text_buffer_needed = has_msdf_text_draw_passes()")) &&
            atlas_source.contains(QByteArrayLiteral(
                "if (msdf_text_buffer_needed)")),
        "atlas buffer updates do not allocate MSDF instance resources without MSDF draw passes");
    ok &= check(
        atlas_source.contains(QByteArrayLiteral(
            "!m_msdf_text_resources_ready")) &&
            atlas_source.contains(QByteArrayLiteral(
                "void draw_msdf_text_pass(")),
        "atlas render suppresses MSDF draws when MSDF resources are not ready");
    ok &= check(!surface_source.contains(QByteArrayLiteral("build_terminal_render_frame(")),
        "surface updatePaintNode does not build the render frame");

    const QByteArray capture_call =
        QByteArrayLiteral("capture_qsg_atlas_frame(");
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
    const qreal physical_pixel_size =
        term::qsg_atlas_physical_pixel_size(font, 1.0);
    const term::Glyph_atlas_cache_key base_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        physical_pixel_size,
        0);
    const term::Glyph_atlas_cache_key hidpi_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        term::qsg_atlas_physical_pixel_size(font, 2.0),
        0);
    const term::Glyph_atlas_cache_key fallback_key = term::qsg_atlas_cache_key(
        65U,
        QStringLiteral("fallback-face"),
        physical_pixel_size,
        0);
    const term::Glyph_atlas_cache_key subpixel_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        physical_pixel_size,
        9);
    const term::Glyph_atlas_cache_key lcd_rgb_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        physical_pixel_size,
        0,
        term::Glyph_coverage_kind::LCD_RGB_MASK);
    const term::Glyph_atlas_cache_key lcd_bgr_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        physical_pixel_size,
        0,
        term::Glyph_coverage_kind::LCD_BGR_MASK);
    const term::Glyph_atlas_cache_key color_key = term::qsg_atlas_cache_key(
        65U,
        face_id,
        physical_pixel_size,
        0,
        term::Glyph_coverage_kind::COLOR_IMAGE);
    const term::Glyph_atlas_cache_key color_grayscale_key =
        term::qsg_atlas_cache_key(
            65U,
            face_id,
            physical_pixel_size,
            0,
            term::Glyph_coverage_kind::GRAYSCALE_MASK,
            term::Glyph_image_presentation::COLOR);
    const term::Glyph_atlas_cache_key color_presentation_key =
        term::qsg_atlas_cache_key(
            65U,
            face_id,
            physical_pixel_size,
            0,
            term::Glyph_coverage_kind::COLOR_IMAGE,
            term::Glyph_image_presentation::COLOR);
    const qreal raw_physical_pixel_size =
        term::qsg_atlas_physical_pixel_size(raw_font, 2.0);

    const term::Glyph_rgba_tile cache_tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4});
    const term::Glyph_rgba_tile lcd_rgb_tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10, 11, 12, 12},
        term::Glyph_coverage_kind::LCD_RGB_MASK);
    const term::Glyph_rgba_tile lcd_bgr_tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {3, 2, 1, 3, 6, 5, 4, 6, 9, 8, 7, 9, 12, 11, 10, 12},
        term::Glyph_coverage_kind::LCD_BGR_MASK);
    const term::Glyph_rgba_tile color_tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {20, 30, 40, 50, 60, 70, 80, 90, 21, 31, 41, 51, 61, 71, 81, 91},
        term::Glyph_coverage_kind::COLOR_IMAGE);
    term::Glyph_atlas_cache model_cache(QSize(16, 16));
    model_cache.set_epoch(1U);
    const term::Glyph_atlas_slot cached_grayscale =
        model_cache.insert_or_get(base_key, cache_tile);
    const term::Glyph_atlas_slot cached_lcd_rgb =
        model_cache.insert_or_get(lcd_rgb_key, lcd_rgb_tile);
    const term::Glyph_atlas_slot cached_lcd_bgr =
        model_cache.insert_or_get(lcd_bgr_key, lcd_bgr_tile);
    const term::Glyph_atlas_slot cached_color =
        model_cache.insert_or_get(color_key, color_tile);
    const term::Glyph_atlas_slot cached_color_grayscale =
        model_cache.insert_or_get(color_grayscale_key, cache_tile);
    const term::Glyph_atlas_slot cached_color_presentation =
        model_cache.insert_or_get(color_presentation_key, color_tile);
    const term::Glyph_atlas_cache_stats model_cache_stats =
        model_cache.stats();

    bool ok = true;
    ok &= check(!face_id.isEmpty(),
        "raw-font face id is populated for cache keys");
    ok &= check(!(base_key == hidpi_key),
        "glyph cache key includes physical pixel size");
    ok &= check(!(base_key == fallback_key),
        "glyph cache key includes fallback face id");
    ok &= check(!(base_key == subpixel_key),
        "glyph cache key includes subpixel phase bucket");
    ok &= check(base_key.coverage_kind == term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            base_key.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            color_key.coverage_kind == term::Glyph_coverage_kind::COLOR_IMAGE &&
            color_key.lcd_order == term::Glyph_lcd_order::UNKNOWN,
        "glyph cache key keeps non-LCD coverage keys on unknown LCD order");
    ok &= check(base_key.presentation == term::Glyph_image_presentation::TEXT &&
            color_grayscale_key.presentation ==
                term::Glyph_image_presentation::COLOR &&
            color_key.presentation == term::Glyph_image_presentation::TEXT &&
            color_presentation_key.presentation ==
                term::Glyph_image_presentation::COLOR &&
            !(base_key == color_grayscale_key) &&
            !(color_key == color_presentation_key),
        "glyph cache key includes presentation-specific rasterization mode");
    ok &= check(lcd_rgb_key.coverage_kind == term::Glyph_coverage_kind::LCD_RGB_MASK &&
            lcd_rgb_key.lcd_order == term::Glyph_lcd_order::RGB &&
            lcd_bgr_key.coverage_kind == term::Glyph_coverage_kind::LCD_BGR_MASK &&
            lcd_bgr_key.lcd_order == term::Glyph_lcd_order::BGR,
        "glyph cache key infers LCD order from LCD coverage kind");
    ok &= check(!(base_key == lcd_rgb_key) &&
            !(base_key == lcd_bgr_key) &&
            !(base_key == color_key) &&
            !(lcd_rgb_key == lcd_bgr_key) &&
            !(lcd_rgb_key == color_key) &&
            !(lcd_bgr_key == color_key) &&
            !(color_grayscale_key == color_presentation_key),
        "glyph cache key does not alias identical glyph phases across coverage models");
    ok &= check(cached_grayscale.is_valid() &&
            cached_lcd_rgb.is_valid() &&
            cached_lcd_bgr.is_valid() &&
            cached_color.is_valid() &&
            cached_color_grayscale.is_valid() &&
            cached_color_presentation.is_valid() &&
            model_cache_stats.inserts == 6U,
        "atlas cache stores distinct coverage and presentation variants independently");
    ok &= check(std::abs(raw_physical_pixel_size - raw_font.pixelSize() * 2.0) < 0.001,
        "raw-font physical pixel size is derived from the run font");
    return ok;
}

QRhiDriverInfo test_driver_info(
    QByteArray                  device_name,
    QRhiDriverInfo::DeviceType  device_type)
{
    QRhiDriverInfo driver_info;
    driver_info.deviceName = std::move(device_name);
    driver_info.deviceType = device_type;
    return driver_info;
}

bool test_software_renderer_driver_classifier()
{
    bool ok = true;
    ok &= check(term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("llvmpipe (LLVM 19.1.7, 256 bits)"),
                QRhiDriverInfo::UnknownDevice)),
        "llvmpipe driver name is classified as software rendering");
    ok &= check(term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("Google SwiftShader"),
                QRhiDriverInfo::UnknownDevice)),
        "SwiftShader driver name is classified as software rendering");
    ok &= check(term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("Microsoft Basic Render Driver"),
                QRhiDriverInfo::UnknownDevice)),
        "Microsoft Basic Render Driver name is classified as software rendering");
    ok &= check(term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(QByteArray(), QRhiDriverInfo::CpuDevice)),
        "CPU QRhi device type is classified as software rendering");
    ok &= check(term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("llvmpipe (LLVM 19.1.7, 256 bits)"),
                QRhiDriverInfo::VirtualDevice)),
        "virtual llvmpipe device is classified as software rendering");

    ok &= check(!term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(QByteArray(), QRhiDriverInfo::VirtualDevice)),
        "unnamed virtual QRhi device is not classified as software rendering");
    ok &= check(!term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("Mesa virgl"),
                QRhiDriverInfo::VirtualDevice)),
        "virtual accelerated OpenGL device is not classified as software rendering");
    ok &= check(!term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("NVIDIA GeForce RTX 4070"),
                QRhiDriverInfo::DiscreteDevice)),
        "discrete NVIDIA device is not classified as software rendering");
    ok &= check(!term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("AMD Radeon RX 7800 XT"),
                QRhiDriverInfo::DiscreteDevice)),
        "discrete AMD device is not classified as software rendering");
    ok &= check(!term::qsg_atlas_driver_info_is_known_software_renderer(
            test_driver_info(
                QByteArrayLiteral("Mesa Intel(R) Arc Graphics"),
                QRhiDriverInfo::IntegratedDevice)),
        "Mesa Intel hardware OpenGL device is not classified as software rendering");
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

    const term::Glyph_rgba_tile tile = test_rgba_tile(
        QSize(2, 2),
        16,
        {
            1, 2, 3, 4, 5, 6, 7, 8, 90, 91, 92, 93, 94, 95, 96, 97,
            9, 10, 11, 12, 13, 14, 15, 16, 80, 81, 82, 83, 84, 85, 86, 87,
        });

    term::Glyph_atlas_cache cache(QSize(8, 8));
    cache.set_epoch(1U);
    const term::Glyph_atlas_cache_key key =
        term::qsg_atlas_cache_key(1U, QStringLiteral("face"), 12.0, 0);
    const term::Glyph_atlas_slot slot = cache.insert_or_get(key, tile);
    const QByteArray& page = cache.page_bytes(slot.page);
    const int stride = cache.stats().page_size.width() * 4;
    const int first_offset = slot.rect.y() * stride + slot.rect.x() * 4;
    const int second_row_offset = (slot.rect.y() + 1) * stride + slot.rect.x() * 4;
    ok &= check(page.mid(first_offset, 8) ==
            byte_array({1, 2, 3, 4, 5, 6, 7, 8}) &&
            page.mid(second_row_offset, 8) ==
            byte_array({9, 10, 11, 12, 13, 14, 15, 16}),
        "atlas cache copies RGBA rows using tile stride");

    term::Glyph_atlas_cache capped_cache(QSize(4, 4));
    capped_cache.set_epoch(1U);
    const term::Glyph_rgba_tile capped_tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4});
    const term::Glyph_atlas_slot capped_first = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(2U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    const term::Glyph_atlas_slot capped_second = capped_cache.insert_or_get(
        term::qsg_atlas_cache_key(3U, QStringLiteral("face"), 12.0, 0),
        capped_tile);
    ok &= check(capped_first.is_valid() &&
            capped_second.is_valid() &&
            capped_first.page == 0 &&
            capped_second.page == 1 &&
            capped_cache.stats().page_count == 2,
        "atlas cache allocates additional RGBA texture-array pages");
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

    QImage argb(3, 2, QImage::Format_ARGB32);
    argb.setPixelColor(0, 0, QColor(10, 20, 30, 11));
    argb.setPixelColor(1, 0, QColor(10, 20, 30, 12));
    argb.setPixelColor(2, 0, QColor(10, 20, 30, 13));
    argb.setPixelColor(0, 1, QColor(10, 20, 30, 21));
    argb.setPixelColor(1, 1, QColor(10, 20, 30, 22));
    argb.setPixelColor(2, 1, QColor(10, 20, 30, 23));
    const term::Glyph_coverage_tile argb_tile =
        term::qsg_atlas_coverage_tile_from_image(argb);

    QImage rgb(3, 2, QImage::Format_RGB32);
    rgb.fill(QColor(10, 20, 30));
    const term::Glyph_coverage_tile rgb_tile =
        term::qsg_atlas_coverage_tile_from_image(rgb);

    bool ok = true;
    ok &= check(indexed_tile.is_valid() &&
            indexed_tile.bytes_per_line == 3 &&
            indexed_tile.bytes == byte_array({10, 20, 30, 40, 50, 60}),
        "Format_Indexed8 glyph alpha map converts to tight coverage rows");
    ok &= check(gray_tile.is_valid() &&
            gray_tile.bytes_per_line == 3 &&
            gray_tile.bytes == byte_array({7, 8, 9, 17, 18, 19}),
        "Format_Grayscale8 glyph alpha map conversion honors source stride");
    ok &= check(!argb_tile.is_valid(),
        "alpha-bearing color glyph maps are rejected by the legacy coverage helper");
    ok &= check(!rgb_tile.is_valid(),
        "RGB subpixel glyph maps are rejected by the legacy coverage helper");
    return ok;
}

bool test_rgba_tile_model_preparation()
{
    uchar gray_storage[16] = {
        4, 5, 6, 200, 201, 202, 203, 204,
        7, 8, 9, 205, 206, 207, 208, 209,
    };
    QImage gray(
        gray_storage,
        3,
        2,
        8,
        QImage::Format_Grayscale8);
    const term::Glyph_rgba_tile gray_tile =
        term::qsg_atlas_rgba_tile_from_image(gray);

    uchar alpha_storage[16] = {
        31, 32, 33, 210, 211, 212, 213, 214,
        41, 42, 43, 215, 216, 217, 218, 219,
    };
    QImage alpha(
        alpha_storage,
        3,
        2,
        8,
        QImage::Format_Alpha8);
    const term::Glyph_rgba_tile alpha_tile =
        term::qsg_atlas_rgba_tile_from_image(alpha);

    QImage lcd_rgb(2, 1, QImage::Format_RGB32);
    lcd_rgb.setPixelColor(0, 0, QColor(10, 30, 20));
    lcd_rgb.setPixelColor(1, 0, QColor(4, 5, 14));
    const term::Glyph_rgba_tile lcd_rgb_tile =
        term::qsg_atlas_rgba_tile_from_image(
            lcd_rgb,
            term::Glyph_image_presentation::TEXT);

    QImage lcd_rgb888(2, 1, QImage::Format_RGB888);
    lcd_rgb888.setPixelColor(0, 0, QColor(20, 70, 30));
    lcd_rgb888.setPixelColor(1, 0, QColor(9, 11, 21));
    const term::Glyph_rgba_tile lcd_rgb888_tile =
        term::qsg_atlas_rgba_tile_from_image(
            lcd_rgb888,
            term::Glyph_image_presentation::TEXT);

    QImage lcd_bgr(2, 1, QImage::Format_BGR888);
    lcd_bgr.setPixelColor(0, 0, QColor(40, 10, 24));
    lcd_bgr.setPixelColor(1, 0, QColor(5, 45, 25));
    const term::Glyph_rgba_tile lcd_bgr_tile =
        term::qsg_atlas_rgba_tile_from_image(
            lcd_bgr,
            term::Glyph_image_presentation::TEXT);
    const term::Glyph_rgba_tile forced_gray_lcd_rgb_tile =
        term::qsg_atlas_rgba_tile_from_image(
            lcd_rgb,
            term::Glyph_image_presentation::TEXT,
            term::Glyph_coverage_kind::GRAYSCALE_MASK);
    const term::Glyph_rgba_tile forced_gray_lcd_bgr_tile =
        term::qsg_atlas_rgba_tile_from_image(
            lcd_bgr,
            term::Glyph_image_presentation::TEXT,
            term::Glyph_coverage_kind::GRAYSCALE_MASK);

    QImage color(2, 1, QImage::Format_ARGB32);
    color.setPixelColor(0, 0, QColor(11, 22, 33, 44));
    color.setPixelColor(1, 0, QColor(50, 60, 70, 80));
    const term::Glyph_rgba_tile color_tile =
        term::qsg_atlas_rgba_tile_from_image(
            color,
            term::Glyph_image_presentation::COLOR);

    QImage rgba(2, 1, QImage::Format_RGBA8888);
    rgba.setPixelColor(0, 0, QColor(1, 2, 3, 4));
    rgba.setPixelColor(1, 0, QColor(210, 120, 40, 90));
    const term::Glyph_rgba_tile rgba_tile =
        term::qsg_atlas_rgba_tile_from_image(
            rgba,
            term::Glyph_image_presentation::COLOR);

    QImage premultiplied_argb(2, 1, QImage::Format_ARGB32_Premultiplied);
    premultiplied_argb.setPixelColor(0, 0, QColor(255, 0, 0, 128));
    premultiplied_argb.setPixelColor(1, 0, QColor(0, 255, 0, 64));
    const term::Glyph_rgba_tile premultiplied_argb_tile =
        term::qsg_atlas_rgba_tile_from_image(
            premultiplied_argb,
            term::Glyph_image_presentation::COLOR);

    QImage premultiplied_rgba(2, 1, QImage::Format_RGBA8888_Premultiplied);
    premultiplied_rgba.setPixelColor(0, 0, QColor(0, 0, 255, 200));
    premultiplied_rgba.setPixelColor(1, 0, QColor(255, 255, 0, 180));
    const term::Glyph_rgba_tile premultiplied_rgba_tile =
        term::qsg_atlas_rgba_tile_from_image(
            premultiplied_rgba,
            term::Glyph_image_presentation::COLOR);

    QImage neutral_color(2, 1, QImage::Format_RGB32);
    neutral_color.setPixelColor(0, 0, QColor(90, 90, 90));
    neutral_color.setPixelColor(1, 0, QColor(12, 12, 12));
    const term::Glyph_rgba_tile neutral_color_tile =
        term::qsg_atlas_rgba_tile_from_image(
            neutral_color,
            term::Glyph_image_presentation::COLOR);

    QImage neutral_text(2, 1, QImage::Format_RGB32);
    neutral_text.setPixelColor(0, 0, QColor(90, 92, 93));
    neutral_text.setPixelColor(1, 0, QColor(12, 14, 15));
    const term::Glyph_rgba_tile neutral_text_tile =
        term::qsg_atlas_rgba_tile_from_image(
            neutral_text,
            term::Glyph_image_presentation::TEXT);

    QImage ambiguous(2, 1, QImage::Format_RGB16);
    ambiguous.fill(QColor(31, 47, 63));
    const term::Glyph_rgba_tile ambiguous_tile =
        term::qsg_atlas_rgba_tile_from_image(ambiguous);

    bool ok = true;
    ok &= check(gray_tile.is_valid() &&
            gray_tile.coverage_kind == term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            gray_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            gray_tile.source_format == QImage::Format_Grayscale8 &&
            gray_tile.size == QSize(3, 2) &&
            gray_tile.bytes_per_line == 12 &&
            gray_tile.bytes == byte_array({
                4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
                7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
            }),
        "grayscale glyph image prepares tight RGBA coverage rows");
    ok &= check(alpha_tile.is_valid() &&
            alpha_tile.coverage_kind == term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            alpha_tile.source_format == QImage::Format_Alpha8 &&
            alpha_tile.bytes_per_line == 12 &&
            alpha_tile.bytes == byte_array({
                31, 31, 31, 31, 32, 32, 32, 32, 33, 33, 33, 33,
                41, 41, 41, 41, 42, 42, 42, 42, 43, 43, 43, 43,
            }),
        "Alpha8 glyph image prepares replicated RGBA coverage rows");
    ok &= check(lcd_rgb_tile.is_valid() &&
            lcd_rgb_tile.coverage_kind == term::Glyph_coverage_kind::LCD_RGB_MASK &&
            lcd_rgb_tile.lcd_order == term::Glyph_lcd_order::RGB &&
            lcd_rgb_tile.source_format == QImage::Format_RGB32 &&
            lcd_rgb_tile.size == QSize(2, 1) &&
            lcd_rgb_tile.bytes_per_line == 8 &&
            lcd_rgb_tile.bytes == byte_array({10, 30, 20, 30, 4, 5, 14, 14}),
        "RGB32 text glyph image prepares LCD RGB rows");
    ok &= check(lcd_rgb888_tile.is_valid() &&
            lcd_rgb888_tile.coverage_kind == term::Glyph_coverage_kind::LCD_RGB_MASK &&
            lcd_rgb888_tile.lcd_order == term::Glyph_lcd_order::RGB &&
            lcd_rgb888_tile.source_format == QImage::Format_RGB888 &&
            lcd_rgb888_tile.bytes_per_line == 8 &&
            lcd_rgb888_tile.bytes == byte_array({20, 70, 30, 70, 9, 11, 21, 21}),
        "RGB888 text glyph image prepares LCD RGB rows");
    ok &= check(lcd_bgr_tile.is_valid() &&
            lcd_bgr_tile.coverage_kind == term::Glyph_coverage_kind::LCD_BGR_MASK &&
            lcd_bgr_tile.lcd_order == term::Glyph_lcd_order::BGR &&
            lcd_bgr_tile.source_format == QImage::Format_BGR888 &&
            lcd_bgr_tile.size == QSize(2, 1) &&
            lcd_bgr_tile.bytes_per_line == 8 &&
            lcd_bgr_tile.bytes == byte_array({40, 10, 24, 40, 5, 45, 25, 45}),
        "BGR888 text glyph image prepares LCD BGR rows");
    ok &= check(forced_gray_lcd_rgb_tile.is_valid() &&
            forced_gray_lcd_rgb_tile.coverage_kind ==
                term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            forced_gray_lcd_rgb_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            forced_gray_lcd_rgb_tile.source_format == QImage::Format_RGB32 &&
            forced_gray_lcd_rgb_tile.bytes == byte_array({21, 21, 21, 21, 6, 6, 6, 6}),
        "requested grayscale glyph image demotes LCD RGB coverage");
    ok &= check(forced_gray_lcd_bgr_tile.is_valid() &&
            forced_gray_lcd_bgr_tile.coverage_kind ==
                term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            forced_gray_lcd_bgr_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            forced_gray_lcd_bgr_tile.source_format == QImage::Format_BGR888 &&
            forced_gray_lcd_bgr_tile.bytes == byte_array({22, 22, 22, 22, 28, 28, 28, 28}),
        "requested grayscale glyph image demotes LCD BGR coverage");
    ok &= check(color_tile.is_valid() &&
            color_tile.coverage_kind == term::Glyph_coverage_kind::COLOR_IMAGE &&
            color_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            color_tile.source_format == QImage::Format_ARGB32 &&
            color_tile.size == QSize(2, 1) &&
            color_tile.bytes_per_line == 8 &&
            color_tile.bytes == byte_array({11, 22, 33, 44, 50, 60, 70, 80}),
        "ARGB color glyph image prepares color RGBA rows");
    ok &= check(rgba_tile.is_valid() &&
            rgba_tile.coverage_kind == term::Glyph_coverage_kind::COLOR_IMAGE &&
            rgba_tile.source_format == QImage::Format_RGBA8888 &&
            rgba_tile.bytes_per_line == 8 &&
            rgba_tile.bytes == byte_array({1, 2, 3, 4, 210, 120, 40, 90}),
        "RGBA8888 color glyph image prepares straight RGBA rows");
    ok &= check(premultiplied_argb_tile.is_valid() &&
            premultiplied_argb_tile.coverage_kind ==
                term::Glyph_coverage_kind::COLOR_IMAGE &&
            premultiplied_argb_tile.source_format ==
                QImage::Format_ARGB32_Premultiplied &&
            premultiplied_argb_tile.bytes_per_line == 8 &&
            premultiplied_argb_tile.bytes == byte_array({
                255, 0, 0, 128, 0, 255, 0, 64,
            }),
        "ARGB premultiplied color glyph image normalizes to straight RGBA rows");
    ok &= check(premultiplied_rgba_tile.is_valid() &&
            premultiplied_rgba_tile.coverage_kind ==
                term::Glyph_coverage_kind::COLOR_IMAGE &&
            premultiplied_rgba_tile.source_format ==
                QImage::Format_RGBA8888_Premultiplied &&
            premultiplied_rgba_tile.bytes_per_line == 8 &&
            premultiplied_rgba_tile.bytes == byte_array({
                0, 0, 255, 200, 255, 255, 0, 180,
            }),
        "RGBA premultiplied color glyph image normalizes to straight RGBA rows");
    ok &= check(neutral_color_tile.is_valid() &&
            neutral_color_tile.coverage_kind ==
                term::Glyph_coverage_kind::COLOR_IMAGE &&
            neutral_color_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            neutral_color_tile.source_format == QImage::Format_RGB32 &&
            neutral_color_tile.size == QSize(2, 1) &&
            neutral_color_tile.bytes_per_line == 8 &&
            neutral_color_tile.bytes == byte_array({
                90, 90, 90, 255, 12, 12, 12, 255,
            }),
        "neutral RGB color glyph image preserves explicit color presentation");
    ok &= check(neutral_text_tile.is_valid() &&
            neutral_text_tile.coverage_kind ==
                term::Glyph_coverage_kind::GRAYSCALE_MASK &&
            neutral_text_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            neutral_text_tile.source_format == QImage::Format_RGB32 &&
            neutral_text_tile.size == QSize(2, 1) &&
            neutral_text_tile.bytes_per_line == 8 &&
            neutral_text_tile.bytes == byte_array({
                91, 91, 91, 91, 13, 13, 13, 13,
            }),
        "neutral RGB text glyph image prepares qGray-replicated coverage rows");
    ok &= check(!ambiguous_tile.is_valid() &&
            ambiguous_tile.coverage_kind == term::Glyph_coverage_kind::AMBIGUOUS &&
            ambiguous_tile.lcd_order == term::Glyph_lcd_order::UNKNOWN &&
            ambiguous_tile.source_format == QImage::Format_RGB16 &&
            ambiguous_tile.size == QSize(2, 1) &&
            ambiguous_tile.bytes_per_line == 0 &&
            ambiguous_tile.bytes.isEmpty(),
        "ambiguous RGB16 glyph image preserves metadata without RGBA bytes");
    return ok;
}

bool test_glyph_coverage_candidate_classifier()
{
    const QImage null_image;
    QImage grayscale(2, 2, QImage::Format_Grayscale8);
    grayscale.fill(128);

    QImage neutral_rgb(2, 2, QImage::Format_RGB32);
    neutral_rgb.fill(QColor(90, 90, 90));

    QImage lcd_rgb(2, 2, QImage::Format_RGB32);
    lcd_rgb.fill(QColor(220, 20, 80));

    QImage threshold_rgb(2, 2, QImage::Format_RGB32);
    threshold_rgb.fill(QColor(90, 93, 90));

    QImage edge_rgb(2, 2, QImage::Format_RGB32);
    edge_rgb.fill(QColor(90, 90, 90));
    edge_rgb.setPixelColor(1, 1, QColor(90, 94, 90));

    QImage neutral_bgr(2, 2, QImage::Format_BGR888);
    neutral_bgr.fill(QColor(100, 100, 100));

    QImage lcd_bgr(2, 2, QImage::Format_BGR888);
    lcd_bgr.fill(QColor(20, 120, 220));

    QImage color_alpha(2, 2, QImage::Format_ARGB32);
    color_alpha.fill(QColor(10, 20, 30, 180));

    QImage ambiguous(2, 2, QImage::Format_RGB16);
    ambiguous.fill(QColor(40, 40, 40));

    QImage unsupported(2, 2, QImage::Format_Mono);
    unsupported.fill(1);

    bool ok = true;
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(null_image) ==
            term::Glyph_coverage_kind::UNKNOWN,
        "null glyph image candidate is reported as unknown");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(grayscale) ==
            term::Glyph_coverage_kind::GRAYSCALE_MASK,
        "grayscale glyph image candidate is classified as a grayscale mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(neutral_rgb) ==
            term::Glyph_coverage_kind::GRAYSCALE_MASK,
        "neutral RGB glyph image candidate is classified as a grayscale mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                neutral_rgb,
                term::Glyph_image_presentation::COLOR) ==
            term::Glyph_coverage_kind::COLOR_IMAGE,
        "neutral RGB color glyph image candidate honors color presentation");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(lcd_rgb) ==
            term::Glyph_coverage_kind::AMBIGUOUS,
        "unknown RGB channel-varying glyph image candidate is ambiguous");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                lcd_rgb,
                term::Glyph_image_presentation::TEXT) ==
            term::Glyph_coverage_kind::LCD_RGB_MASK,
        "text RGB channel-varying glyph image candidate is an LCD RGB mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                lcd_rgb,
                term::Glyph_image_presentation::COLOR) ==
            term::Glyph_coverage_kind::COLOR_IMAGE,
        "color RGB channel-varying glyph image candidate is a color image");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                threshold_rgb,
                term::Glyph_image_presentation::TEXT) ==
            term::Glyph_coverage_kind::GRAYSCALE_MASK,
        "RGB glyph image candidate at the variation threshold is grayscale");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                edge_rgb,
                term::Glyph_image_presentation::TEXT) ==
            term::Glyph_coverage_kind::LCD_RGB_MASK,
        "RGB glyph image candidate with one colored edge pixel is an LCD RGB mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(neutral_bgr) ==
            term::Glyph_coverage_kind::GRAYSCALE_MASK,
        "neutral BGR glyph image candidate is classified as a grayscale mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(
                lcd_bgr,
                term::Glyph_image_presentation::TEXT) ==
            term::Glyph_coverage_kind::LCD_BGR_MASK,
        "text BGR channel-varying glyph image candidate is an LCD BGR mask");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(color_alpha) ==
            term::Glyph_coverage_kind::COLOR_IMAGE,
        "alpha-bearing color glyph image candidate is classified as a color image");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(ambiguous) ==
            term::Glyph_coverage_kind::AMBIGUOUS,
        "packed RGB glyph image candidate is classified as ambiguous");
    ok &= check(term::qsg_atlas_classify_glyph_image_candidate(unsupported) ==
            term::Glyph_coverage_kind::UNSUPPORTED,
        "unsupported glyph image candidate format is classified as unsupported");
    return ok;
}

bool test_glyph_image_diagnostics()
{
    term::Qsg_atlas_shaped_glyph_record record;
    record.text_run_index      = 12;
    record.glyph_run_index     = 3;
    record.glyph_index_in_run  = 4;
    record.source_string_start = 5;
    record.source_string_end   = 7;
    record.glyph_index         = 1234U;
    record.fallback_face_id    = QStringLiteral("diagnostic-face");

    QImage ambiguous(2, 3, QImage::Format_RGB16);
    ambiguous.fill(QColor(10, 20, 30));
    const term::Qsg_atlas_glyph_image_diagnostic ambiguous_diagnostic =
        term::qsg_atlas_glyph_image_diagnostic_from_record(
            record,
            ambiguous,
            term::Glyph_image_presentation::UNKNOWN);

    QImage unsupported(3, 2, QImage::Format_Mono);
    unsupported.fill(1);
    const term::Qsg_atlas_glyph_image_diagnostic unsupported_diagnostic =
        term::qsg_atlas_glyph_image_diagnostic_from_record(
            record,
            unsupported,
            term::Glyph_image_presentation::TEXT);

    bool ok = true;
    ok &= check(ambiguous_diagnostic.coverage_kind ==
                term::Glyph_coverage_kind::AMBIGUOUS &&
            ambiguous_diagnostic.presentation ==
                term::Glyph_image_presentation::UNKNOWN &&
            ambiguous_diagnostic.source_format == QImage::Format_RGB16 &&
            ambiguous_diagnostic.source_size == QSize(2, 3) &&
            ambiguous_diagnostic.glyph_index == record.glyph_index &&
            ambiguous_diagnostic.fallback_face_id == record.fallback_face_id &&
            ambiguous_diagnostic.text_run_index == record.text_run_index &&
            ambiguous_diagnostic.glyph_run_index == record.glyph_run_index &&
            ambiguous_diagnostic.glyph_index_in_run ==
                record.glyph_index_in_run &&
            ambiguous_diagnostic.source_string_start ==
                record.source_string_start &&
            ambiguous_diagnostic.source_string_end == record.source_string_end,
        "ambiguous glyph image diagnostics retain source format and glyph identity");
    ok &= check(unsupported_diagnostic.coverage_kind ==
                term::Glyph_coverage_kind::UNSUPPORTED &&
            unsupported_diagnostic.presentation ==
                term::Glyph_image_presentation::TEXT &&
            unsupported_diagnostic.source_format == QImage::Format_Mono &&
            unsupported_diagnostic.source_size == QSize(3, 2) &&
            unsupported_diagnostic.glyph_index == record.glyph_index &&
            unsupported_diagnostic.fallback_face_id == record.fallback_face_id &&
            unsupported_diagnostic.text_run_index == record.text_run_index &&
            unsupported_diagnostic.glyph_run_index == record.glyph_run_index &&
            unsupported_diagnostic.glyph_index_in_run ==
                record.glyph_index_in_run &&
            unsupported_diagnostic.source_string_start ==
                record.source_string_start &&
            unsupported_diagnostic.source_string_end == record.source_string_end,
        "unsupported glyph image diagnostics retain source format and glyph identity");
    return ok;
}

term::Terminal_render_text_run make_shaped_record_test_run(
    QString                         text,
    int                             column,
    int                             cell_span,
    term::terminal_cell_metrics_t   metrics)
{
    term::Terminal_render_text_run run;
    run.row                = 2;
    run.logical_row        = 42;
    run.retained_line_id   = 9001U;
    run.content_generation = 17U;
    run.column             = column;
    run.rect = QRectF(
        static_cast<qreal>(column) * metrics.width,
        static_cast<qreal>(run.row) * metrics.height,
        static_cast<qreal>(std::max(1, cell_span)) * metrics.width,
        metrics.height);
    run.baseline_origin = QPointF(run.rect.left(), run.rect.top() + metrics.ascent);
    run.text            = std::move(text);
    run.foreground      = QColor(220, 230, 210);
    run.background      = QColor(10, 20, 30);
    run.style_id        = term::k_default_terminal_style_id;
    return run;
}

bool shaped_records_have_valid_source_ranges(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    qsizetype                                     text_size)
{
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.source_string_start < 0 ||
            record.source_string_start >= text_size ||
            record.source_string_end <= record.source_string_start ||
            record.source_string_end > text_size)
        {
            return false;
        }
    }
    return true;
}

bool shaped_records_cover_source_range(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    qsizetype                                     start,
    qsizetype                                     end)
{
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.source_string_start <= start &&
            record.source_string_end >= end)
        {
            return true;
        }
    }
    return false;
}

bool shaped_records_have_non_notdef_glyph(
    const term::Qsg_atlas_shaped_text_run_result& shaped)
{
    return std::any_of(
        shaped.records.begin(),
        shaped.records.end(),
        [](const term::Qsg_atlas_shaped_glyph_record& record) {
            return record.glyph_index != 0U;
        });
}

bool shaped_records_share_owner_span(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    int                                           owner_column,
    int                                           owner_cell_span)
{
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.owner_column != owner_column ||
            record.owner_cell_span != owner_cell_span)
        {
            return false;
        }
    }
    return true;
}

bool shaped_records_have_cursor_text_run_flag(
    const term::Qsg_atlas_shaped_text_run_result& shaped)
{
    return std::all_of(
        shaped.records.begin(),
        shaped.records.end(),
        [](const term::Qsg_atlas_shaped_glyph_record& record) {
            return record.cursor_text_run;
        });
}

bool coordinate_is_physical_pixel_snapped(qreal coordinate, qreal dpr)
{
    const qreal physical = coordinate * pixel_normalized_device_pixel_ratio(dpr);
    return std::abs(physical - std::round(physical)) <= 0.001;
}

bool point_is_physical_pixel_snapped(const QPointF& point, qreal dpr)
{
    return
        coordinate_is_physical_pixel_snapped(point.x(), dpr) &&
        coordinate_is_physical_pixel_snapped(point.y(), dpr);
}

bool rectangle_origin_is_physical_pixel_snapped(const QRectF& rect, qreal dpr)
{
    return point_is_physical_pixel_snapped(rect.topLeft(), dpr);
}

bool points_are_close(QPointF left, QPointF right)
{
    return
        std::abs(left.x() - right.x()) <= 0.001 &&
        std::abs(left.y() - right.y()) <= 0.001;
}

bool rectangle_has_physical_pixel_size(
    const QRectF& rect,
    QSize         physical_size,
    qreal         dpr)
{
    const qreal normalized_dpr = pixel_normalized_device_pixel_ratio(dpr);
    return
        std::abs(rect.width() * normalized_dpr -
            static_cast<qreal>(physical_size.width())) <= 0.001 &&
        std::abs(rect.height() * normalized_dpr -
            static_cast<qreal>(physical_size.height())) <= 0.001;
}

bool shaped_records_snap_fractional_physical_pixel_placement(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    qreal                                         dpr)
{
    bool saw_fractional_input = false;
    bool checked              = false;
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.glyph_index == 0U ||
            record.glyph_bounds.width() <= 0.0 ||
            record.glyph_bounds.height() <= 0.0)
        {
            continue;
        }

        constexpr QSize k_probe_physical_size(17, 11);
        constexpr QPoint k_probe_physical_offset(-2, -9);
        const QPointF input_draw_origin =
            record.glyph_origin +
            QPointF(
                static_cast<qreal>(k_probe_physical_offset.x()) / dpr,
                static_cast<qreal>(k_probe_physical_offset.y()) / dpr);
        saw_fractional_input =
            saw_fractional_input ||
            !point_is_physical_pixel_snapped(record.glyph_origin, dpr) ||
            !point_is_physical_pixel_snapped(input_draw_origin, dpr);

        const QPointF snapped_origin =
            term::qsg_atlas_snapped_physical_point(record.glyph_origin, dpr);
        const QRectF snapped_draw_rect =
            term::qsg_atlas_snapped_glyph_draw_rect(
                record.glyph_origin,
                k_probe_physical_offset,
                k_probe_physical_size,
                dpr);
        const int snapped_physical_origin_x = static_cast<int>(
            std::lround(snapped_origin.x() * dpr));
        const int snapped_physical_origin_y = static_cast<int>(
            std::lround(snapped_origin.y() * dpr));
        const QPointF expected_top_left(
            static_cast<qreal>(
                snapped_physical_origin_x + k_probe_physical_offset.x()) /
                dpr,
            static_cast<qreal>(
                snapped_physical_origin_y + k_probe_physical_offset.y()) /
                dpr);
        checked = true;
        if (!point_is_physical_pixel_snapped(snapped_origin, dpr) ||
            !rectangle_origin_is_physical_pixel_snapped(snapped_draw_rect, dpr) ||
            !rectangle_has_physical_pixel_size(
                snapped_draw_rect,
                k_probe_physical_size,
                dpr) ||
            !points_are_close(snapped_draw_rect.topLeft(), expected_top_left))
        {
            return false;
        }
    }

    return checked && saw_fractional_input;
}

bool shaped_records_use_raster_offsets_not_public_bounds_proxy(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    qreal                                         dpr)
{
    bool checked              = false;
    bool saw_distinct_offset  = false;
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.glyph_index == 0U ||
            record.glyph_bounds.width() <= 0.0 ||
            record.glyph_bounds.height() <= 0.0)
        {
            continue;
        }

        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(record.physical_pixel_size);
        const QImage alpha_map = raster_font.alphaMapForGlyph(
            record.glyph_index,
            QRawFont::SubPixelAntialiasing);
        const term::Glyph_rgba_tile tile =
            term::qsg_atlas_rgba_tile_from_image(
                alpha_map,
                term::Glyph_image_presentation::TEXT);
        if (!tile.is_valid()) {
            continue;
        }

        const QPoint raster_offset =
            term::qsg_atlas_glyph_physical_offset_for_raster_font(
                raster_font,
                record.glyph_index,
                term::Glyph_image_presentation::TEXT);
        const QPoint public_bounds_proxy(
            static_cast<int>(std::lround(record.glyph_bounds.left() * dpr)),
            static_cast<int>(std::lround(record.glyph_bounds.top() * dpr)));
        const QRectF draw_rect =
            term::qsg_atlas_snapped_glyph_draw_rect(
                record.glyph_origin,
                raster_offset,
                tile.size,
                dpr);

        checked = true;
        saw_distinct_offset =
            saw_distinct_offset || raster_offset != public_bounds_proxy;
        if (!rectangle_origin_is_physical_pixel_snapped(draw_rect, dpr) ||
            !rectangle_has_physical_pixel_size(draw_rect, tile.size, dpr))
        {
            return false;
        }
    }

    return checked && saw_distinct_offset;
}

QImage render_qpainter_text_layout_image(
    const term::Terminal_render_text_run& run,
    const QFont&                          font,
    QSizeF                                logical_size,
    qreal                                 dpr)
{
    QImage image(
        pixel_window_physical_pixel_size(logical_size, dpr),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(pixel_normalized_device_pixel_ratio(dpr));
    image.fill(run.background);

    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(run.foreground);

    QTextLayout layout(run.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(1024.0 * 1024.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
    if (line.isValid()) {
        layout.draw(
            &painter,
            QPointF(
                run.baseline_origin.x(),
                run.baseline_origin.y() - line.ascent()));
    }
    return image;
}

QImage render_atlas_tile_placement_image(
    const term::Terminal_render_text_run&      run,
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    QSizeF                                     logical_size,
    qreal                                      dpr)
{
    QImage image(
        pixel_window_physical_pixel_size(logical_size, dpr),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(pixel_normalized_device_pixel_ratio(dpr));
    image.fill(run.background);

    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(record.physical_pixel_size);
        paint_atlas_rgba_reference_glyph(
            image,
            record,
            raster_font,
            run,
            dpr);
    }
    return image;
}

bool shaped_records_match_qpainter_physical_ink_placement(
    const term::Terminal_render_text_run&      run,
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    const QFont&                              font,
    term::terminal_cell_metrics_t             metrics,
    qreal                                     dpr)
{
    const QSizeF logical_size =
        pixel_logical_size({4, 12}, metrics);
    const QRectF probe_region =
        run.rect.adjusted(-metrics.width, -metrics.height,
            metrics.width, metrics.height);
    const QImage expected =
        render_qpainter_text_layout_image(run, font, logical_size, dpr);
    const QImage actual =
        render_atlas_tile_placement_image(run, shaped, logical_size, dpr);
    const Pixel_image_ink_stats expected_ink =
        measure_image_ink(expected, run.background, probe_region, dpr);
    const Pixel_image_ink_stats actual_ink =
        measure_image_ink(actual, run.background, probe_region, dpr);

    if (!expected_ink.has_ink() || !actual_ink.has_ink()) {
        return false;
    }

    constexpr int k_bbox_tolerance_pixels = 1;
    constexpr double k_center_tolerance_pixels = 1.0;
    return
        std::abs(expected_ink.bbox.left() - actual_ink.bbox.left()) <=
            k_bbox_tolerance_pixels &&
        std::abs(expected_ink.bbox.right() - actual_ink.bbox.right()) <=
            k_bbox_tolerance_pixels &&
        std::abs(expected_ink.bbox.top() - actual_ink.bbox.top()) <=
            k_bbox_tolerance_pixels &&
        std::abs(expected_ink.bbox.bottom() - actual_ink.bbox.bottom()) <=
            k_bbox_tolerance_pixels &&
        std::abs(expected_ink.center_x - actual_ink.center_x) <=
            k_center_tolerance_pixels &&
        std::abs(expected_ink.center_y - actual_ink.center_y) <=
            k_center_tolerance_pixels;
}

bool test_shaped_glyph_physical_pixel_placement()
{
    constexpr qreal dpr = 1.5;
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    term::Terminal_render_text_run run =
        make_shaped_record_test_run(QStringLiteral("Hg"), 2, 2, metrics);
    const QPointF fractional_offset(0.2, 0.2);
    run.rect.translate(fractional_offset);
    run.baseline_origin += fractional_offset;

    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(
            run,
            font,
            metrics,
            dpr,
            19,
            false);

    bool ok = true;
    ok &= check(!shaped.records.empty() &&
            shaped_records_have_non_notdef_glyph(shaped),
        "fractional glyph placement fixture produces drawable shaped records");
    ok &= check(
        shaped_records_snap_fractional_physical_pixel_placement(shaped, dpr),
        "fractional glyph origins and draw rectangles snap to physical pixels");
    ok &= check(
        shaped_records_use_raster_offsets_not_public_bounds_proxy(shaped, dpr),
        "glyph draw rectangles use Qt raster offsets instead of public bounds proxy");
    ok &= check(
        shaped_records_match_qpainter_physical_ink_placement(
            run,
            shaped,
            font,
            metrics,
            dpr),
        "atlas raster placement matches independent QPainter text ink placement");
    return ok;
}

constexpr ushort k_ascii_layout_probe_first = 0x20U;
constexpr ushort k_ascii_layout_probe_last  = 0x7eU;

QString ascii_layout_probe_text()
{
    QString text;
    text.reserve(static_cast<qsizetype>(
        k_ascii_layout_probe_last - k_ascii_layout_probe_first + 1U +
        QStringLiteral("==!=->=><=>=::///www").size()));
    for (ushort value = k_ascii_layout_probe_first;
        value <= k_ascii_layout_probe_last;
        ++value)
    {
        text.append(QChar(value));
    }
    text.append(QStringLiteral("==!=->=><=>=::///www"));
    return text;
}

bool ascii_layout_positions_are_cell_stable(
    const QString&                  probe_text,
    const QFont&                    layout_font,
    term::terminal_cell_metrics_t   metrics)
{
    QTextLayout layout(probe_text, layout_font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return false;
    }
    line.setLineWidth(1024.0 * 1024.0);
    line.setPosition(QPointF(0.0, 0.0));
    const bool line_is_complete =
        line.textStart() == 0 && line.textLength() == probe_text.size();
    const QList<QGlyphRun> glyph_runs = line.glyphRuns(
        0,
        probe_text.size(),
        QTextLayout::RetrieveGlyphIndexes   |
            QTextLayout::RetrieveGlyphPositions |
            QTextLayout::RetrieveStringIndexes);
    layout.endLayout();

    if (!line_is_complete) {
        return false;
    }

    const QRawFont raw_font = QRawFont::fromFont(layout_font);
    if (!raw_font.isValid()) {
        return false;
    }

    // Cell stability is verified against the layout font's own monospace
    // advance -- the value QTextLayout uses to place each glyph -- rather than
    // the snapped cell width. The terminal rounds the glyph advance UP to a
    // whole device pixel for cell sizing (see Qt_grid_metrics_provider), so raw
    // layout positions sit just below integer cell multiples on font engines
    // that report fractional advances (FreeType on Linux: ~8.984px) while they
    // coincide with the snapped width on engines that round advances to whole
    // pixels (DirectWrite/CoreText). Both the reference advance and the glyph
    // positions come from the same engine here, so the guarantee under test --
    // one evenly spaced glyph per source cell, no ligature collapsing -- holds
    // identically on every platform.
    const QList<QPointF> probe_advances =
        raw_font.advancesForGlyphIndexes(
            raw_font.glyphIndexesForString(QStringLiteral("M")));
    const qreal cell_advance =
        probe_advances.isEmpty() ? metrics.width : probe_advances.first().x();

    std::optional<qreal> glyph_position_y;
    std::vector<bool> seen_string_indexes(
        static_cast<std::size_t>(probe_text.size()));
    for (const QGlyphRun& glyph_run : glyph_runs) {
        if (term::qsg_atlas_face_id_for_raw_font(glyph_run.rawFont()) !=
            term::qsg_atlas_face_id_for_raw_font(raw_font))
        {
            return false;
        }

        const QList<quint32>   glyph_indexes  = glyph_run.glyphIndexes();
        const QList<QPointF>   positions      = glyph_run.positions();
        const QList<qsizetype> string_indexes = glyph_run.stringIndexes();
        if (glyph_indexes.size() != positions.size() ||
            glyph_indexes.size() != string_indexes.size())
        {
            return false;
        }

        for (qsizetype index = 0; index < glyph_indexes.size(); ++index) {
            const qsizetype string_index = string_indexes.at(index);
            if (string_index < 0 || string_index >= probe_text.size()) {
                return false;
            }

            const QChar code_unit = probe_text.at(string_index);
            const QList<quint32> raw_indexes =
                raw_font.glyphIndexesForString(QString(1, code_unit));
            if (raw_indexes.size() != 1 ||
                raw_indexes.at(0) == 0U ||
                raw_indexes.at(0) != glyph_indexes.at(index))
            {
                return false;
            }

            const QPointF position = positions.at(index);
            const qreal expected_x =
                static_cast<qreal>(string_index) * cell_advance;
            if (std::abs(position.x() - expected_x) > 0.5) {
                return false;
            }
            if (!glyph_position_y.has_value()) {
                glyph_position_y = position.y();
            }
            else
            if (std::abs(position.y() - *glyph_position_y) > 0.001) {
                return false;
            }

            seen_string_indexes[static_cast<std::size_t>(string_index)] = true;
        }
    }

    for (bool seen : seen_string_indexes) {
        if (!seen) {
            return false;
        }
    }

    return true;
}

bool test_cell_stable_ascii_layout_font()
{
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const QFont layout_font =
        term::qsg_atlas_cell_stable_ascii_layout_font(font);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(1.0);

    bool ok = true;
    ok &= check(!layout_font.kerning(),
        "cell-stable ASCII layout font disables kerning");
    ok &= check(
        (static_cast<int>(layout_font.styleStrategy()) &
            static_cast<int>(QFont::PreferNoShaping)) != 0,
        "cell-stable ASCII layout font requests no shaping");
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    for (QFont::Tag tag : {
        QFont::Tag("calt"),
        QFont::Tag("clig"),
        QFont::Tag("dlig"),
        QFont::Tag("hlig"),
        QFont::Tag("liga"),
        QFont::Tag("rlig"),
    }) {
        ok &= check(layout_font.featureValue(tag) == 0U,
            "cell-stable ASCII layout font disables ASCII ligature features");
    }
#endif
    const QFontInfo resolved_font(layout_font);
    ok &= check(resolved_font.fixedPitch(),
        "cell-stable ASCII layout font resolves to a fixed-pitch face");

    const QFontMetricsF font_metrics(layout_font);
    for (ushort value = k_ascii_layout_probe_first;
        value <= k_ascii_layout_probe_last;
        ++value)
    {
        // The terminal snaps each cell to an integer pixel width, so the cell
        // width is the rounded glyph advance. QFontMetricsF reports the precise
        // fractional advance, which differs from that rounded width by up to half
        // a pixel depending on the font engine (FreeType returns the unrounded
        // design advance; DirectWrite/CoreText round to whole pixels). Per-cell
        // glyph placement uses the cell width, not this advance, and the actual
        // cell-stability of positions is asserted separately below, so require the
        // advance only to round to the cell width.
        ok &= check(
            std::abs(
                font_metrics.horizontalAdvance(
                    QLatin1Char(static_cast<char>(value))) -
                metrics.width) <= 0.5,
            "cell-stable ASCII glyph advances match terminal cell width");
    }

    ok &= check(
        ascii_layout_positions_are_cell_stable(
            ascii_layout_probe_text(),
            layout_font,
            metrics),
        "cell-stable ASCII probe keeps one glyph per source cell");
    return ok;
}

bool test_shaped_glyph_records()
{
    const qreal dpr = 1.0;
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);

    const term::Terminal_render_text_run ascii_run =
        make_shaped_record_test_run(QStringLiteral("ABC"), 4, 3, metrics);
    const term::Qsg_atlas_shaped_text_run_result ascii =
        term::qsg_atlas_shape_text_run(
            ascii_run,
            font,
            metrics,
            dpr,
            7,
            false);
    const term::Qsg_atlas_shaped_text_run_result cursor_ascii =
        term::qsg_atlas_shape_text_run(
            ascii_run,
            font,
            metrics,
            dpr,
            12,
            true);

    const QString combining_text = QString::fromUtf8("e\xcc\x81");
    const term::Terminal_render_text_run combining_run =
        make_shaped_record_test_run(combining_text, 3, 1, metrics);
    const term::Qsg_atlas_shaped_text_run_result combining =
        term::qsg_atlas_shape_text_run(
            combining_run,
            font,
            metrics,
            dpr,
            8,
            false);

    const QString cjk_text = QString::fromUtf8("\xe7\x95\x8c");
    const term::Terminal_render_text_run cjk_run =
        make_shaped_record_test_run(cjk_text, 5, 2, metrics);
    const term::Qsg_atlas_shaped_text_run_result cjk =
        term::qsg_atlas_shape_text_run(
            cjk_run,
            font,
            metrics,
            dpr,
            9,
            false);

    const QString vs16_text = QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f");
    const term::Terminal_render_text_run vs16_run =
        make_shaped_record_test_run(vs16_text, 6, 2, metrics);
    const term::Qsg_atlas_shaped_text_run_result vs16 =
        term::qsg_atlas_shape_text_run(
            vs16_run,
            font,
            metrics,
            dpr,
            10,
            false);

    const QString zwj_text =
        QString::fromUcs4(U"\U0001F469\u200D\U0001F4BB");
    const term::Terminal_render_text_run zwj_run =
        make_shaped_record_test_run(zwj_text, 7, 2, metrics);
    const term::Qsg_atlas_shaped_text_run_result zwj =
        term::qsg_atlas_shape_text_run(
            zwj_run,
            font,
            metrics,
            dpr,
            11,
            false);

    bool ok = true;
    ok &= check(!ascii.records.empty() &&
            ascii.missing_string_indexes == 0 &&
            ascii.invalid_string_indexes == 0 &&
            shaped_records_have_non_notdef_glyph(ascii),
        "printable ASCII is shaped into glyph records with string indexes");
    ok &= check(ascii.records.size() == 3U &&
            ascii.records[0].source_string_start == 0 &&
            ascii.records[0].source_string_end == 1 &&
            ascii.records[0].owner_column == 4 &&
            ascii.records[0].owner_cell_span == 1 &&
            ascii.records[0].glyph_origin.x() == ascii_run.rect.left() &&
            ascii.records[1].source_string_start == 1 &&
            ascii.records[1].source_string_end == 2 &&
            ascii.records[1].owner_column == 5 &&
            ascii.records[1].owner_cell_span == 1 &&
            ascii.records[1].glyph_origin.x() ==
                ascii_run.rect.left() + metrics.width &&
            ascii.records[2].source_string_start == 2 &&
            ascii.records[2].source_string_end == 3 &&
            ascii.records[2].owner_column == 6 &&
            ascii.records[2].owner_cell_span == 1 &&
            ascii.records[2].glyph_origin.x() ==
                ascii_run.rect.left() + metrics.width * 2.0,
        "printable ASCII shaped records retain exact per-cell ownership");
    ok &= check(ascii.records[0].text_run_index == 7 &&
            !ascii.records[0].cursor_text_run &&
            ascii.records[0].row == ascii_run.row &&
            ascii.records[0].logical_row == ascii_run.logical_row &&
            ascii.records[0].retained_line_id == ascii_run.retained_line_id &&
            ascii.records[0].content_generation ==
                ascii_run.content_generation,
        "printable ASCII shaped records retain text-run provenance");
    ok &= check(!cursor_ascii.records.empty() &&
            cursor_ascii.missing_string_indexes == 0 &&
            cursor_ascii.invalid_string_indexes == 0 &&
            shaped_records_have_cursor_text_run_flag(cursor_ascii),
        "cursor shaped records retain cursor-run provenance");

    ok &= check(!combining.records.empty() &&
            combining.missing_string_indexes == 0 &&
            combining.invalid_string_indexes == 0 &&
            shaped_records_have_non_notdef_glyph(combining) &&
            shaped_records_have_valid_source_ranges(
                combining,
                combining_text.size()) &&
            shaped_records_cover_source_range(
                combining,
                0,
                combining_text.size()) &&
            shaped_records_share_owner_span(combining, 3, 1),
        "combining-mark shaped records retain one-cell cluster ownership");

    // The CPU unit environment may lack deployed CJK/emoji fallback fonts.
    // Concrete family glyph-image coverage is gated by the D3D11 LCD probe.
    ok &= check(!cjk.records.empty() &&
            cjk.missing_string_indexes == 0 &&
            cjk.invalid_string_indexes == 0 &&
            shaped_records_have_valid_source_ranges(cjk, cjk_text.size()) &&
            shaped_records_share_owner_span(cjk, 5, 2),
        "CJK shaped records retain two-cell ownership");

    ok &= check(!vs16.records.empty() &&
            vs16.missing_string_indexes == 0 &&
            vs16.invalid_string_indexes == 0 &&
            shaped_records_have_valid_source_ranges(vs16, vs16_text.size()) &&
            shaped_records_cover_source_range(vs16, 0, vs16_text.size()) &&
            shaped_records_share_owner_span(vs16, 6, 2),
        "VS16 emoji shaped records retain source cluster ownership");

    ok &= check(!zwj.records.empty() &&
            zwj.missing_string_indexes == 0 &&
            zwj.invalid_string_indexes == 0 &&
            shaped_records_have_valid_source_ranges(zwj, zwj_text.size()) &&
            shaped_records_cover_source_range(zwj, 0, zwj_text.size()) &&
            shaped_records_share_owner_span(zwj, 7, 2),
        "ZWJ emoji shaped records retain one terminal-span owner");
    return ok;
}

bool test_epoch_invalidation()
{
    const term::Glyph_rgba_tile tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14});

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

struct Buffer_update_test_instance
{
    int row_marker = 0;
    int value      = 0;
};

QByteArray buffer_update_instance_bytes(
    const std::vector<Buffer_update_test_instance>& instances)
{
    return QByteArray(
        reinterpret_cast<const char*>(instances.data()),
        static_cast<int>(instances.size() * sizeof(Buffer_update_test_instance)));
}

term::Qsg_atlas_buffer_update_plan committed_buffer_update_plan(
    term::Qsg_atlas_buffer_upload_planner&        planner,
    const term::Qsg_atlas_buffer_update_input&    input)
{
    term::Qsg_atlas_buffer_update_plan plan = planner.plan(input);
    planner.commit(plan);
    return plan;
}

bool test_atlas_rotating_buffer_planner()
{
    term::Qsg_atlas_buffer_upload_planner planner;
    const std::vector<int> rows = {0, 1, 2};
    const QByteArray layout_key = QByteArrayLiteral("stable-layout");
    const std::vector<term::Terminal_render_dirty_row_range> dirty_row_1 = {{1, 1}};

    std::vector<Buffer_update_test_instance> base = {
        {0, 10},
        {1, 20},
        {2, 30},
    };
    std::vector<Buffer_update_test_instance> row_1_changed = base;
    row_1_changed[1].value = 21;

    const QByteArray base_bytes = buffer_update_instance_bytes(base);
    const QByteArray changed_bytes = buffer_update_instance_bytes(row_1_changed);
    const std::vector<int> shifted_rows = {0, 1, 1};
    std::vector<Buffer_update_test_instance> shifted_row_owner = row_1_changed;
    shifted_row_owner[2].row_marker = 1;
    shifted_row_owner[2].value      = 31;
    const QByteArray shifted_row_owner_bytes =
        buffer_update_instance_bytes(shifted_row_owner);
    const std::vector<int> grown_rows = {0, 1, 1, 1};
    std::vector<Buffer_update_test_instance> grown_row_owner = shifted_row_owner;
    grown_row_owner.push_back({1, 40});
    const QByteArray grown_row_owner_bytes =
        buffer_update_instance_bytes(grown_row_owner);
    const term::Qsg_atlas_buffer_update_plan seed_slot_0 =
        committed_buffer_update_plan(planner, {
            2,
            0,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            base_bytes.constData(),
            static_cast<int>(base_bytes.size()),
            &rows,
            layout_key,
            {},
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan seed_slot_1 =
        committed_buffer_update_plan(planner, {
            2,
            1,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            base_bytes.constData(),
            static_cast<int>(base_bytes.size()),
            &rows,
            layout_key,
            {},
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan partial_slot_0 =
        committed_buffer_update_plan(planner, {
            2,
            0,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            changed_bytes.constData(),
            static_cast<int>(changed_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan partial_slot_1 =
        committed_buffer_update_plan(planner, {
            2,
            1,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            changed_bytes.constData(),
            static_cast<int>(changed_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan clean_slot_0 =
        committed_buffer_update_plan(planner, {
            2,
            0,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            changed_bytes.constData(),
            static_cast<int>(changed_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan shifted_rows_slot_0 =
        committed_buffer_update_plan(planner, {
            2,
            0,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            shifted_row_owner_bytes.constData(),
            static_cast<int>(shifted_row_owner_bytes.size()),
            &shifted_rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
        });
    const term::Qsg_atlas_buffer_update_plan grown_rows_slot_0 =
        committed_buffer_update_plan(planner, {
            2,
            0,
            3,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            grown_row_owner_bytes.constData(),
            static_cast<int>(grown_row_owner_bytes.size()),
            &grown_rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
        });

    bool ok = true;
    ok &= check(seed_slot_0.summary.full_upload &&
            seed_slot_0.summary.rotating_slot_seed_upload &&
            seed_slot_0.ranges.size() == 1U,
        "atlas upload planner seeds rotating buffer slot 0 with a full upload");
    ok &= check(seed_slot_1.summary.full_upload &&
            seed_slot_1.summary.rotating_slot_seed_upload &&
            seed_slot_1.summary.seeded_slots == 2,
        "atlas upload planner seeds rotating buffer slot 1 independently");
    ok &= check(partial_slot_0.summary.partial_upload &&
            !partial_slot_0.summary.full_upload &&
            partial_slot_0.ranges.size() == 1U &&
            partial_slot_0.ranges.front().byte_offset ==
                static_cast<int>(sizeof(Buffer_update_test_instance)) &&
            partial_slot_0.ranges.front().byte_count ==
                static_cast<int>(sizeof(Buffer_update_test_instance)),
        "atlas upload planner emits a dirty-row byte range for slot 0");
    ok &= check(partial_slot_1.summary.partial_upload &&
            !partial_slot_1.summary.full_upload &&
            partial_slot_1.ranges.size() == 1U,
        "atlas upload planner keeps per-slot mirrors for the rotating buffer");
    ok &= check(clean_slot_0.summary.skipped_upload &&
            clean_slot_0.summary.uploaded_bytes == 0,
        "atlas upload planner skips uploads when the active slot already matches");
    ok &= check(shifted_rows_slot_0.summary.partial_upload &&
            !shifted_rows_slot_0.summary.full_upload &&
            !shifted_rows_slot_0.summary.instance_layout_changed_upload &&
            shifted_rows_slot_0.ranges.size() == 1U &&
            shifted_rows_slot_0.ranges.front().byte_offset ==
                static_cast<int>(2U * sizeof(Buffer_update_test_instance)) &&
            shifted_rows_slot_0.ranges.front().byte_count ==
                static_cast<int>(sizeof(Buffer_update_test_instance)),
        "atlas upload planner diffs row-owner changes when layout bytes stay sized");
    ok &= check(grown_rows_slot_0.summary.partial_upload &&
            !grown_rows_slot_0.summary.full_upload &&
            grown_rows_slot_0.summary.instance_layout_changed_upload &&
            grown_rows_slot_0.ranges.size() == 1U &&
            grown_rows_slot_0.ranges.front().byte_offset ==
                static_cast<int>(3U * sizeof(Buffer_update_test_instance)) &&
            grown_rows_slot_0.ranges.front().byte_count ==
                static_cast<int>(sizeof(Buffer_update_test_instance)),
        "atlas upload planner appends dirty-row instances without full upload");
    return ok;
}

bool test_atlas_row_stable_glyph_planner()
{
    term::Qsg_atlas_buffer_upload_planner planner;
    constexpr int row_count    = 3;
    constexpr int row_capacity = 2;
    const std::vector<int> rows = {
        0, 0,
        1, 1,
        2, 2,
    };
    const QByteArray layout_key = QByteArrayLiteral("row-stable-glyph-layout");
    const std::vector<term::Terminal_render_dirty_row_range> dirty_row_1 = {{1, 1}};

    std::vector<Buffer_update_test_instance> base = {
        {0, 10}, {0, 11},
        {1, 20}, {1, 0},
        {2, 30}, {2, 31},
    };
    std::vector<Buffer_update_test_instance> dirty_row_grown = base;
    dirty_row_grown[3] = {1, 21};
    std::vector<Buffer_update_test_instance> dirty_row_shrunk = base;
    dirty_row_shrunk[2] = {1, 0};
    std::vector<Buffer_update_test_instance> clean_row_and_dirty_row_changed =
        dirty_row_grown;
    clean_row_and_dirty_row_changed[0].value = 12;
    std::vector<Buffer_update_test_instance> dirty_row_non_adjacent = base;
    dirty_row_non_adjacent[2] = {1, 21};
    dirty_row_non_adjacent[3] = {1, 23};

    const QByteArray base_bytes = buffer_update_instance_bytes(base);
    const QByteArray dirty_row_grown_bytes =
        buffer_update_instance_bytes(dirty_row_grown);
    const QByteArray dirty_row_shrunk_bytes =
        buffer_update_instance_bytes(dirty_row_shrunk);
    const QByteArray clean_row_and_dirty_row_changed_bytes =
        buffer_update_instance_bytes(clean_row_and_dirty_row_changed);
    const QByteArray dirty_row_non_adjacent_bytes =
        buffer_update_instance_bytes(dirty_row_non_adjacent);
    const std::vector<term::Qsg_atlas_row_stable_range> row_stable_ranges = {
        {0, 0, row_capacity},
        {1, 2, row_capacity},
        {2, 4, row_capacity},
    };
    (void)committed_buffer_update_plan(planner, {
        1,
        0,
        row_count,
        static_cast<int>(sizeof(Buffer_update_test_instance)),
        base_bytes.constData(),
        static_cast<int>(base_bytes.size()),
        &rows,
        layout_key,
        {},
        false,
        false,
        false,
        5,
        true,
    });

    const term::Qsg_atlas_buffer_update_plan dirty =
        committed_buffer_update_plan(planner, {
            1,
            0,
            row_count,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            dirty_row_grown_bytes.constData(),
            static_cast<int>(dirty_row_grown_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
            6,
            true,
        });
    term::Qsg_atlas_buffer_upload_planner shrink_planner;
    (void)committed_buffer_update_plan(shrink_planner, {
        1,
        0,
        row_count,
        static_cast<int>(sizeof(Buffer_update_test_instance)),
        dirty_row_grown_bytes.constData(),
        static_cast<int>(dirty_row_grown_bytes.size()),
        &rows,
        layout_key,
        {},
        false,
        false,
        false,
        6,
        true,
    });
    const term::Qsg_atlas_buffer_update_plan shrink =
        committed_buffer_update_plan(shrink_planner, {
            1,
            0,
            row_count,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            dirty_row_shrunk_bytes.constData(),
            static_cast<int>(dirty_row_shrunk_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
            4,
            true,
        });

    term::Qsg_atlas_buffer_upload_planner fallback_planner;
    (void)committed_buffer_update_plan(fallback_planner, {
        1,
        0,
        row_count,
        static_cast<int>(sizeof(Buffer_update_test_instance)),
        base_bytes.constData(),
        static_cast<int>(base_bytes.size()),
        &rows,
        layout_key,
        {},
        false,
        false,
        false,
        5,
        true,
    });
    const term::Qsg_atlas_buffer_update_plan clean_slot_fallback =
        committed_buffer_update_plan(fallback_planner, {
            1,
            0,
            row_count,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            clean_row_and_dirty_row_changed_bytes.constData(),
            static_cast<int>(clean_row_and_dirty_row_changed_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
            6,
            true,
        });

    term::Qsg_atlas_buffer_upload_planner row_span_planner;
    (void)committed_buffer_update_plan(row_span_planner, {
        1,
        0,
        row_count,
        static_cast<int>(sizeof(Buffer_update_test_instance)),
        base_bytes.constData(),
        static_cast<int>(base_bytes.size()),
        &rows,
        layout_key,
        {},
        false,
        false,
        false,
        5,
        true,
        &row_stable_ranges,
    });
    const term::Qsg_atlas_buffer_update_plan dirty_row_span =
        committed_buffer_update_plan(row_span_planner, {
            1,
            0,
            row_count,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            dirty_row_non_adjacent_bytes.constData(),
            static_cast<int>(dirty_row_non_adjacent_bytes.size()),
            &rows,
            layout_key,
            dirty_row_1,
            false,
            false,
            false,
            6,
            true,
            &row_stable_ranges,
        });

    bool ok = true;
    ok &= check(dirty.summary.row_stable_layout &&
            dirty.summary.active_instance_count == 6 &&
            dirty.summary.instance_count == row_count * row_capacity,
        "atlas row-stable glyph planner reports active and reserved instances");
    ok &= check(dirty.summary.partial_upload &&
            !dirty.summary.full_upload &&
            !dirty.summary.non_dirty_state_upload &&
            dirty.ranges.size() == 1U &&
            dirty.ranges.front().byte_offset ==
                static_cast<int>(3U * sizeof(Buffer_update_test_instance)) &&
            dirty.ranges.front().byte_count ==
                static_cast<int>(sizeof(Buffer_update_test_instance)),
        "atlas row-stable glyph planner patches dirty-row growth without "
        "full-uploading stable clean rows");
    ok &= check(shrink.summary.row_stable_layout &&
            shrink.summary.partial_upload &&
            !shrink.summary.full_upload &&
            !shrink.summary.non_dirty_state_upload &&
            shrink.summary.active_instance_count == 4 &&
            shrink.ranges.size() == 1U &&
            shrink.ranges.front().byte_offset ==
                static_cast<int>(2U * sizeof(Buffer_update_test_instance)) &&
            shrink.ranges.front().byte_count ==
                static_cast<int>(2U * sizeof(Buffer_update_test_instance)),
        "atlas row-stable glyph planner patches dirty-row shrink-to-empty "
        "without full-uploading stable clean rows");
    ok &= check(dirty_row_span.summary.row_stable_layout &&
            dirty_row_span.summary.partial_upload &&
            !dirty_row_span.summary.full_upload &&
            dirty_row_span.ranges.size() == 1U &&
            dirty_row_span.ranges.front().byte_offset ==
                static_cast<int>(2U * sizeof(Buffer_update_test_instance)) &&
            dirty_row_span.ranges.front().byte_count ==
                static_cast<int>(row_capacity * sizeof(Buffer_update_test_instance)),
        "atlas row-stable glyph planner uploads the whole dirty row slot span "
        "when production row ranges are available");
    ok &= check(clean_slot_fallback.summary.row_stable_layout &&
            clean_slot_fallback.summary.full_upload &&
            !clean_slot_fallback.summary.partial_upload &&
            clean_slot_fallback.summary.non_dirty_state_upload &&
            clean_slot_fallback.ranges.size() == 1U &&
            clean_slot_fallback.ranges.front().byte_offset == 0 &&
            clean_slot_fallback.ranges.front().byte_count ==
                static_cast<int>(clean_row_and_dirty_row_changed_bytes.size()),
        "atlas row-stable glyph planner full-uploads when a clean-row slot "
        "changes beside dirty-row content");
    return ok;
}

bool test_atlas_non_dirty_and_full_reupload_planner()
{
    term::Qsg_atlas_buffer_upload_planner planner;
    const std::vector<int> rows = {0, 1};
    const QByteArray layout_key = QByteArrayLiteral("layout");
    std::vector<Buffer_update_test_instance> instances = {
        {0, 10},
        {1, 20},
    };
    const QByteArray bytes = buffer_update_instance_bytes(instances);
    (void)committed_buffer_update_plan(planner, {
        1,
        0,
        2,
        static_cast<int>(sizeof(Buffer_update_test_instance)),
        bytes.constData(),
        static_cast<int>(bytes.size()),
        &rows,
        layout_key,
        {},
        false,
        false,
        false,
    });

    bool ok = true;
    for (const char* invalidation : {
            "selection",
            "cursor",
            "preedit",
            "options",
            "visual_bell",
        })
    {
        const term::Qsg_atlas_buffer_update_plan non_dirty =
            committed_buffer_update_plan(planner, {
                1,
                0,
                2,
                static_cast<int>(sizeof(Buffer_update_test_instance)),
                bytes.constData(),
                static_cast<int>(bytes.size()),
                &rows,
                layout_key,
                {},
                false,
                false,
                true,
            });
        ok &= check(non_dirty.summary.full_upload &&
                non_dirty.summary.non_dirty_state_upload,
            std::string("atlas upload planner full-uploads on non-dirty ") +
                invalidation + " invalidation");
    }

    const term::Qsg_atlas_buffer_update_plan public_projection =
        committed_buffer_update_plan(planner, {
            1,
            0,
            2,
            static_cast<int>(sizeof(Buffer_update_test_instance)),
            bytes.constData(),
            static_cast<int>(bytes.size()),
            &rows,
            layout_key,
            {{0, 2}},
            false,
            true,
            false,
        });
    ok &= check(public_projection.summary.full_upload &&
            public_projection.summary.full_repaint_upload,
        "atlas upload planner full-uploads PUBLIC_PROJECTION/SCROLL repaint frames");
    return ok;
}

bool test_msdf_text_buffer_fallback_retry_decision()
{
    using Result = term::Qsg_atlas_buffer_update_result;

    bool ok = true;
    ok &= check(
        term::qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
            false,
            true,
            true,
            Result::MSDF_TEXT_BUFFER_FAILED),
        "MSDF text fallback retry is enabled for MSDF buffer failures");
    for (Result result : {
            Result::NOT_RUN,
            Result::READY,
            Result::RECT_BUFFER_FAILED,
            Result::GLYPH_BUFFER_FAILED,
        })
    {
        ok &= check(
            !term::qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
                false,
                true,
                true,
                result),
            "MSDF text fallback retry ignores non-MSDF buffer results");
    }
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
            true,
            true,
            true,
            Result::MSDF_TEXT_BUFFER_FAILED),
        "MSDF text fallback retry does not run twice in one prepare");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
            false,
            false,
            true,
            Result::MSDF_TEXT_BUFFER_FAILED),
        "MSDF text fallback retry waits for successful non-MSDF GPU resources");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_buffer_update(
            false,
            true,
            false,
            Result::MSDF_TEXT_BUFFER_FAILED),
        "MSDF text fallback retry requires pending MSDF draw passes");
    return ok;
}

bool test_msdf_text_prepare_fallback_retry_decision()
{
    bool ok = true;
    ok &= check(
        term::qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
            false,
            true,
            true,
            true),
        "MSDF text prepare fallback retry is enabled for attempted MSDF failures");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
            true,
            true,
            true,
            true),
        "MSDF text prepare fallback retry does not run twice in one prepare");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
            false,
            false,
            true,
            true),
        "MSDF text prepare fallback retry requires pending MSDF draw passes");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
            false,
            true,
            false,
            true),
        "MSDF text prepare fallback retry ignores shared resource failures");
    ok &= check(
        !term::qsg_atlas_should_retry_msdf_text_fallback_after_prepare(
            false,
            true,
            true,
            false),
        "MSDF text prepare fallback retry requires a failed MSDF prepare resource");
    return ok;
}

bool test_msdf_text_failure_diagnostics_merge()
{
    term::Qsg_atlas_render_summary failed;
    failed.msdf_text_supported_runs          = 3;
    failed.msdf_text_missed_supported_runs   = 2;
    failed.msdf_text_missed_supported_glyphs = 7;
    failed.msdf_text_atlas_built             = true;
    failed.msdf_text_atlas_ready             = true;
    failed.msdf_text_texture_ready           = true;
    failed.msdf_text_texture_uploaded        = true;
    failed.msdf_text_cache_miss              = true;
    failed.msdf_text_atlas_build_attempted   = true;
    failed.msdf_text_atlas_build_succeeded   = true;
    failed.msdf_text_baked_pixel_height      = 48;
    failed.msdf_text_atlas_generation        = 5U;
    failed.msdf_text_resources_ready         = true;
    failed.msdf_text_renderer_active         = true;
    failed.msdf_text_font_data_bytes         = 8192;
    failed.msdf_text_pixel_height            = 21;
    failed.msdf_text_atlas_size              = 2048;
    failed.msdf_text_px_range                = 10.0f;
    failed.msdf_text_message = QStringLiteral(
        "MSDF text resources unavailable; using shaped glyph fallback");

    term::Qsg_atlas_render_summary populated_retry;
    populated_retry.msdf_text_supported_runs          = 1;
    populated_retry.msdf_text_missed_supported_runs   = 1;
    populated_retry.msdf_text_missed_supported_glyphs = 1;
    populated_retry.msdf_text_message                 = QStringLiteral("ok");

    term::qsg_atlas_merge_msdf_text_failure_diagnostics(
        failed,
        populated_retry);

    bool ok = true;
    ok &= check(populated_retry.msdf_text_supported_runs == 3 &&
            populated_retry.msdf_text_missed_supported_runs == 3 &&
            populated_retry.msdf_text_missed_supported_glyphs == 8,
        "MSDF diagnostics merge preserves failure counters across populated retry");
    ok &= check(populated_retry.msdf_text_atlas_built &&
            populated_retry.msdf_text_atlas_ready &&
            populated_retry.msdf_text_texture_ready &&
            populated_retry.msdf_text_texture_uploaded &&
            populated_retry.msdf_text_cache_miss &&
            populated_retry.msdf_text_atlas_build_attempted &&
            populated_retry.msdf_text_atlas_build_succeeded,
        "MSDF diagnostics merge preserves resource and baked-cache evidence");
    ok &= check(!populated_retry.msdf_text_resources_ready &&
            !populated_retry.msdf_text_renderer_active,
        "MSDF diagnostics merge keeps fallback resource state inactive");
    ok &= check(populated_retry.msdf_text_baked_pixel_height == 48 &&
            populated_retry.msdf_text_atlas_generation == 5U &&
            populated_retry.msdf_text_font_data_bytes == 8192 &&
            populated_retry.msdf_text_pixel_height == 21 &&
            populated_retry.msdf_text_atlas_size == 2048 &&
            std::abs(populated_retry.msdf_text_px_range - 10.0f) <= 0.0001f,
        "MSDF diagnostics merge preserves sizing diagnostics");
    ok &= check(
        populated_retry.msdf_text_message == failed.msdf_text_message,
        "MSDF diagnostics merge preserves failure message across populated retry");
    return ok;
}

bool test_text_renderer_report_names()
{
    bool ok = true;
    ok &= check(
        std::strcmp(
            term::qsg_atlas_text_renderer_policy_name(
                term::Terminal_text_renderer_policy::AUTO),
            "auto") == 0,
        "text renderer policy name reports auto");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_text_renderer_policy_name(
                term::Terminal_text_renderer_policy::MSDF),
            "msdf") == 0,
        "text renderer policy name reports msdf");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_text_renderer_policy_name(
                term::Terminal_text_renderer_policy::GLYPH),
            "glyph") == 0,
        "text renderer policy name reports glyph");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_text_renderer_kind_name(
                term::Terminal_text_renderer_kind::NONE),
            "none") == 0,
        "effective text renderer name reports none");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_text_renderer_kind_name(
                term::Terminal_text_renderer_kind::MIXED),
            "mixed") == 0,
        "effective text renderer name reports mixed");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_lcd_subpixel_order_name(
                term::Terminal_lcd_subpixel_order::NONE),
            "none") == 0,
        "LCD subpixel order name reports none");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_lcd_subpixel_order_name(
                term::Terminal_lcd_subpixel_order::RGB),
            "rgb") == 0,
        "LCD subpixel order name reports rgb");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_lcd_subpixel_order_name(
                term::Terminal_lcd_subpixel_order::BGR),
            "bgr") == 0,
        "LCD subpixel order name reports bgr");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_lcd_subpixel_order_name(
                term::Terminal_lcd_subpixel_order::VRGB),
            "vrgb") == 0,
        "LCD subpixel order name reports vrgb");
    ok &= check(
        std::strcmp(
            term::qsg_atlas_lcd_subpixel_order_name(
                term::Terminal_lcd_subpixel_order::VBGR),
            "vbgr") == 0,
        "LCD subpixel order name reports vbgr");
    return ok;
}

bool test_atlas_budget_stats()
{
    const term::Glyph_rgba_tile tile = test_rgba_tile(
        QSize(2, 2),
        8,
        {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4});

    term::Glyph_atlas_cache cache(QSize(4, 4));
    cache.set_epoch(1U);
    const term::Glyph_atlas_slot first = cache.insert_or_get(
        term::qsg_atlas_cache_key(10U, QStringLiteral("face"), 12.0, 0),
        tile);
    const term::Glyph_atlas_slot second = cache.insert_or_get(
        term::qsg_atlas_cache_key(11U, QStringLiteral("face"), 12.0, 0),
        tile);
    const term::Glyph_atlas_slot third = cache.insert_or_get(
        term::qsg_atlas_cache_key(12U, QStringLiteral("face"), 12.0, 0),
        tile);
    const term::Glyph_atlas_slot fourth = cache.insert_or_get(
        term::qsg_atlas_cache_key(13U, QStringLiteral("face"), 12.0, 0),
        tile);
    const term::Glyph_atlas_slot fifth = cache.insert_or_get(
        term::qsg_atlas_cache_key(14U, QStringLiteral("face"), 12.0, 0),
        tile);
    const term::Glyph_atlas_cache_stats stats = cache.stats();
    const term::Glyph_rgba_cache_accounting rgba_accounting =
        term::qsg_atlas_rgba_cache_accounting(stats);
    const term::Glyph_atlas_cache_stats stats_after_accounting = cache.stats();

    bool ok = true;
    ok &= check(first.is_valid() &&
            second.is_valid()     &&
            third.is_valid()      &&
            fourth.is_valid()     &&
            !fifth.is_valid(),
        "atlas budget test reaches the RGBA page cap");
    ok &= check(stats.page_budget == 4 &&
            stats.page_count == 4 &&
            stats.page_bytes == 64U &&
            stats.allocated_bytes == 256U &&
            stats.budget_bytes == 256U,
        "atlas stats report page count, allocation, and budget bytes");
    ok &= check(stats.used_bytes == 64U && stats.failed_inserts == 1U,
        "atlas stats report used RGBA bytes and failed inserts");
    ok &= check(term::qsg_atlas_rgba_tile_byte_count(QSize(4, 4)) == 64U &&
            term::qsg_atlas_rgba_tile_byte_count(QSize(2, 2)) == 16U &&
            term::qsg_atlas_rgba_tile_byte_count(QSize(0, 4)) == 0U,
        "RGBA atlas byte count maps tile area to four bytes per pixel");
    ok &= check(rgba_accounting.page_bytes == stats.page_bytes &&
            rgba_accounting.allocated_bytes == stats.allocated_bytes &&
            rgba_accounting.budget_bytes == stats.budget_bytes &&
            rgba_accounting.used_bytes == stats.used_bytes,
        "RGBA atlas accounting reflects canonical cache stats");
    ok &= check(stats_after_accounting.page_bytes == stats.page_bytes &&
            stats_after_accounting.allocated_bytes == stats.allocated_bytes &&
            stats_after_accounting.budget_bytes == stats.budget_bytes &&
            stats_after_accounting.used_bytes == stats.used_bytes &&
            stats_after_accounting.failed_inserts == stats.failed_inserts,
        "RGBA atlas accounting leaves cache stats unchanged");
    return ok;
}

QString warm_seed_qstring(const term::qsg_atlas_warm_seed_string_t& seed)
{
    return QString::fromUtf16(
        seed.text.data(),
        static_cast<qsizetype>(seed.text.size()));
}

bool warm_set_has_family(std::string_view family)
{
    return std::any_of(
        term::k_qsg_atlas_warm_seed_strings.begin(),
        term::k_qsg_atlas_warm_seed_strings.end(),
        [family](const term::qsg_atlas_warm_seed_string_t& seed) {
            return seed.family == family;
        });
}

struct Warm_family_coverage_result
{
    int shaped_records       = 0;
    int inserted_records     = 0;
    int unsupported_records  = 0;
    int non_rendering_records = 0;
    int failed_inserts       = 0;
};

struct Warm_required_probe
{
    std::string_view    label;
    std::u16string_view text;
    bool                allow_environment_skip = false;
};

bool warm_seed_source_range_is_non_rendering(
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
        const QChar ch = text.at(index);
        if (ch.isSpace()) {
            continue;
        }

        switch (ch.category()) {
            case QChar::Mark_NonSpacing:
            case QChar::Mark_SpacingCombining:
            case QChar::Mark_Enclosing:
            case QChar::Other_Format:
            case QChar::Other_Control:
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool warm_coverage_is_concrete(const Warm_family_coverage_result& coverage)
{
    return coverage.inserted_records > 0 && coverage.failed_inserts == 0;
}

bool warm_family_allows_environment_skip(std::string_view family)
{
    return
        family == std::string_view("pua_powerline_nerd") ||
        family == std::string_view("emoji_clusters");
}

Warm_family_coverage_result warm_family_coverage(
    const term::qsg_atlas_warm_seed_string_t& seed,
    const QFont&                              font,
    term::terminal_cell_metrics_t             metrics)
{
    const QString text = warm_seed_qstring(seed);
    term::Terminal_render_text_run run;
    run.row             = 0;
    run.logical_row     = 0;
    run.column          = 0;
    run.text            = text;
    run.foreground      = QColor(Qt::white);
    run.background      = QColor(Qt::transparent);
    run.rect            = QRectF(
        0.0,
        0.0,
        static_cast<qreal>(std::max<qsizetype>(1, text.size())) *
            metrics.width,
        metrics.height);
    run.baseline_origin = QPointF(0.0, metrics.ascent);

    const bool emoji_presentation_run =
        seed.family == std::string_view("emoji_clusters");
    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(run, font, metrics, 1.0, 0, false);

    Warm_family_coverage_result result;
    result.shaped_records = static_cast<int>(shaped.records.size());
    term::Glyph_atlas_cache cache(QSize(256, 256));
    cache.set_epoch(1U);
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.glyph_bounds.width() <= 0.0 ||
            record.glyph_bounds.height() <= 0.0)
        {
            if (warm_seed_source_range_is_non_rendering(
                    text,
                    record.source_string_start,
                    record.source_string_end))
            {
                ++result.non_rendering_records;
            }
            else {
                ++result.unsupported_records;
            }
            continue;
        }

        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(record.physical_pixel_size);
        term::Glyph_image_presentation presentation =
            emoji_presentation_run
                ? term::Glyph_image_presentation::COLOR
                : term::Glyph_image_presentation::TEXT;

        const QRawFont::AntialiasingType antialiasing =
            presentation == term::Glyph_image_presentation::TEXT
                ? QRawFont::SubPixelAntialiasing
                : QRawFont::PixelAntialiasing;
        const QImage alpha_map =
            raster_font.alphaMapForGlyph(record.glyph_index, antialiasing);
        const term::Glyph_rgba_tile tile =
            term::qsg_atlas_rgba_tile_from_image(alpha_map, presentation);
        if (!tile.is_valid()) {
            ++result.unsupported_records;
            continue;
        }

        const term::Glyph_atlas_slot slot = cache.insert_or_get(
            term::qsg_atlas_cache_key(
                record.glyph_index,
                record.fallback_face_id,
                record.physical_pixel_size,
                0,
                tile.coverage_kind,
                presentation),
            tile,
            term::qsg_atlas_glyph_physical_offset_for_raster_font(
                raster_font,
                record.glyph_index,
                presentation));
        if (slot.is_valid()) {
            ++result.inserted_records;
        }
        else {
            ++result.failed_inserts;
        }
    }
    return result;
}

bool test_atlas_warm_set_table_and_shaping()
{
    const term::terminal_cell_metrics_t metrics = pixel_metrics(1.0);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);

    std::size_t code_units = 0U;
    int shaped_records     = 0;
    bool ok                = true;
    for (const term::qsg_atlas_warm_seed_string_t& seed :
        term::k_qsg_atlas_warm_seed_strings)
    {
        const QString text = warm_seed_qstring(seed);
        code_units += static_cast<std::size_t>(text.size());
        term::Terminal_render_text_run run;
        run.row             = 0;
        run.logical_row     = 0;
        run.column          = 0;
        run.text            = text;
        run.foreground      = QColor(Qt::white);
        run.background      = QColor(Qt::transparent);
        run.rect            = QRectF(
            0.0,
            0.0,
            static_cast<qreal>(std::max<qsizetype>(1, text.size())) *
                metrics.width,
            metrics.height);
        run.baseline_origin = QPointF(0.0, metrics.ascent);

        const term::Qsg_atlas_shaped_text_run_result shaped =
            term::qsg_atlas_shape_text_run(run, font, metrics, 1.0, 0, false);
        shaped_records += static_cast<int>(shaped.records.size());
        ok &= check(!text.isEmpty(), "warm-set seed strings are non-empty");
        ok &= check(!shaped.records.empty(),
            "warm-set seed strings shape through Qt");
    }

    ok &= check(
        term::k_qsg_atlas_warm_seed_strings.size() <=
            term::k_qsg_atlas_warm_seed_string_budget,
        "warm-set seed string count stays within the source budget");
    ok &= check(
        code_units <= term::k_qsg_atlas_warm_seed_code_unit_budget,
        "warm-set seed code-unit count stays within the source budget");
    ok &= check(
        shaped_records <=
            static_cast<int>(term::k_qsg_atlas_warm_seed_shaped_record_budget),
        "warm-set shaped record count stays within the source budget");

    for (std::string_view family : {
        std::string_view("ascii_common"),
        std::string_view("latin_1"),
        std::string_view("latin_extended_a"),
        std::string_view("greek"),
        std::string_view("cyrillic"),
        std::string_view("symbols_currency_math"),
        std::string_view("terminal_graphics"),
        std::string_view("pua_powerline_nerd"),
        std::string_view("cjk_kana_hangul"),
        std::string_view("combining_clusters"),
        std::string_view("emoji_clusters"),
    }) {
        ok &= check(warm_set_has_family(family),
            "warm-set table contains every required family");
    }

    for (const term::qsg_atlas_warm_seed_string_t& seed :
        term::k_qsg_atlas_warm_seed_strings)
    {
        const Warm_family_coverage_result coverage =
            warm_family_coverage(seed, font, metrics);
        const bool family_covered = coverage.inserted_records > 0 &&
            coverage.failed_inserts == 0;
        if (!family_covered && warm_family_allows_environment_skip(seed.family)) {
            std::cerr << "SKIP: warm-set family " << seed.family
                << " has no concrete atlas entries on this font environment"
                << " shaped=" << coverage.shaped_records
                << " unsupported=" << coverage.unsupported_records
                << " non_rendering=" << coverage.non_rendering_records
                << " failed_inserts=" << coverage.failed_inserts
                << '\n';
            continue;
        }
        ok &= check(family_covered,
            "warm-set family shapes into concrete atlas entries");
    }
    return ok;
}

bool test_atlas_warm_set_representative_family_coverage()
{
    const term::terminal_cell_metrics_t metrics = pixel_metrics(1.0);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    constexpr std::array<Warm_required_probe, 11> probes = {{
        {"latin_1",              u"\u00e9",                         false},
        {"latin_extended_a",     u"\u0141",                         false},
        {"greek_cyrillic",       u"\u03a9\u0411",                   false},
        {"symbols_math_arrows",  u"\u20ac\u2192\u2211",             false},
        {"box_drawing",          u"\u2500\u2502\u251c",             false},
        {"block_elements",       u"\u2588\u258c\u2591",             false},
        {"braille",              u"\u2801\u28ff",                   false},
        {"pua_powerline_nerd",   u"\ue0b0\uf120",                   true},
        {"cjk_kana_hangul",      u"\u4e2d\u3042\uac00",             false},
        {"combining_clusters",   u"e\u0301",                        false},
        {"emoji_clusters",       u"\u2615\ufe0f\U0001f600",         true},
    }};

    bool ok = true;
    for (const Warm_required_probe& probe : probes) {
        const term::qsg_atlas_warm_seed_string_t seed{
            probe.text,
            probe.label,
        };
        const Warm_family_coverage_result coverage =
            warm_family_coverage(seed, font, metrics);
        if (!warm_coverage_is_concrete(coverage) &&
            probe.allow_environment_skip)
        {
            std::cerr << "SKIP: warm-set probe " << probe.label
                << " has no concrete atlas entries on this font environment"
                << " shaped=" << coverage.shaped_records
                << " unsupported=" << coverage.unsupported_records
                << " non_rendering=" << coverage.non_rendering_records
                << " failed_inserts=" << coverage.failed_inserts
                << '\n';
            continue;
        }
        ok &= check(warm_coverage_is_concrete(coverage),
            "warm-set representative probe shapes into atlas entries");
    }
    return ok;
}

bool atlas_report_has_prepare_glyph_coverage_evidence(
    const term::Qsg_atlas_frame_report& report)
{
    if (report.raw_font_rasterized_in_prepare &&
        report.prepare_thread_id == report.raw_font_raster_thread_id)
    {
        return true;
    }

    const term::Qsg_atlas_warm_lazy_summary& warm_lazy = report.warm_lazy;
    return
        warm_lazy.warm_completed                &&
        warm_lazy.warm_epoch == report.render_font_epoch &&
        warm_lazy.warm_covered_glyph_records > 0 &&
        warm_lazy.warm_failed_glyph_records == 0 &&
        warm_lazy.warm_missing_string_indexes == 0 &&
        warm_lazy.warm_invalid_string_indexes == 0 &&
        warm_lazy.warm_unsupported_images == 0 &&
        warm_lazy.warm_failed_inserts == 0;
}

bool atlas_report_render_state_ready(const term::Qsg_atlas_frame_report& report)
{
    return
        report.prepare_count > 0U       &&
        report.render_count > 0U        &&
        report.drew                     &&
        report.command_buffer_non_null  &&
        report.render_target_non_null   &&
        report.rhi_non_null;
}

bool test_atlas_recorder_accumulates_elapsed_counters()
{
    term::Qsg_atlas_recorder recorder;
    const term::Captured_atlas_frame frame;
    const term::Glyph_atlas_cache_stats cache;
    const term::Qsg_atlas_frame_build_summary frame_build;
    const term::Qsg_atlas_render_summary render_summary;
    const term::Qsg_atlas_producer_summary producer_summary;
    const term::Qsg_atlas_warm_lazy_summary warm_lazy_summary;

    const auto record_prepare = [&](std::uint64_t elapsed_ns) {
        recorder.record_prepare(
            frame,
            elapsed_ns,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            0,
            0U,
            0U,
            cache,
            frame_build,
            render_summary,
            producer_summary,
            warm_lazy_summary);
    };

    record_prepare(11U);
    record_prepare(13U);
    recorder.record_render(frame, 17U, QRect(0, 0, 1, 1), true);
    recorder.record_render(frame, 19U, QRect(0, 0, 1, 1), true);

    const term::Qsg_atlas_frame_report report = recorder.snapshot();
    bool ok = true;
    ok &= check(report.prepare_count == 2U,
        "atlas recorder counts prepare records");
    ok &= check(report.prepare_elapsed_ns == 24U,
        "atlas recorder accumulates prepare elapsed time");
    ok &= check(report.render_count == 2U,
        "atlas recorder counts render records");
    ok &= check(report.render_elapsed_ns == 36U,
        "atlas recorder accumulates render elapsed time");
    return ok;
}

bool pump_until(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    const std::function<bool(const term::Qsg_atlas_frame_report&)>&
                        predicate,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (predicate(report)) {
            return true;
        }
    }

    return false;
}

bool pump_next_atlas_report(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    std::uint64_t       previous_prepare_count,
    term::Qsg_atlas_frame_report& out_report,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (report.prepare_count > previous_prepare_count) {
            out_report = report;
            return true;
        }
    }

    return false;
}

bool atlas_report_matches_sequence(
    const term::Qsg_atlas_frame_report& report,
    std::uint64_t                       sequence)
{
    return
        report.captured_snapshot_sequence == sequence &&
        report.render_snapshot_sequence == sequence;
}

bool pump_next_atlas_report_for_sequence(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    std::uint64_t       previous_prepare_count,
    std::uint64_t       expected_sequence,
    term::Qsg_atlas_frame_report& out_report,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (report.prepare_count > previous_prepare_count &&
            atlas_report_matches_sequence(report, expected_sequence))
        {
            out_report = report;
            return true;
        }
    }

    return false;
}

class Direct_atlas_item final : public QQuickItem
{
public:
    Direct_atlas_item(
        term::terminal_cell_metrics_t  metrics,
        QFont                          font,
        QSizeF                         logical_size)
    :
        m_metrics(metrics),
        m_font(std::move(font))
    {
        setFlag(QQuickItem::ItemHasContents, true);
        setSize(logical_size);
    }

    std::uint64_t set_frame(
        std::shared_ptr<const term::Terminal_render_snapshot>
                                      snapshot,
        term::Terminal_render_options options)
    {
        m_snapshot = std::move(snapshot);
        m_options  = std::move(options);
        ++m_capture_sequence;
        update();
        return m_capture_sequence;
    }

    term::Qsg_atlas_frame_report report() const
    {
        return m_recorder->snapshot();
    }

protected:
    QSGNode* updatePaintNode(
        QSGNode*           old_node,
        UpdatePaintNodeData*) override
    {
        QQuickWindow* const current_window = window();
        const qreal dpr = current_window != nullptr
            ? pixel_window_device_pixel_ratio(*current_window)
            : 1.0;

        term::Captured_atlas_frame frame = term::capture_qsg_atlas_frame(
            m_snapshot,
            {},
            m_options,
            m_metrics,
            size(),
            m_font,
            nullptr,
            dpr,
            m_font_epoch,
            m_capture_sequence,
            true);
        return term::update_qsg_atlas_node(
            old_node,
            std::move(frame),
            m_recorder);
    }

private:
    std::shared_ptr<term::Qsg_atlas_recorder> m_recorder =
        std::make_shared<term::Qsg_atlas_recorder>();
    std::shared_ptr<const term::Terminal_render_snapshot>
                                      m_snapshot;
    term::Terminal_render_options     m_options;
    term::terminal_cell_metrics_t     m_metrics{};
    QFont                             m_font;
    std::uint64_t                     m_capture_sequence = 0U;
    std::uint64_t                     m_font_epoch       = 1U;
};

bool pump_next_direct_atlas_report(
    QGuiApplication&                app,
    QQuickWindow&                   window,
    Direct_atlas_item&              item,
    std::uint64_t                   previous_prepare_count,
    std::uint64_t                   expected_capture_sequence,
    term::Qsg_atlas_frame_report&   out_report,
    int                             attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        item.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_frame_report report = item.report();
        if (report.prepare_count > previous_prepare_count &&
            report.capture_sequence == expected_capture_sequence)
        {
            out_report = report;
            return true;
        }
    }

    return false;
}

bool pump_direct_atlas_until(
    QGuiApplication&    app,
    QQuickWindow&       window,
    Direct_atlas_item&  item,
    const std::function<bool(const term::Qsg_atlas_frame_report&)>&
                        predicate,
    term::Qsg_atlas_frame_report& out_report,
    int                 attempts = 120)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        item.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        const term::Qsg_atlas_frame_report report = item.report();
        if (predicate(report)) {
            out_report = report;
            return true;
        }
    }

    return false;
}

struct Atlas_host_state_capture
{
    QImage                       image;
    term::Qsg_atlas_frame_report report;
};

struct Atlas_host_state_result
{
    Atlas_host_state_capture capture;
    bool                     ok          = false;
    bool                     unsupported = false;
};

term::Terminal_render_snapshot make_atlas_host_state_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({2, 8}, sequence);
    snapshot.color_state.default_background_rgba = 0xff22cc60U;
    snapshot.color_state.default_foreground_rgba = 0xffffffffU;
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    return snapshot;
}

void configure_atlas_host_state_surface(
    VNM_TerminalSurface& surface,
    QSizeF               logical_size,
    std::uint64_t        sequence,
    bool                 use_default_terminal_font = false)
{
    surface.setClip(false);
    surface.setSize(logical_size);
    surface.set_font_family(
        use_default_terminal_font ? QString() : QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
        surface,
        true);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_host_state_snapshot(sequence)));
}

bool atlas_host_backend_unusable(
    const term::Qsg_atlas_frame_report& report)
{
    return
        report.prepare_count == 0U ||
        report.render_count  == 0U ||
        (report.prepare_count > 0U && !report.rhi_non_null);
}

bool is_atlas_host_pixel(const QColor& color)
{
    return
        color.green() >= 80             &&
        color.green() > color.red() + 45 &&
        color.green() > color.blue() + 25;
}

int count_atlas_host_pixels(
    const QImage& image,
    QRect         area)
{
    int count = 0;
    area = area.intersected(image.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (is_atlas_host_pixel(image.pixelColor(x, y))) {
                ++count;
            }
        }
    }

    return count;
}

QRect atlas_host_sample_rect_for_scene_point(
    const QQuickWindow& window,
    QPointF             scene_point,
    int                 radius)
{
    const qreal dpr = pixel_window_device_pixel_ratio(window);
    const QPoint center(
        static_cast<int>(std::lround(scene_point.x() * dpr)),
        static_cast<int>(std::lround(scene_point.y() * dpr)));
    return QRect(
        center.x() - radius,
        center.y() - radius,
        radius * 2 + 1,
        radius * 2 + 1);
}

void print_atlas_host_state_report(
    const char*                     case_name,
    const Atlas_host_state_capture& capture)
{
    const term::Qsg_atlas_frame_report& report = capture.report;
    std::cout
        << "atlas host-state " << case_name
        << ": prepare=" << report.prepare_count
        << " render=" << report.render_count
        << " rhi=" << report.rhi_non_null
        << " command_buffer=" << report.command_buffer_non_null
        << " render_target=" << report.render_target_non_null
        << " viewport=" << report.viewport_rect.x()
        << ',' << report.viewport_rect.y()
        << ' ' << report.viewport_rect.width()
        << 'x' << report.viewport_rect.height()
        << " image=" << capture.image.width()
        << 'x' << capture.image.height()
        << " rect_draws=" << report.render.rect_draw_calls
        << " glyph_draws=" << report.render.glyph_draw_calls
        << " msdf_text_draws=" << report.render.msdf_text_draw_calls
        << " draw_calls=" << report.render.draw_calls
        << '\n';
}

bool check_atlas_host_state_report(
    const char*                     case_name,
    const Atlas_host_state_capture& capture)
{
    print_atlas_host_state_report(case_name, capture);

    const term::Qsg_atlas_frame_report& report = capture.report;
    const std::string prefix =
        std::string("atlas host-state ") + case_name + ' ';
    bool ok = true;
    ok &= check(atlas_report_render_state_ready(report),
        prefix + "reaches usable QRhi render state");
    ok &= check(!capture.image.isNull(),
        prefix + "produces a captured window image");
    ok &= check(report.viewport_rect.width() > 0 &&
            report.viewport_rect.height() > 0,
        prefix + "reports a positive render target viewport");
    ok &= check(report.render.draw_calls > 0 &&
            report.render.rect_draw_calls > 0 &&
            (report.render.glyph_draw_calls > 0 ||
                report.render.msdf_text_draw_calls > 0),
        prefix + "reports non-empty rect and text draw calls");
    return ok;
}

bool pump_atlas_host_state_surface(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    Atlas_host_state_capture& out,
    const std::function<bool(const Atlas_host_state_capture&)>& predicate,
    int                 attempts = 160)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out.image = window.grabWindow();
        out.report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (out.image.isNull() ||
            !atlas_report_render_state_ready(out.report))
        {
            continue;
        }

        if (predicate(out)) {
            return true;
        }
    }

    return false;
}

Atlas_host_state_result test_atlas_transformed_host_state(
    QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(3, 7, 11));
    window.resize(260, 180);

    QQuickItem host;
    host.setParentItem(window.contentItem());
    host.setPosition(QPointF(20.0, 18.0));
    host.setScale(1.5);
    host.setTransformOrigin(QQuickItem::TopLeft);
    host.setOpacity(0.65);
    host.setSize(QSizeF(80.0, 60.0));

    VNM_TerminalSurface surface;
    surface.setParentItem(&host);
    configure_atlas_host_state_surface(
        surface,
        QSizeF(80.0, 60.0),
        981U,
        true);

    window.show();
    Atlas_host_state_result result;
    const QRect inside_transformed = atlas_host_sample_rect_for_scene_point(
        window,
        host.mapToScene(QPointF(60.0, 30.0)),
        4);
    const bool rendered = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        result.capture,
        [&](const Atlas_host_state_capture& capture) {
            return count_atlas_host_pixels(capture.image, inside_transformed) >
                20;
        });
    if (!rendered) {
        result.unsupported = atlas_host_backend_unusable(result.capture.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas transformed host did not render expected "
                << "captured pixels\n";
            print_atlas_host_state_report("transformed", result.capture);
        }
        return result;
    }

    const QRect origin_probe = atlas_host_sample_rect_for_scene_point(
        window,
        QPointF(5.0, 50.0),
        4);
    bool ok = check_atlas_host_state_report("transformed", result.capture);
    ok &= check(count_atlas_host_pixels(result.capture.image, inside_transformed) >
            20,
        "atlas transformed host renders through projectionMatrix() * matrix()");
    ok &= check(count_atlas_host_pixels(result.capture.image, origin_probe) == 0,
        "atlas transformed host does not draw at the untransformed origin");
    // The MSDF text path requires dual-source blending. On backends that lack
    // it (e.g. the software/WARP D3D11 device on GitHub Windows runners) the
    // renderer correctly falls back to the grayscale glyph path, so only
    // require MSDF to be exercised when the device actually advertises the
    // capability that makes it reachable.
    if (result.capture.report.render.msdf_text_renderer_enabled &&
        result.capture.report.render.dual_source_blend_factors_available) {
        ok &= check(
            result.capture.report.render.msdf_text_renderer_active &&
                result.capture.report.render.msdf_text_runs > 0 &&
                result.capture.report.render.msdf_text_draw_calls > 0,
            "atlas transformed host exercises the MSDF renderer path");
    }
    result.ok = ok;
    return result;
}

Atlas_host_state_result test_atlas_scissor_host_state(
    QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(2, 6, 10));
    window.resize(240, 160);

    QQuickItem clip_host;
    clip_host.setParentItem(window.contentItem());
    clip_host.setPosition(QPointF(30.0, 30.0));
    clip_host.setSize(QSizeF(60.0, 45.0));
    clip_host.setClip(true);

    VNM_TerminalSurface surface;
    surface.setParentItem(&clip_host);
    configure_atlas_host_state_surface(surface, QSizeF(130.0, 80.0), 982U);

    window.show();
    Atlas_host_state_result result;
    const QRect inside_clip = atlas_host_sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(10.0, 10.0)),
        4);
    const QRect outside_clip = atlas_host_sample_rect_for_scene_point(
        window,
        surface.mapToScene(QPointF(96.0, 12.0)),
        4);
    const bool rendered = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        result.capture,
        [&](const Atlas_host_state_capture& capture) {
            return count_atlas_host_pixels(capture.image, inside_clip) > 20;
        });
    if (!rendered) {
        result.unsupported = atlas_host_backend_unusable(result.capture.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas scissor host did not render expected "
                << "captured pixels inside the clip\n";
            print_atlas_host_state_report("scissor", result.capture);
        }
        return result;
    }

    bool ok = check_atlas_host_state_report("scissor", result.capture);
    ok &= check(count_atlas_host_pixels(result.capture.image, inside_clip) > 20,
        "atlas scissor host renders inside the rectangular clip");
    ok &= check(count_atlas_host_pixels(result.capture.image, outside_clip) == 0,
        "atlas scissor host rejects pixels outside state->scissorRect()");
    result.ok = ok;
    return result;
}

Atlas_host_state_result test_atlas_stencil_host_state(
    QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(4, 8, 12));
    window.resize(280, 220);

    QQuickItem clip_host;
    clip_host.setParentItem(window.contentItem());
    clip_host.setPosition(QPointF(80.0, 54.0));
    clip_host.setSize(QSizeF(70.0, 54.0));
    clip_host.setClip(true);
    clip_host.setTransformOrigin(QQuickItem::TopLeft);
    clip_host.setRotation(16.0);

    VNM_TerminalSurface surface;
    surface.setParentItem(&clip_host);
    configure_atlas_host_state_surface(surface, QSizeF(130.0, 82.0), 983U);

    window.show();
    Atlas_host_state_result result;
    const QRect inside_clip = atlas_host_sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(30.0, 25.0)),
        4);
    const bool rendered = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        result.capture,
        [&](const Atlas_host_state_capture& capture) {
            return count_atlas_host_pixels(capture.image, inside_clip) > 20;
        });
    if (!rendered) {
        result.unsupported = atlas_host_backend_unusable(result.capture.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas stencil host did not render expected "
                << "captured pixels inside the rotated clip\n";
            print_atlas_host_state_report("stencil", result.capture);
        }
        return result;
    }

    const QRect outside_right = atlas_host_sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(100.0, 12.0)),
        4);
    const QRect outside_lower = atlas_host_sample_rect_for_scene_point(
        window,
        clip_host.mapToScene(QPointF(92.0, 45.0)),
        4);

    bool ok = check_atlas_host_state_report("stencil", result.capture);
    ok &= check(count_atlas_host_pixels(result.capture.image, inside_clip) > 20,
        "atlas stencil host renders inside the rotated clip");
    ok &= check(count_atlas_host_pixels(result.capture.image, outside_right) == 0,
        "atlas stencil host rejects pixels right of the rotated clip");
    ok &= check(count_atlas_host_pixels(result.capture.image, outside_lower) == 0,
        "atlas stencil host rejects pixels below the rotated clip body");
    result.ok = ok;
    return result;
}

Atlas_host_state_result test_atlas_layer_host_state(
    QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(1, 5, 9));
    window.resize(320, 200);

    QQuickItem layer_host;
    layer_host.setParentItem(window.contentItem());
    layer_host.setPosition(QPointF(26.0, 24.0));
    layer_host.setSize(QSizeF(110.0, 70.0));
    QQuickItemPrivate::get(&layer_host)->layer()->setEnabled(true);

    VNM_TerminalSurface surface;
    surface.setParentItem(&layer_host);
    configure_atlas_host_state_surface(surface, QSizeF(100.0, 60.0), 984U);

    window.show();
    Atlas_host_state_result result;
    const QRect inside_layer = atlas_host_sample_rect_for_scene_point(
        window,
        layer_host.mapToScene(QPointF(40.0, 30.0)),
        4);
    const bool rendered = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        result.capture,
        [&](const Atlas_host_state_capture& capture) {
            return count_atlas_host_pixels(capture.image, inside_layer) > 20;
        });
    if (!rendered) {
        result.unsupported = atlas_host_backend_unusable(result.capture.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas layer host did not render expected "
                << "captured pixels\n";
            print_atlas_host_state_report("layer", result.capture);
        }
        return result;
    }

    bool ok = check_atlas_host_state_report("layer", result.capture);
    ok &= check(count_atlas_host_pixels(result.capture.image, inside_layer) > 20,
        "atlas layer host composites the redirected atlas render target");
    ok &= check(result.capture.report.viewport_rect.width() <
            result.capture.image.width(),
        "atlas layer host reports a redirected render target width");
    ok &= check(result.capture.report.viewport_rect.height() <
            result.capture.image.height(),
        "atlas layer host reports a redirected render target height");
    result.ok = ok;
    return result;
}

Atlas_host_state_result test_atlas_layer_toggle_host_state(
    QGuiApplication& app)
{
    QQuickWindow window;
    window.setColor(QColor(6, 10, 14));
    window.resize(320, 200);

    QQuickItem layer_host;
    layer_host.setParentItem(window.contentItem());
    layer_host.setPosition(QPointF(24.0, 22.0));
    layer_host.setSize(QSizeF(110.0, 70.0));

    VNM_TerminalSurface surface;
    surface.setParentItem(&layer_host);
    configure_atlas_host_state_surface(surface, QSizeF(100.0, 60.0), 985U);

    window.show();
    Atlas_host_state_result result;
    Atlas_host_state_capture before_layer;
    const QRect inside_layer = atlas_host_sample_rect_for_scene_point(
        window,
        layer_host.mapToScene(QPointF(40.0, 30.0)),
        4);
    const bool rendered_before = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        before_layer,
        [&](const Atlas_host_state_capture& capture) {
            return count_atlas_host_pixels(capture.image, inside_layer) > 20;
        });
    if (!rendered_before) {
        result.capture     = before_layer;
        result.unsupported = atlas_host_backend_unusable(before_layer.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas layer-toggle host did not render before "
                << "enabling the layer\n";
            print_atlas_host_state_report("layer-toggle-before", before_layer);
        }
        return result;
    }

    const std::uint64_t before_prepare_count = before_layer.report.prepare_count;
    QQuickItemPrivate::get(&layer_host)->layer()->setEnabled(true);
    layer_host.update();
    surface.update();

    const bool rendered_after = pump_atlas_host_state_surface(
        app,
        window,
        surface,
        result.capture,
        [&](const Atlas_host_state_capture& capture) {
            return capture.report.prepare_count > before_prepare_count &&
                count_atlas_host_pixels(capture.image, inside_layer) > 20;
        });
    if (!rendered_after) {
        result.unsupported = atlas_host_backend_unusable(result.capture.report);
        if (!result.unsupported) {
            std::cerr << "FAIL: atlas layer-toggle host did not render after "
                << "enabling the layer\n";
            print_atlas_host_state_report("layer-toggle-after", result.capture);
        }
        return result;
    }

    bool ok = check_atlas_host_state_report(
        "layer-toggle-before",
        before_layer);
    ok &= check_atlas_host_state_report("layer-toggle-after", result.capture);
    ok &= check(before_layer.report.viewport_rect.width() >=
            before_layer.image.width(),
        "atlas layer-toggle starts on the window render target");
    ok &= check(result.capture.report.viewport_rect.width() <
            result.capture.image.width(),
        "atlas layer-toggle redirects to a smaller layer render target");
    ok &= check(result.capture.report.prepare_count > before_prepare_count,
        "atlas layer-toggle produces a new prepared frame after enabling layer");
    result.ok = ok;
    return result;
}

int test_atlas_host_state_smoke(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas host-state smoke");
    if (backend_status != 0) {
        return backend_status;
    }

    bool ok = true;
    const auto run_case = [&](Atlas_host_state_result result) {
        if (result.unsupported) {
            std::cerr << "SKIP: atlas host-state smoke did not reach usable "
                << "QRhi render state on " << backend << '\n';
            return false;
        }

        ok &= result.ok;
        return true;
    };

    if (!run_case(test_atlas_transformed_host_state(app))) {
        return k_unsupported_backend_skip_return_code;
    }
    if (!run_case(test_atlas_scissor_host_state(app))) {
        return k_unsupported_backend_skip_return_code;
    }
    if (!run_case(test_atlas_stencil_host_state(app))) {
        return k_unsupported_backend_skip_return_code;
    }
    if (!run_case(test_atlas_layer_host_state(app))) {
        return k_unsupported_backend_skip_return_code;
    }
    if (!run_case(test_atlas_layer_toggle_host_state(app))) {
        return k_unsupported_backend_skip_return_code;
    }

    return ok ? 0 : 1;
}

void mutate_surface_after_sync(VNM_TerminalSurface& surface)
{
    surface.set_color_scheme(QStringLiteral("Campbell"));
    surface.set_font_size(surface.font_size() + 1.0);
    term::Terminal_render_snapshot mutation_snapshot =
        make_text_snapshot(777U, QStringLiteral("B"));
    mutation_snapshot.metadata.publication_generation = 1777U;
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            std::move(mutation_snapshot)));
}

int test_render_smoke(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas render smoke");
    if (backend_status != 0) {
        return backend_status;
    }

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
    surface.set_color_scheme(QStringLiteral("Solarized Light"));
    term::Terminal_render_snapshot publication_snapshot =
        make_text_snapshot(601U, QStringLiteral("A"));
    publication_snapshot.metadata.publication_generation = 1601U;
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            std::move(publication_snapshot)));
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

    term::Qsg_atlas_frame_report report;
    bool render_state_observed = false;
    const bool coverage_ready = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& current_report) {
            const bool render_ready =
                atlas_report_render_state_ready(current_report);
            if (render_ready || !render_state_observed) {
                report = current_report;
            }
            render_state_observed = render_state_observed || render_ready;
            return render_ready && mutation_done.load() &&
                atlas_report_has_prepare_glyph_coverage_evidence(
                    current_report);
        });
    QObject::disconnect(mutation_connection);

    if (!render_state_observed &&
        (report.prepare_count == 0U ||
            report.render_count == 0U ||
            (report.prepare_count > 0U && !report.rhi_non_null)))
    {
        std::cerr << "SKIP: atlas render smoke did not reach usable QRhi render state\n";
        return k_unsupported_backend_skip_return_code;
    }

    bool ok = true;
    ok &= check(atlas_report_render_state_ready(report),
        "atlas render smoke draws the captured frame");
    ok &= check(report.first_render_snapshot_sequence == 601U,
        "atlas render smoke uses captured snapshot for first render");
    const auto known_publication_generation = [](std::uint64_t generation) {
        return generation == 1601U || generation == 1777U;
    };
    ok &= check(known_publication_generation(report.captured_publication_generation) &&
            known_publication_generation(
                report.first_captured_publication_generation) &&
            known_publication_generation(report.render_publication_generation) &&
            known_publication_generation(report.first_render_publication_generation),
        "atlas render smoke carries publication generation through capture and render");
    ok &= check(report.first_render_snapshot_sequence ==
            report.first_captured_snapshot_sequence,
        "atlas first render uses the first captured snapshot value");
    ok &= check(report.first_render_light_options,
        "atlas render smoke uses captured options for first render");
    ok &= check(report.first_render_light_options == report.first_captured_light_options,
        "atlas first render uses the first captured options value");
    ok &= check(report.first_render_font_epoch == report.first_captured_font_epoch,
        "atlas first render uses the first captured font epoch");
    ok &= check(!threaded_loop || mutation_done.load(),
        "threaded atlas smoke mutates GUI-thread state after sync through beforeRendering");
    ok &= check(!basic_loop || mutation_done.load(),
        "basic atlas smoke mutates GUI state after sync through beforeRendering");
    ok &= check(coverage_ready &&
            atlas_report_has_prepare_glyph_coverage_evidence(report),
        "atlas smoke covers QRawFont glyphs through prepare");
    return ok ? 0 : 1;
}

term::Terminal_render_snapshot make_dense_x_grid_snapshot(
    int             rows,
    int             columns,
    std::uint64_t   sequence)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({rows, columns}, sequence);
    snapshot.color_state.default_foreground_rgba = 0xffffffffU;
    snapshot.color_state.default_background_rgba = 0xff000000U;
    snapshot.cells.reserve(static_cast<std::size_t>(rows * columns));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            snapshot.cells.push_back(make_pixel_cell(
                row,
                column,
                QStringLiteral("X"),
                1,
                term::k_default_terminal_style_id));
        }
    }
    return snapshot;
}

int test_dense_grid_smoke(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "dense atlas X grid");
    if (backend_status != 0) {
        return backend_status;
    }

    constexpr int   k_rows      = 28;
    constexpr int   k_columns   = 96;
    constexpr qreal k_font_size = 10.0;
    const QColor    background(0, 0, 0);

    QQuickWindow window;
    window.setColor(background);
    window.resize(640, 360);
    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);

    const qreal dpr = std::max<qreal>(1.0, window.effectiveDevicePixelRatio());
    if (std::abs(dpr - std::round(dpr)) <= 0.001) {
        std::cerr << "SKIP: dense atlas X grid needs fractional DPR, observed "
            << dpr << '\n';
        return k_unsupported_backend_skip_return_code;
    }

    term::Qt_grid_metrics_provider provider(
        term::vnm_terminal_font(QStringLiteral("monospace"), k_font_size),
        dpr);
    const term::terminal_cell_metrics_t metrics = provider.cell_metrics();
    const QSizeF logical_size(
        metrics.width * static_cast<qreal>(k_columns),
        metrics.height * static_cast<qreal>(k_rows));

    window.resize(
        static_cast<int>(std::ceil(logical_size.width())),
        static_cast<int>(std::ceil(logical_size.height())));

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(logical_size);
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(k_font_size);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
        surface,
        true);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_dense_x_grid_snapshot(k_rows, k_columns, 910U)));

    const bool rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    const term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!rendered &&
        (report.prepare_count == 0U ||
            report.render_count == 0U ||
            (report.prepare_count > 0U && !report.rhi_non_null)))
    {
        std::cerr << "SKIP: dense atlas X grid did not reach usable QRhi render "
            << "state on " << backend << '\n';
        return k_unsupported_backend_skip_return_code;
    }

    const qreal rendered_dpr = pixel_window_device_pixel_ratio(window);
    const QImage image = window.grabWindow();
    const qreal image_dpr = image.isNull()
        ? 0.0
        : pixel_normalized_device_pixel_ratio(image.devicePixelRatio());
    const QByteArray output_path = qgetenv("VNM_TERMINAL_DENSE_GRID_IMAGE");
    if (!output_path.isEmpty()) {
        image.save(QString::fromLocal8Bit(output_path));
    }

    bool ok = true;
    ok &= check(rendered,
        "dense atlas X grid reaches a rendered atlas frame");
    const int producer_glyph_records =
        report.producer.shaped_glyph_records_built +
        report.producer.shaped_glyph_records_reused;
    ok &= check(producer_glyph_records >= k_rows * k_columns,
        "dense atlas X grid uses canonical producer glyph records");
    ok &= check(report.producer.simple_path_used > 0,
        "dense atlas X grid uses the simple producer path for printable ASCII");
    ok &= check(report.warm_lazy.lazy_inserts <= 1,
        "dense atlas X grid reuses one visible-frame atlas coverage tile for repeated X glyphs");
    ok &= check(report.render.atlas_failed_inserts == 0U,
        "dense atlas X grid packs all required glyph coverage");
    ok &= check(!image.isNull(),
        "dense atlas X grid produces a captured window image");
    if (!pixel_device_pixel_ratios_match(dpr, rendered_dpr) ||
        (!image.isNull() && !pixel_device_pixel_ratios_match(dpr, image_dpr)))
    {
        std::cerr << "FAIL: dense atlas X grid DPR changed after resize/render"
            << " initial_dpr=" << dpr
            << " rendered_dpr=" << rendered_dpr
            << " image_dpr=" << image_dpr
            << " image_size=" << image.width() << 'x' << image.height()
            << '\n';
        ok = false;
    }
    if (!image.isNull()) {
        ok &= check_dense_grid_spacing(
            image,
            dpr,
            metrics,
            k_rows,
            k_columns,
            background);
    }
    return ok ? 0 : 1;
}

term::Terminal_render_snapshot make_cursor_descender_snapshot(
    std::uint64_t sequence,
    bool          cursor_visible)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({2, 6}, sequence);
    snapshot.color_state.default_foreground_rgba = 0xffffffffU;
    snapshot.color_state.default_background_rgba = 0xff000000U;
    snapshot.color_state.cursor_rgba             = 0xffffffffU;
    snapshot.cells.push_back(make_pixel_cell(
        0, 0, QStringLiteral("P"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(make_pixel_cell(
        0, 1, QStringLiteral("Q"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(make_pixel_cell(
        0, 2, QStringLiteral("R"), 1, term::k_default_terminal_style_id));
    snapshot.cursor.position      = {0, 1};
    snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    snapshot.cursor.visible       = cursor_visible;
    snapshot.cursor.blink_enabled = false;
    return snapshot;
}

struct Cursor_descender_capture
{
    QImage                        image;
    term::terminal_cell_metrics_t metrics{};
    qreal                         dpr = 1.0;
    term::Qsg_atlas_frame_report  report;
    bool                          software_adapter = false;
};

int render_cursor_descender_capture(
    QGuiApplication&          app,
    const char*               backend,
    bool                      cursor_visible,
    Cursor_descender_capture& out_capture)
{
    constexpr qreal k_font_size = 16.0;
    const QColor    background(0, 0, 0);

    QQuickWindow window;
    window.setColor(background);
    window.resize(320, 160);
    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);

    const qreal dpr = std::max<qreal>(1.0, window.effectiveDevicePixelRatio());
    const QString bundled_family =
        term::vnm_terminal_default_monospace_font_family();
    term::Qt_grid_metrics_provider provider(
        term::vnm_terminal_font(bundled_family, k_font_size),
        dpr);
    const term::terminal_cell_metrics_t metrics = provider.cell_metrics();
    const QSizeF logical_size(metrics.width * 6.0, metrics.height * 2.0);
    window.resize(
        static_cast<int>(std::ceil(logical_size.width())),
        static_cast<int>(std::ceil(logical_size.height())));

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(logical_size);
    surface.set_font_family(bundled_family);
    surface.set_font_size(k_font_size);
    surface.set_text_renderer_mode(
        VNM_TerminalSurface::Text_renderer_mode::AUTO);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
        surface,
        true);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_cursor_descender_snapshot(
                cursor_visible ? 911U : 910U,
                cursor_visible)));

    const bool rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    if (!rendered) {
        std::cerr << "SKIP: cursor descender clip did not reach usable QRhi "
            << "render state on " << backend << '\n';
        return k_unsupported_backend_skip_return_code;
    }

    pump_until(
        app,
        window,
        surface,
        [](const term::Qsg_atlas_frame_report& report) {
            return report.render.msdf_text_runs > 0;
        },
        60);
    for (int extra = 0; extra < 6; ++extra) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    out_capture.report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    out_capture.image            = window.grabWindow();
    out_capture.metrics          = metrics;
    out_capture.dpr              = dpr;
    out_capture.software_adapter = window_uses_known_software_adapter(window);
    return 0;
}

// Regression coverage for the MSDF caret-clip displacement: the BLOCK caret
// clips the Q quad at the cell bottom (descender plus SDF pad extend past
// it), and a top-down UV crop on the V-flipped MSDF quad used to lift the
// glyph image by the cropped amount. The caret Q ink must stay on the same
// rows as the unobscured Q ink.
int test_cursor_descender_clip(QGuiApplication& app, const char* backend)
{
#if !VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED
    std::cerr << "SKIP: cursor descender clip requires the MSDF text "
        << "renderer\n";
    (void)app;
    (void)backend;
    return k_unsupported_backend_skip_return_code;
#else
    const int backend_status =
        verify_requested_backend(app, backend, "cursor descender clip");
    if (backend_status != 0) {
        return backend_status;
    }

    Cursor_descender_capture cursor_capture;
    const int cursor_status = render_cursor_descender_capture(
        app, backend, /*cursor_visible=*/true, cursor_capture);
    if (cursor_status != 0) {
        return cursor_status;
    }

    Cursor_descender_capture plain_capture;
    const int plain_status = render_cursor_descender_capture(
        app, backend, /*cursor_visible=*/false, plain_capture);
    if (plain_status != 0) {
        return plain_status;
    }

    const QByteArray image_prefix =
        qgetenv("VNM_TERMINAL_CURSOR_DESCENDER_IMAGE");
    if (!image_prefix.isEmpty()) {
        const QString prefix = QString::fromLocal8Bit(image_prefix);
        cursor_capture.image.save(prefix + QStringLiteral(".cursor.png"));
        plain_capture.image.save(prefix + QStringLiteral(".plain.png"));
    }

    // The renderer never grants MSDF text ownership to known software
    // adapters, so on devices like the GitHub-hosted Windows runner's WARP
    // D3D11 adapter the MSDF-dependent ink measurements below cannot run.
    // Mirror the LCD capability probe and report the backend as unsupported.
    // The skip is strictly adapter-based: on real hardware the msdf_text_runs
    // assertion below stays hard, so a genuine MSDF regression still fails.
    if (cursor_capture.software_adapter || plain_capture.software_adapter) {
        std::cerr << "SKIP: cursor descender clip runs on a software "
            << "adapter, where the MSDF text path stays inactive on "
            << backend << '\n';
        return k_unsupported_backend_skip_return_code;
    }

    bool ok = true;
    ok &= check(!cursor_capture.image.isNull() && !plain_capture.image.isNull(),
        "cursor descender clip captures both window images");
    ok &= check(cursor_capture.report.render.msdf_text_runs > 0 &&
            plain_capture.report.render.msdf_text_runs > 0,
        "cursor descender clip renders text through the MSDF path");
    if (!ok) {
        return 1;
    }

    // Measure inside the caret cell, inset to stay clear of neighbor-cell
    // antialiasing fringes and of edge blending on the caret block borders.
    const QRectF q_cell_band = inset_rect(
        pixel_cell_rect(0, 1, 1, cursor_capture.metrics), 2.0, 1.0);
    const qreal dpr = cursor_capture.dpr;

    // Under the BLOCK caret the Q is drawn background-on-cursor-fill; sample
    // the fill from the band's ink-free top-left corner instead of assuming
    // the exact cursor color.
    const QRect band_pixels = logical_rect_to_pixels(q_cell_band, dpr)
        .intersected(cursor_capture.image.rect());
    const QColor cursor_fill =
        cursor_capture.image.pixelColor(band_pixels.topLeft());
    const QColor background(0, 0, 0);
    ok &= check(pixel_delta(cursor_fill, background) > 64,
        "cursor descender clip sees the block caret fill in the caret cell");

    const Pixel_image_ink_stats cursor_ink = measure_image_ink(
        cursor_capture.image, cursor_fill, q_cell_band, dpr);
    const Pixel_image_ink_stats plain_ink = measure_image_ink(
        plain_capture.image, background, q_cell_band, dpr);
    ok &= check(cursor_ink.has_ink(),
        "cursor descender clip finds dark Q ink inside the block caret");
    ok &= check(plain_ink.has_ink(),
        "cursor descender clip finds light Q ink without the caret");
    if (!ok) {
        return 1;
    }

    std::cerr << "cursor descender clip ink rows:"
        << " caret_top=" << cursor_ink.bbox.top()
        << " plain_top=" << plain_ink.bbox.top()
        << " caret_bottom=" << cursor_ink.bbox.bottom()
        << " plain_bottom=" << plain_ink.bbox.bottom()
        << '\n';
    constexpr int k_ink_row_tolerance_pixels = 1;
    ok &= check(
        std::abs(cursor_ink.bbox.top() - plain_ink.bbox.top()) <=
            k_ink_row_tolerance_pixels,
        "block caret keeps the clipped Q ink top row in place");
    ok &= check(
        std::abs(cursor_ink.bbox.bottom() - plain_ink.bbox.bottom()) <=
            k_ink_row_tolerance_pixels,
        "block caret keeps the clipped Q ink bottom row in place");
    return ok ? 0 : 1;
#endif
}

term::Terminal_render_snapshot make_atlas_report_snapshot(std::uint64_t sequence)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({3, 8}, sequence);
    snapshot.cursor.visible = false;
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    return snapshot;
}

term::Terminal_render_snapshot make_atlas_row_stable_text_snapshot(
    std::uint64_t sequence,
    QString       middle_text,
    int           middle_display_width,
    bool          add_wide_continuations,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({3, 8}, sequence);
    snapshot.cursor.visible    = false;
    snapshot.dirty_row_ranges  = std::move(dirty_row_ranges);
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(
            1,
            0,
            middle_text,
            middle_display_width,
            term::k_default_terminal_style_id));
    if (add_wide_continuations) {
        const int continuation_columns =
            std::min(middle_display_width, snapshot.grid_size.columns);
        for (int column = 1; column < continuation_columns; ++column) {
            snapshot.cells.push_back(
                make_pixel_continuation_cell(
                    1,
                    column,
                    term::k_default_terminal_style_id));
        }
    }
    snapshot.cells.push_back(
        make_pixel_cell(2, 0, QStringLiteral("C"), 1, term::k_default_terminal_style_id));
    return snapshot;
}

term::Terminal_render_snapshot make_atlas_row_stable_text_snapshot(
    std::uint64_t sequence,
    QString       middle_text,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    const int middle_display_width = static_cast<int>(middle_text.size());
    return make_atlas_row_stable_text_snapshot(
        sequence,
        std::move(middle_text),
        middle_display_width,
        false,
        std::move(dirty_row_ranges));
}

term::Terminal_render_snapshot make_atlas_row_stable_graphic_snapshot(
    std::uint64_t sequence,
    term::Terminal_style_id middle_style,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({3, 8}, sequence);
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    snapshot.styles.push_back(rgb_style(0xff55e6c1U, 0xff101820U));
    snapshot.styles.push_back(rgb_style(0xffffd166U, 0xff101820U));

    const auto append_block_row =
        [&](int row, term::Terminal_style_id style_id) {
            for (int column = 0; column < snapshot.grid_size.columns; ++column) {
                snapshot.cells.push_back(
                    make_pixel_cell(
                        row,
                        column,
                        QStringLiteral("\u2588"),
                        1,
                        style_id));
            }
        };
    append_block_row(0, 1U);
    append_block_row(1, middle_style);
    append_block_row(2, 1U);
    return snapshot;
}

term::Terminal_render_snapshot make_atlas_row_stable_graphic_arc_snapshot(
    std::uint64_t sequence,
    term::Terminal_style_id middle_style,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({3, 8}, sequence);
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    snapshot.styles.push_back(rgb_style(0xff55e6c1U, 0xff101820U));
    snapshot.styles.push_back(rgb_style(0xffffd166U, 0xff101820U));

    const auto append_arc_row =
        [&](int row, term::Terminal_style_id style_id) {
            snapshot.cells.push_back(
                make_pixel_cell(row, 0, QStringLiteral("\u256d"), 1, style_id));
            snapshot.cells.push_back(
                make_pixel_cell(row, 1, QStringLiteral("\u256e"), 1, style_id));
            snapshot.cells.push_back(
                make_pixel_cell(row, 2, QStringLiteral("\u256f"), 1, style_id));
            snapshot.cells.push_back(
                make_pixel_cell(row, 3, QStringLiteral("\u2570"), 1, style_id));
        };
    append_arc_row(0, 1U);
    append_arc_row(1, middle_style);
    append_arc_row(2, 1U);
    return snapshot;
}

term::Terminal_render_snapshot make_atlas_row_stable_background_snapshot(
    std::uint64_t sequence,
    term::Terminal_style_id middle_style,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({3, 8}, sequence);
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff303030U));
    snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff505050U));

    const auto append_blank_row =
        [&](int row, term::Terminal_style_id style_id) {
            for (int column = 0; column < snapshot.grid_size.columns; ++column) {
                snapshot.cells.push_back(
                    make_pixel_cell(row, column, QString(), 1, style_id));
            }
        };
    append_blank_row(0, 1U);
    append_blank_row(1, middle_style);
    append_blank_row(2, 1U);
    return snapshot;
}

bool pump_atlas_seeded_glyph_slots(
    QGuiApplication& app,
    QQuickWindow&    window,
    VNM_TerminalSurface& surface,
    term::Qsg_atlas_frame_report& report,
    int              attempts = 12)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const term::Qsg_atlas_buffer_update_summary& glyph_buffer =
            report.render.glyph_buffer;
        if (glyph_buffer.seeded_slots >= glyph_buffer.rhi_frames_in_flight) {
            return true;
        }

        const std::uint64_t previous_prepare_count = report.prepare_count;
        if (!pump_next_atlas_report(
                app,
                window,
                surface,
                previous_prepare_count,
                report))
        {
            return false;
        }
    }

    return false;
}

bool pump_atlas_seeded_rect_slots(
    QGuiApplication& app,
    QQuickWindow&    window,
    VNM_TerminalSurface& surface,
    term::Qsg_atlas_frame_report& report,
    int              attempts = 12)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const term::Qsg_atlas_buffer_update_summary& rect_buffer =
            report.render.rect_buffer;
        if (rect_buffer.seeded_slots >= rect_buffer.rhi_frames_in_flight) {
            return true;
        }

        const std::uint64_t previous_prepare_count = report.prepare_count;
        if (!pump_next_atlas_report(
                app,
                window,
                surface,
                previous_prepare_count,
                report))
        {
            return false;
        }
    }

    return false;
}

// Baked-atlas identity: the fields that must stay stable while the baked atlas
// is reused across a same-bucket zoom step. atlas_generation only advances on a
// real baked rebuild, so it discriminates a baked miss from a draw-only change.
struct Msdf_text_baked_identity
{
    int           font_data_bytes    = 0;
    int           atlas_size         = 0;
    int           baked_pixel_height = 0;
    std::uint64_t atlas_generation   = 0U;
    QString       message;
};

// Draw-layout identity: the fields that legitimately change with the font size
// even when the baked atlas is reused.
struct Msdf_text_draw_identity
{
    int   pixel_height = 0;
    float px_range     = 0.0f;
};

QString atlas_msdf_resource_stability_row_0()
{
    return QStringLiteral(
        "WMeaom#@|1.\u00e9\u03a9\u0411\u20ac\u2211");
}

QString atlas_msdf_resource_stability_row_1()
{
    return QStringLiteral("[]{}<>/?+-=_\u2500\u2588\ue0b0");
}

int atlas_msdf_resource_stability_drawable_glyphs(QStringView text)
{
    int count = 0;
    for (QChar character : text) {
        if (!character.isSpace()) {
            ++count;
        }
    }
    return count;
}

int atlas_msdf_resource_stability_expected_glyphs()
{
    return
        atlas_msdf_resource_stability_drawable_glyphs(
            atlas_msdf_resource_stability_row_0()) +
        atlas_msdf_resource_stability_drawable_glyphs(
            atlas_msdf_resource_stability_row_1());
}

term::Terminal_render_snapshot make_atlas_msdf_resource_stability_snapshot(
    std::uint64_t sequence,
    bool          color_changed_row)
{
    const QString row_0 = atlas_msdf_resource_stability_row_0();
    const QString row_1 = atlas_msdf_resource_stability_row_1();

    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({2, 20}, sequence);
    snapshot.cursor.visible = false;
    snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    snapshot.styles.push_back(rgb_style(0xff8be9fdU, 0xff101820U));

    const term::Terminal_style_id normal = 1U;
    const term::Terminal_style_id accent = 2U;
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, row_0, row_0.size(), normal));
    snapshot.cells.push_back(
        make_pixel_cell(
            1,
            0,
            row_1,
            row_1.size(),
            color_changed_row ? accent : normal));
    snapshot.dirty_row_ranges = color_changed_row
        ? std::vector<term::Terminal_render_dirty_row_range>{{1, 1}}
        : std::vector<term::Terminal_render_dirty_row_range>{{0, 2}};
    return snapshot;
}

Msdf_text_baked_identity msdf_text_baked_identity(
    const term::Qsg_atlas_render_summary& render_summary)
{
    return {
        render_summary.msdf_text_font_data_bytes,
        render_summary.msdf_text_atlas_size,
        render_summary.msdf_text_baked_pixel_height,
        render_summary.msdf_text_atlas_generation,
        render_summary.msdf_text_message,
    };
}

bool msdf_text_baked_identity_matches(
    const Msdf_text_baked_identity&        expected,
    const term::Qsg_atlas_render_summary&  render_summary)
{
    return
        render_summary.msdf_text_font_data_bytes  == expected.font_data_bytes &&
        render_summary.msdf_text_atlas_size       == expected.atlas_size      &&
        render_summary.msdf_text_baked_pixel_height ==
            expected.baked_pixel_height &&
        render_summary.msdf_text_atlas_generation == expected.atlas_generation &&
        render_summary.msdf_text_message          == expected.message;
}

Msdf_text_draw_identity msdf_text_draw_identity(
    const term::Qsg_atlas_render_summary& render_summary)
{
    return {
        render_summary.msdf_text_pixel_height,
        render_summary.msdf_text_px_range,
    };
}

bool msdf_text_draw_identity_matches(
    const Msdf_text_draw_identity&         expected,
    const term::Qsg_atlas_render_summary&  render_summary)
{
    return
        render_summary.msdf_text_pixel_height == expected.pixel_height &&
        std::abs(render_summary.msdf_text_px_range - expected.px_range) <=
            0.0001f;
}

void print_atlas_msdf_resource_stability_report(
    const char*                         label,
    const term::Qsg_atlas_frame_report& report)
{
    const term::Qsg_atlas_render_summary& render_summary = report.render;
    const term::Qsg_atlas_buffer_update_summary& buffer =
        render_summary.msdf_text_buffer;
    const QByteArray message = render_summary.msdf_text_message.toUtf8();
    std::cout << "atlas MSDF resource stability " << label
        << " prepare=" << report.prepare_count
        << " render=" << report.render_count
        << " requested="
        << render_summary.msdf_text_renderer_enabled
        << " active=" << render_summary.msdf_text_renderer_active
        << " resources_ready=" << render_summary.msdf_text_resources_ready
        << " atlas_built=" << render_summary.msdf_text_atlas_built
        << " atlas_ready=" << render_summary.msdf_text_atlas_ready
        << " texture_ready=" << render_summary.msdf_text_texture_ready
        << " texture_uploaded="
        << render_summary.msdf_text_texture_uploaded
        << " font_bytes=" << render_summary.msdf_text_font_data_bytes
        << " pixel_height=" << render_summary.msdf_text_pixel_height
        << " atlas_size=" << render_summary.msdf_text_atlas_size
        << " px_range=" << render_summary.msdf_text_px_range
        << " message=" << message.constData()
        << " msdf_runs=" << render_summary.msdf_text_runs
        << " msdf_supported_runs="
        << render_summary.msdf_text_supported_runs
        << " msdf_instances="
        << render_summary.msdf_text_glyph_instances
        << " msdf_draws=" << render_summary.msdf_text_draw_calls
        << " missed_runs="
        << render_summary.msdf_text_missed_supported_runs
        << " missed_glyphs="
        << render_summary.msdf_text_missed_supported_glyphs
        << " glyph_draws=" << render_summary.glyph_draw_calls
        << " glyph_instances=" << render_summary.glyph_buffer_instances
        << " buffer_full=" << buffer.full_upload
        << " buffer_partial=" << buffer.partial_upload
        << " buffer_skipped=" << buffer.skipped_upload
        << " buffer_seed=" << buffer.rotating_slot_seed_upload
        << " buffer_recreated=" << buffer.buffer_recreated_upload
        << " buffer_layout_changed="
        << buffer.instance_layout_changed_upload
        << " buffer_full_repaint=" << buffer.full_repaint_upload
        << " buffer_non_dirty=" << buffer.non_dirty_state_upload
        << " buffer_uploaded=" << buffer.uploaded_bytes
        << " buffer_bytes=" << buffer.buffer_bytes
        << " seeded_slots=" << buffer.seeded_slots
        << '/' << buffer.rhi_frames_in_flight
        << '\n';
}

bool atlas_msdf_resource_stability_render_ok(
    const char*                         label,
    const term::Qsg_atlas_frame_report& report,
    int                                 expected_glyphs)
{
    const term::Qsg_atlas_render_summary& render_summary = report.render;
    bool ok = true;
    ok &= check(
        atlas_report_render_state_ready(report),
        std::string("atlas MSDF resource stability ") + label +
            " reaches usable QRhi render state");
    ok &= check(
        render_summary.msdf_text_renderer_active,
        std::string("atlas MSDF resource stability ") + label +
            " keeps the MSDF renderer active");
    ok &= check(
        render_summary.msdf_text_shader_package_available &&
            render_summary.msdf_text_atlas_built &&
            render_summary.msdf_text_atlas_ready &&
            render_summary.msdf_text_texture_ready &&
            render_summary.msdf_text_resources_ready,
        std::string("atlas MSDF resource stability ") + label +
            " reports ready MSDF resources");
    ok &= check(
        render_summary.msdf_text_missed_supported_runs == 0 &&
            render_summary.msdf_text_missed_supported_glyphs == 0,
        std::string("atlas MSDF resource stability ") + label +
            " reports no supported MSDF misses");
    ok &= check(
        render_summary.glyph_draw_calls == 0 &&
            render_summary.glyph_buffer_instances == 0,
        std::string("atlas MSDF resource stability ") + label +
            " does not use the old glyph path");
    ok &= check(
        render_summary.msdf_text_glyph_instances == expected_glyphs,
        std::string("atlas MSDF resource stability ") + label +
            " reports the expected supported glyph instance count");
    ok &= check(
        render_summary.msdf_text_draw_calls > 0 &&
            render_summary.msdf_text_draw_calls <= 2,
        std::string("atlas MSDF resource stability ") + label +
            " keeps MSDF draw calls bounded");
    ok &= check(
        render_summary.msdf_text_supported_runs > 0 &&
            render_summary.msdf_text_runs >=
                render_summary.msdf_text_supported_runs,
        std::string("atlas MSDF resource stability ") + label +
            " reports supported MSDF runs");
    return ok;
}

bool atlas_msdf_text_path_unsupported_by_adapter(
    const term::Qsg_atlas_frame_report& report,
    bool                                software_adapter)
{
    const term::Qsg_atlas_render_summary& render = report.render;
    const bool glyph_fallback_evidence =
        render.text_renderer_fallback_allowed &&
        render.text_renderer_fallback_used    &&
        (render.effective_text_renderer ==
                term::Terminal_text_renderer_kind::GLYPH ||
            render.effective_text_renderer ==
                term::Terminal_text_renderer_kind::MIXED) &&
        render.glyph_draw_calls > 0;

    return
        software_adapter                         &&
        atlas_report_render_state_ready(report)  &&
        render.msdf_text_renderer_enabled        &&
        !render.msdf_text_renderer_active        &&
        glyph_fallback_evidence;
}

bool atlas_msdf_text_path_skipped(
    const char*                         label,
    QQuickWindow&                       window,
    const term::Qsg_atlas_frame_report& report)
{
    if (!atlas_msdf_text_path_unsupported_by_adapter(
            report,
            window_uses_known_software_adapter(window)))
    {
        return false;
    }

    std::cerr << "SKIP: " << label << " cannot activate MSDF text on this "
        << "backend; it falls back to the glyph renderer\n";
    return true;
}

bool test_atlas_msdf_text_path_adapter_skip_predicate()
{
    term::Qsg_atlas_frame_report report;
    report.render.msdf_text_renderer_enabled = false;

    bool ok = true;
    ok &= check(
        !atlas_msdf_text_path_unsupported_by_adapter(report, true),
        "MSDF adapter skip predicate rejects disabled MSDF renderer");

    report.render.msdf_text_renderer_enabled = true;
    ok &= check(
        !atlas_msdf_text_path_unsupported_by_adapter(report, false),
        "MSDF adapter skip predicate keeps inactive real-hardware path strict");
    ok &= check(
        !atlas_msdf_text_path_unsupported_by_adapter(report, true),
        "MSDF adapter skip predicate rejects stale software-adapter report");

    report.prepare_count = 1U;
    report.render_count = 1U;
    report.drew = true;
    report.command_buffer_non_null = true;
    report.render_target_non_null = true;
    report.rhi_non_null = true;
    ok &= check(
        !atlas_msdf_text_path_unsupported_by_adapter(report, true),
        "MSDF adapter skip predicate requires explicit glyph fallback evidence");

    report.render.effective_text_renderer =
        term::Terminal_text_renderer_kind::GLYPH;
    report.render.text_renderer_fallback_used = true;
    report.render.glyph_draw_calls = 1;
    ok &= check(
        atlas_msdf_text_path_unsupported_by_adapter(report, true),
        "MSDF adapter skip predicate accepts rendered glyph-fallback software path");

    report.render.msdf_text_renderer_active = true;
    ok &= check(
        !atlas_msdf_text_path_unsupported_by_adapter(report, true),
        "MSDF adapter skip predicate keeps active renderer strict");
    return ok;
}

enum class Atlas_msdf_steady_buffer_expectation
{
    SKIPPED,
    PARTIAL,
};

bool atlas_msdf_resource_stability_steady_buffer_ok(
    const char*                         label,
    const term::Qsg_atlas_frame_report& report,
    Atlas_msdf_steady_buffer_expectation expectation)
{
    const term::Qsg_atlas_buffer_update_summary& buffer =
        report.render.msdf_text_buffer;
    bool ok = true;
    ok &= check(
        buffer.seeded_slots >= buffer.rhi_frames_in_flight,
        std::string("atlas MSDF resource stability ") + label +
            " runs after all MSDF rotating slots are seeded");
    ok &= check(
        !buffer.full_upload &&
            !buffer.rotating_slot_seed_upload &&
            !buffer.buffer_recreated_upload &&
            !buffer.instance_layout_changed_upload &&
            !buffer.full_repaint_upload &&
            !buffer.non_dirty_state_upload,
        std::string("atlas MSDF resource stability ") + label +
            " avoids full or fallback MSDF instance-buffer uploads");
    if (expectation == Atlas_msdf_steady_buffer_expectation::SKIPPED) {
        ok &= check(
            buffer.skipped_upload &&
                !buffer.partial_upload &&
                !buffer.full_upload &&
                buffer.uploaded_bytes == 0,
            std::string("atlas MSDF resource stability ") + label +
                " skips the unchanged MSDF instance buffer");
    }
    else {
        ok &= check(
            buffer.partial_upload &&
                !buffer.skipped_upload &&
                !buffer.full_upload &&
                buffer.uploaded_bytes > 0 &&
                buffer.uploaded_bytes < buffer.buffer_bytes,
            std::string("atlas MSDF resource stability ") + label +
                " partially patches the dirty MSDF instance buffer");
    }
    return ok;
}

bool pump_atlas_seeded_msdf_text_slots(
    QGuiApplication&    app,
    QQuickWindow&       window,
    VNM_TerminalSurface& surface,
    term::Qsg_atlas_frame_report& report,
    std::vector<term::Qsg_atlas_frame_report>& reports,
    int                 attempts = 12)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const term::Qsg_atlas_buffer_update_summary& buffer =
            report.render.msdf_text_buffer;
        if (buffer.seeded_slots >= buffer.rhi_frames_in_flight) {
            return true;
        }

        const std::uint64_t previous_prepare_count = report.prepare_count;
        if (!pump_next_atlas_report(
                app,
                window,
                surface,
                previous_prepare_count,
                report))
        {
            return false;
        }
        reports.push_back(report);
    }

    return false;
}

bool test_atlas_msdf_resource_stability(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(420, 140);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 90.0));
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(970U, false)));
    window.show();

    const int expected_glyphs =
        atlas_msdf_resource_stability_expected_glyphs();
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& report) {
            if (!atlas_report_render_state_ready(report)) {
                return false;
            }
            if (!report.render.msdf_text_renderer_enabled) {
                return true;
            }
            return
                report.render.msdf_text_renderer_active &&
                report.render.msdf_text_resources_ready &&
                report.render.msdf_text_texture_ready &&
                report.render.msdf_text_glyph_instances == expected_glyphs;
        });
    baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    print_atlas_msdf_resource_stability_report(
        "baseline",
        baseline_report);
    if (!baseline_rendered) {
        if (atlas_msdf_text_path_skipped(
                "atlas MSDF resource stability",
                window,
                baseline_report))
        {
            return true;
        }
        std::cerr << "FAIL: atlas MSDF resource stability did not reach "
            << "a usable baseline render state\n";
        return false;
    }

    if (!baseline_report.render.msdf_text_renderer_enabled) {
        return check(
            !baseline_report.render.msdf_text_renderer_active,
            "atlas MSDF resource stability skips renderer gates when disabled");
    }

    std::vector<term::Qsg_atlas_frame_report> reports;
    reports.push_back(baseline_report);
    term::Qsg_atlas_frame_report seeded_report = baseline_report;
    const bool seeded = pump_atlas_seeded_msdf_text_slots(
        app,
        window,
        surface,
        seeded_report,
        reports);
    print_atlas_msdf_resource_stability_report("seeded", seeded_report);

    term::Terminal_render_snapshot unchanged_snapshot =
        make_atlas_msdf_resource_stability_snapshot(971U, false);
    unchanged_snapshot.dirty_row_ranges.clear();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            unchanged_snapshot));
    term::Qsg_atlas_frame_report unchanged_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool unchanged_prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        seeded_report.prepare_count,
        unchanged_report);
    reports.push_back(unchanged_report);
    print_atlas_msdf_resource_stability_report(
        "unchanged",
        unchanged_report);

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(972U, true)));
    term::Qsg_atlas_frame_report color_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool color_prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        unchanged_report.prepare_count,
        color_report);
    reports.push_back(color_report);
    print_atlas_msdf_resource_stability_report(
        "color-only",
        color_report);

    const Msdf_text_baked_identity baked_identity =
        msdf_text_baked_identity(baseline_report.render);
    const Msdf_text_draw_identity draw_identity =
        msdf_text_draw_identity(baseline_report.render);
    int texture_uploads = 0;
    for (const term::Qsg_atlas_frame_report& report : reports) {
        if (report.render.msdf_text_texture_uploaded) {
            ++texture_uploads;
        }
    }

    bool ok = true;
    ok &= check(
        seeded,
        "atlas MSDF resource stability seeds all MSDF rotating slots");
    ok &= check(
        unchanged_prepared,
        "atlas MSDF resource stability renders an unchanged steady frame");
    ok &= check(
        color_prepared,
        "atlas MSDF resource stability renders a color-only dirty frame");
    for (const term::Qsg_atlas_frame_report& report : reports) {
        ok &= atlas_msdf_resource_stability_render_ok(
            "frame",
            report,
            expected_glyphs);
        ok &= check(
            msdf_text_baked_identity_matches(baked_identity, report.render),
            "atlas MSDF resource stability keeps baked atlas identity stable");
        ok &= check(
            msdf_text_draw_identity_matches(draw_identity, report.render),
            "atlas MSDF resource stability keeps draw layout identity stable at one size");
    }
    ok &= check(
        baseline_report.render.msdf_text_texture_uploaded,
        "atlas MSDF resource stability uploads the MSDF texture on initial generation");
    ok &= check(
        texture_uploads == 1,
        "atlas MSDF resource stability does not reupload the MSDF texture after seeding");
    ok &= atlas_msdf_resource_stability_steady_buffer_ok(
        "unchanged",
        unchanged_report,
        Atlas_msdf_steady_buffer_expectation::SKIPPED);
    ok &= atlas_msdf_resource_stability_steady_buffer_ok(
        "color-only",
        color_report,
        Atlas_msdf_steady_buffer_expectation::PARTIAL);

    // Batch 1 instrumentation assertions. The baseline frame builds and uploads
    // exactly one MSDF atlas; the steady same-size frames reuse it with no extra
    // build or upload. Cross-size reuse is not enabled until Batch 3.
    ok &= check(
        baseline_report.render.msdf_text_cache_miss &&
            baseline_report.render.msdf_text_atlas_build_attempted &&
            baseline_report.render.msdf_text_atlas_build_succeeded,
        "atlas MSDF resource stability records the initial baked-atlas build");
    ok &= check(
        baseline_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            baseline_report.render.msdf_text_atlas_texture_uploads_total == 1U,
        "atlas MSDF resource stability builds and uploads the atlas exactly once at baseline");
    // Batch 2: the library bakes at max(draw_pixel_height,
    // ceil(min_atlas_font_size)). The renderer's min_atlas_font_size is 48 and the
    // 18px test font draws below it, so the baked height is the 48px floor and is
    // deliberately distinct from the draw height.
    constexpr int k_msdf_min_bake_pixel_height = 48;
    ok &= check(
        baseline_report.render.msdf_text_baked_pixel_height ==
            std::max(
                baseline_report.render.msdf_text_pixel_height,
                k_msdf_min_bake_pixel_height),
        "atlas MSDF resource stability reports the baked height as the bake-bucket floor");
    ok &= check(
        color_report.render.msdf_text_cache_hit &&
            color_report.render.msdf_text_baked_atlas_reused &&
            !color_report.render.msdf_text_cache_miss,
        "atlas MSDF resource stability reuses the baked atlas on steady same-size frames");
    ok &= check(
        color_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            color_report.render.msdf_text_atlas_texture_uploads_total == 1U,
        "atlas MSDF resource stability does not rebuild or re-upload for steady same-size frames");

    // Batch 3 cache-identity check: a font-size change that stays in the same
    // bake bucket (the 18px and 30px draw heights both fall at or below the 48px
    // floor) reuses the baked atlas. The draw layout changes, but there is no
    // rebuild and no re-upload, so the cumulative build/upload counts stay at one
    // and the baked generation is stable. This also exercises the resource-
    // lifetime split: the MSDF pipelines, shader-resource bindings, and atlas
    // texture all survive the font-size change.
    const Msdf_text_draw_identity baseline_draw_identity =
        msdf_text_draw_identity(baseline_report.render);
    surface.set_font_size(30.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(973U, false)));
    const bool resized_prepared = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& report) {
            return atlas_report_render_state_ready(report) &&
                report.render.msdf_text_renderer_active &&
                report.render.msdf_text_resources_ready &&
                report.render.msdf_text_texture_ready &&
                report.render.msdf_text_pixel_height !=
                    baseline_draw_identity.pixel_height;
        });
    const term::Qsg_atlas_frame_report resized_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    print_atlas_msdf_resource_stability_report("resized", resized_report);
    ok &= check(
        resized_prepared,
        "atlas MSDF resource stability re-renders after a font-size change");
    ok &= check(
        resized_report.render.msdf_text_cache_hit &&
            resized_report.render.msdf_text_baked_atlas_reused &&
            !resized_report.render.msdf_text_cache_miss,
        "atlas MSDF resource stability reuses the baked atlas across a same-bucket font-size change");
    ok &= check(
        resized_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            resized_report.render.msdf_text_atlas_texture_uploads_total == 1U &&
            !resized_report.render.msdf_text_texture_uploaded,
        "atlas MSDF resource stability does not rebuild or re-upload for a same-bucket font-size change");
    ok &= check(
        msdf_text_baked_identity_matches(baked_identity, resized_report.render),
        "atlas MSDF resource stability keeps baked identity across a same-bucket font-size change");
    ok &= check(
        !msdf_text_draw_identity_matches(
            baseline_draw_identity, resized_report.render),
        "atlas MSDF resource stability changes the draw layout across a font-size change");

    return ok;
}

// Shared readiness predicate for the zoom tests: render state usable and, when
// MSDF is enabled on this backend, the MSDF resources and texture are ready.
bool atlas_msdf_zoom_report_ready(const term::Qsg_atlas_frame_report& report)
{
    if (!atlas_report_render_state_ready(report)) {
        return false;
    }
    if (!report.render.msdf_text_renderer_enabled) {
        return true;
    }
    return
        report.render.msdf_text_renderer_active &&
        report.render.msdf_text_resources_ready &&
        report.render.msdf_text_texture_ready;
}

// Two font sizes that both fall at or below min_atlas_font_size share one bake
// bucket. The baked atlas (bitmap, UVs, generation) must be reused across the
// size change while the draw layout (draw pixel height, shader px_range) changes.
bool test_atlas_msdf_zoom_reuses_baked_atlas(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(480, 200);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(420.0, 150.0));
    surface.set_font_family(QString());
    surface.set_font_size(12.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(980U, false)));
    window.show();

    const bool ready_small =
        pump_until(app, window, surface, atlas_msdf_zoom_report_ready);
    const term::Qsg_atlas_frame_report small_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!ready_small) {
        if (atlas_msdf_text_path_skipped(
                "msdf zoom reuse",
                window,
                small_report))
        {
            return true;
        }
        std::cerr << "FAIL: msdf zoom reuse did not reach a usable render "
            << "state at the first size\n";
        return false;
    }
    if (!small_report.render.msdf_text_renderer_enabled) {
        return check(
            !small_report.render.msdf_text_renderer_active,
            "msdf zoom reuse skips renderer gates when MSDF is disabled");
    }

    bool ok = true;
    ok &= check(
        small_report.render.msdf_text_cache_miss &&
            small_report.render.msdf_text_atlas_build_succeeded &&
            small_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            small_report.render.msdf_text_atlas_texture_uploads_total == 1U,
        "msdf zoom reuse builds and uploads exactly one atlas at the first size");

    const int small_pixel_height = small_report.render.msdf_text_pixel_height;
    surface.set_font_size(24.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(981U, false)));
    const bool ready_large = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& report) {
            return atlas_msdf_zoom_report_ready(report) &&
                report.render.msdf_text_pixel_height != small_pixel_height;
        });
    const term::Qsg_atlas_frame_report large_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    ok &= check(
        ready_large,
        "msdf zoom reuse renders a second font size in the same bake bucket");

    // Precondition: the two sizes share one bake bucket but differ in draw height.
    ok &= check(
        small_report.render.msdf_text_baked_pixel_height ==
            large_report.render.msdf_text_baked_pixel_height,
        "msdf zoom reuse keeps both sizes in one bake bucket");
    ok &= check(
        small_report.render.msdf_text_pixel_height !=
            large_report.render.msdf_text_pixel_height,
        "msdf zoom reuse actually changes the draw pixel height");

    // The second size reuses the baked atlas: no rebuild, no re-upload, stable
    // generation, but a changed draw-size shader range. Unlike the cross-bucket
    // case, a same-bucket transition never produces a miss-then-hit sequence, so
    // every ready frame after the draw-height change is a cache hit and the
    // per-frame hit flags below are stable regardless of which frame is captured.
    ok &= check(
        large_report.render.msdf_text_cache_hit &&
            large_report.render.msdf_text_baked_atlas_reused &&
            !large_report.render.msdf_text_cache_miss,
        "msdf zoom reuse reports a baked-atlas hit at the second size");
    ok &= check(
        large_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            large_report.render.msdf_text_atlas_texture_uploads_total == 1U &&
            !large_report.render.msdf_text_texture_uploaded,
        "msdf zoom reuse does not rebuild or re-upload the atlas for the second size");
    ok &= check(
        small_report.render.msdf_text_atlas_generation ==
            large_report.render.msdf_text_atlas_generation,
        "msdf zoom reuse keeps a stable baked atlas generation");
    ok &= check(
        std::abs(small_report.render.msdf_text_px_range -
                 large_report.render.msdf_text_px_range) > 0.0001f,
        "msdf zoom reuse changes the draw-size shader range with font size");
    return ok;
}

// A font size whose draw height exceeds min_atlas_font_size bakes at the draw
// size, a different bake bucket. Crossing the bucket must rebuild and re-upload
// the atlas exactly once and advance the baked generation by one.
bool test_atlas_msdf_zoom_crosses_bake_bucket(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(720, 540);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(640.0, 460.0));
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(990U, false)));
    window.show();

    const bool ready_small =
        pump_until(app, window, surface, atlas_msdf_zoom_report_ready);
    const term::Qsg_atlas_frame_report small_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!ready_small) {
        if (atlas_msdf_text_path_skipped(
                "msdf zoom bucket-cross",
                window,
                small_report))
        {
            return true;
        }
        std::cerr << "FAIL: msdf zoom bucket-cross did not reach a usable "
            << "render state at the small size\n";
        return false;
    }
    if (!small_report.render.msdf_text_renderer_enabled) {
        return check(
            !small_report.render.msdf_text_renderer_active,
            "msdf zoom bucket-cross skips renderer gates when MSDF is disabled");
    }

    bool ok = true;
    ok &= check(
        small_report.render.msdf_text_atlas_build_attempts_total == 1U &&
            small_report.render.msdf_text_atlas_texture_uploads_total == 1U,
        "msdf zoom bucket-cross builds and uploads exactly one atlas at the small size");

    const int small_baked = small_report.render.msdf_text_baked_pixel_height;
    surface.set_font_size(96.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(991U, false)));
    const bool ready_large = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& report) {
            return atlas_msdf_zoom_report_ready(report) &&
                report.render.msdf_text_baked_pixel_height != small_baked;
        });
    const term::Qsg_atlas_frame_report large_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    ok &= check(
        ready_large,
        "msdf zoom bucket-cross renders a font size in a new bake bucket");

    // Precondition: the large size genuinely crosses into a different bucket.
    ok &= check(
        small_report.render.msdf_text_baked_pixel_height !=
            large_report.render.msdf_text_baked_pixel_height,
        "msdf zoom bucket-cross moves to a different baked pixel height");

    // The per-frame cache_miss flag is transient (true only on the single rebuild
    // frame), and a warm event pump can land the captured report on a later steady
    // hit frame. Assert the rebuild through the monotonic cumulative counters and
    // the baked generation instead, which are stable on whatever frame is caught.
    ok &= check(
        large_report.render.msdf_text_atlas_build_attempts_total == 2U &&
            large_report.render.msdf_text_atlas_build_successes_total == 2U &&
            large_report.render.msdf_text_atlas_texture_uploads_total == 2U,
        "msdf zoom bucket-cross rebuilds and re-uploads exactly once for the new bucket");
    ok &= check(
        large_report.render.msdf_text_baked_cache_misses_total ==
            small_report.render.msdf_text_baked_cache_misses_total + 1U,
        "msdf zoom bucket-cross records exactly one additional baked-cache miss");
    ok &= check(
        large_report.render.msdf_text_atlas_generation ==
            small_report.render.msdf_text_atlas_generation + 1U,
        "msdf zoom bucket-cross advances the baked atlas generation by one");
    return ok;
}

bool run_atlas_glyph_row_stable_report_case(
    QGuiApplication& app,
    const char*      name,
    const term::Terminal_render_snapshot& baseline_snapshot,
    const term::Terminal_render_snapshot& mutated_snapshot,
    const std::function<bool(const term::Qsg_atlas_render_summary&)>&
                     extra_expected,
    bool             expect_full_fallback = false)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(baseline_snapshot));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered ||
        !pump_atlas_seeded_glyph_slots(app, window, surface, baseline_report))
    {
        std::cerr << "FAIL: atlas glyph row-stable " << name
            << " could not seed all glyph buffer slots"
            << " prepare_count=" << baseline_report.prepare_count
            << " seeded_slots=" << baseline_report.render.glyph_buffer.seeded_slots
            << " frames_in_flight="
            << baseline_report.render.glyph_buffer.rhi_frames_in_flight
            << '\n';
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(mutated_snapshot));

    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        report);
    const term::Qsg_atlas_render_summary& render_summary = report.render;
    const term::Qsg_atlas_buffer_update_summary& glyph_buffer =
        render_summary.glyph_buffer;
    const bool partial_update =
        glyph_buffer.row_stable_layout       &&
        glyph_buffer.partial_upload          &&
        !glyph_buffer.full_upload            &&
        !glyph_buffer.non_dirty_state_upload &&
        glyph_buffer.uploaded_bytes > 0      &&
        glyph_buffer.uploaded_bytes < glyph_buffer.buffer_bytes;
    const bool full_fallback =
        glyph_buffer.row_stable_layout       &&
        glyph_buffer.full_upload             &&
        !glyph_buffer.partial_upload         &&
        glyph_buffer.non_dirty_state_upload  &&
        glyph_buffer.uploaded_bytes == glyph_buffer.buffer_bytes;
    const bool expected_upload =
        expect_full_fallback ? full_fallback : partial_update;
    const bool extra = prepared && extra_expected(render_summary);

    if (!prepared || !expected_upload || !extra) {
        std::cerr << "atlas glyph row-stable " << name
            << " prepared=" << prepared
            << " rect_full=" << render_summary.rect_buffer.full_upload
            << " rect_partial=" << render_summary.rect_buffer.partial_upload
            << " rect_non_dirty=" << render_summary.rect_buffer.non_dirty_state_upload
            << " rect_uploaded=" << render_summary.rect_buffer.uploaded_bytes
            << " rect_buffer_bytes=" << render_summary.rect_buffer.buffer_bytes
            << " glyph_full=" << glyph_buffer.full_upload
            << " glyph_partial=" << glyph_buffer.partial_upload
            << " glyph_non_dirty=" << glyph_buffer.non_dirty_state_upload
            << " glyph_uploaded=" << glyph_buffer.uploaded_bytes
            << " glyph_buffer_bytes=" << glyph_buffer.buffer_bytes
            << " row_stable=" << glyph_buffer.row_stable_layout
            << " glyph_active=" << glyph_buffer.active_instance_count
            << " glyph_instances=" << glyph_buffer.instance_count
            << " glyph_draw_calls=" << render_summary.glyph_draw_calls
            << " msdf_draw_calls=" << render_summary.msdf_text_draw_calls
            << " glyph_text_capacity=" << render_summary.glyph_text_row_capacity
            << " glyph_cursor_text_capacity="
            << render_summary.glyph_cursor_text_row_capacity
            << " shaped_text_runs=" << render_summary.shaped_text_runs
            << " non_dirty_cursor="
            << render_summary.non_dirty_cursor_invalidation
            << '\n';
    }

    bool ok = true;
    ok &= check(prepared,
        std::string("atlas glyph row-stable ") + name +
            " reaches a prepared frame");
    ok &= check(expected_upload,
        std::string("atlas glyph row-stable ") + name +
            (expect_full_fallback
                ? " falls back to a full glyph upload"
                : " patches only dirty row glyph bytes"));
    ok &= check(extra,
        std::string("atlas glyph row-stable ") + name +
            " reports expected case counters");
    return ok;
}

bool test_atlas_glyph_row_stable_dirty_update(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_text_snapshot(
                930U,
                QStringLiteral("B"),
                {{0, 3}})));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered ||
        !pump_atlas_seeded_glyph_slots(app, window, surface, baseline_report))
    {
        std::cerr << "FAIL: atlas glyph row-stable dirty update could not seed "
            << "all glyph buffer slots"
            << " prepare_count=" << baseline_report.prepare_count
            << " seeded_slots=" << baseline_report.render.glyph_buffer.seeded_slots
            << " frames_in_flight="
            << baseline_report.render.glyph_buffer.rhi_frames_in_flight
            << '\n';
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_text_snapshot(
                931U,
                QStringLiteral("BC"),
                {{1, 1}})));

    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        report);
    const term::Qsg_atlas_buffer_update_summary& glyph_buffer =
        report.render.glyph_buffer;

    if (!prepared ||
        !glyph_buffer.partial_upload ||
        glyph_buffer.full_upload ||
        glyph_buffer.non_dirty_state_upload)
    {
        std::cerr << "atlas glyph row-stable dirty update"
            << " prepared=" << prepared
            << " glyph_full=" << glyph_buffer.full_upload
            << " glyph_partial=" << glyph_buffer.partial_upload
            << " glyph_non_dirty=" << glyph_buffer.non_dirty_state_upload
            << " glyph_uploaded=" << glyph_buffer.uploaded_bytes
            << " glyph_buffer_bytes=" << glyph_buffer.buffer_bytes
            << " row_stable=" << glyph_buffer.row_stable_layout
            << '\n';
    }

    bool ok = true;
    ok &= check(prepared,
        "atlas glyph row-stable dirty update reaches a prepared frame");
    ok &= check(glyph_buffer.row_stable_layout &&
            glyph_buffer.active_instance_count < glyph_buffer.instance_count &&
            report.render.glyph_text_row_capacity >= 8,
        "atlas glyph row-stable dirty update reserves stable row slots");
    ok &= check(glyph_buffer.partial_upload &&
            !glyph_buffer.full_upload &&
            !glyph_buffer.non_dirty_state_upload &&
            glyph_buffer.uploaded_bytes > 0 &&
            glyph_buffer.uploaded_bytes < glyph_buffer.buffer_bytes,
        "atlas glyph row-stable dirty update patches the dirty row only");
    return ok;
}

bool test_atlas_rect_row_stable_dense_graphic_update(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_graphic_snapshot(
                936U,
                1U,
                {{0, 3}})));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered ||
        !pump_atlas_seeded_rect_slots(app, window, surface, baseline_report))
    {
        std::cerr << "FAIL: atlas rect row-stable dense graphic update could not "
            << "seed all rect buffer slots"
            << " prepare_count=" << baseline_report.prepare_count
            << " seeded_slots=" << baseline_report.render.rect_buffer.seeded_slots
            << " frames_in_flight="
            << baseline_report.render.rect_buffer.rhi_frames_in_flight
            << '\n';
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_graphic_snapshot(
                937U,
                2U,
                {{1, 1}})));

    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        report);
    const term::Qsg_atlas_buffer_update_summary& rect_buffer =
        report.render.rect_buffer;

    if (!prepared ||
        !rect_buffer.partial_upload ||
        rect_buffer.full_upload ||
        rect_buffer.non_dirty_state_upload)
    {
        std::cerr << "atlas rect row-stable dense graphic update"
            << " prepared=" << prepared
            << " rect_full=" << rect_buffer.full_upload
            << " rect_partial=" << rect_buffer.partial_upload
            << " rect_non_dirty=" << rect_buffer.non_dirty_state_upload
            << " rect_uploaded=" << rect_buffer.uploaded_bytes
            << " rect_buffer_bytes=" << rect_buffer.buffer_bytes
            << " row_stable=" << rect_buffer.row_stable_layout
            << " active=" << rect_buffer.active_instance_count
            << " instances=" << rect_buffer.instance_count
            << " rect_row_capacity=" << report.render.rect_row_capacity
            << '\n';
    }

    bool ok = true;
    ok &= check(prepared,
        "atlas rect row-stable dense graphic update reaches a prepared frame");
    ok &= check(rect_buffer.row_stable_layout &&
            rect_buffer.active_instance_count < rect_buffer.instance_count &&
            report.render.rect_row_capacity >= 8,
        "atlas rect row-stable dense graphic update reserves stable row slots");
    ok &= check(report.frame_build.rect_instances ==
            rect_buffer.active_instance_count,
        "atlas rect row-stable dense graphic update reports active logical rect instances");
    ok &= check(rect_buffer.partial_upload &&
            !rect_buffer.full_upload &&
            !rect_buffer.non_dirty_state_upload &&
            rect_buffer.uploaded_bytes > 0 &&
            rect_buffer.uploaded_bytes < rect_buffer.buffer_bytes,
        "atlas rect row-stable dense graphic update patches dirty-row rect bytes");
    ok &= check(report.frame_build.frame_row_descriptors == 0 &&
            report.frame_build.frame_layer_descriptors == 5 &&
            report.frame_build.qsg_layer_descriptors == 0,
        "atlas rect row-stable dense graphic update skips unused descriptors");
    return ok;
}

bool test_atlas_rect_row_stable_graphic_arc_update(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_graphic_arc_snapshot(
                938U,
                1U,
                {{0, 3}})));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered ||
        !pump_atlas_seeded_rect_slots(app, window, surface, baseline_report))
    {
        std::cerr << "FAIL: atlas rect row-stable graphic arc update could not "
            << "seed all rect buffer slots"
            << " prepare_count=" << baseline_report.prepare_count
            << " seeded_slots=" << baseline_report.render.rect_buffer.seeded_slots
            << " frames_in_flight="
            << baseline_report.render.rect_buffer.rhi_frames_in_flight
            << '\n';
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_graphic_arc_snapshot(
                939U,
                2U,
                {{1, 1}})));

    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        report);
    const term::Qsg_atlas_buffer_update_summary& rect_buffer =
        report.render.rect_buffer;

    if (!prepared ||
        !rect_buffer.partial_upload ||
        rect_buffer.full_upload ||
        rect_buffer.non_dirty_state_upload)
    {
        std::cerr << "atlas rect row-stable graphic arc update"
            << " prepared=" << prepared
            << " rect_full=" << rect_buffer.full_upload
            << " rect_partial=" << rect_buffer.partial_upload
            << " rect_non_dirty=" << rect_buffer.non_dirty_state_upload
            << " rect_uploaded=" << rect_buffer.uploaded_bytes
            << " rect_buffer_bytes=" << rect_buffer.buffer_bytes
            << " row_stable=" << rect_buffer.row_stable_layout
            << " active=" << rect_buffer.active_instance_count
            << " instances=" << rect_buffer.instance_count
            << " frame_arcs=" << report.frame_build.frame_graphic_arcs
            << " rect_row_capacity=" << report.render.rect_row_capacity
            << '\n';
    }

    bool ok = true;
    ok &= check(prepared,
        "atlas rect row-stable graphic arc update reaches a prepared frame");
    ok &= check(report.frame_build.frame_graphic_arcs > 0 &&
            rect_buffer.row_stable_layout &&
            rect_buffer.active_instance_count < rect_buffer.instance_count &&
            report.render.rect_row_capacity >= 8,
        "atlas rect row-stable graphic arc update reserves stable arc slots");
    ok &= check(rect_buffer.partial_upload &&
            !rect_buffer.full_upload &&
            !rect_buffer.non_dirty_state_upload &&
            rect_buffer.uploaded_bytes > 0 &&
            rect_buffer.uploaded_bytes < rect_buffer.buffer_bytes,
        "atlas rect row-stable graphic arc update patches dirty-row arc bytes");
    return ok;
}

bool test_atlas_rect_row_stable_styled_blank_background_update(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_background_snapshot(
                946U,
                1U,
                {{0, 3}})));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered ||
        !pump_atlas_seeded_rect_slots(app, window, surface, baseline_report))
    {
        std::cerr << "FAIL: atlas rect row-stable styled blank background update "
            << "could not seed all rect buffer slots"
            << " prepare_count=" << baseline_report.prepare_count
            << " seeded_slots=" << baseline_report.render.rect_buffer.seeded_slots
            << " frames_in_flight="
            << baseline_report.render.rect_buffer.rhi_frames_in_flight
            << '\n';
        return false;
    }

    const term::Terminal_render_snapshot mutated =
        make_atlas_row_stable_background_snapshot(
            947U,
            2U,
            {{1, 1}});
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(mutated));

    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        report);
    const term::Qsg_atlas_buffer_update_summary& rect_buffer =
        report.render.rect_buffer;

    if (!prepared ||
        !rect_buffer.partial_upload ||
        rect_buffer.full_upload ||
        rect_buffer.non_dirty_state_upload ||
        report.frame_build.frame_background_rects <= 1)
    {
        std::cerr << "atlas rect row-stable styled blank background update"
            << " prepared=" << prepared
            << " rect_full=" << rect_buffer.full_upload
            << " rect_partial=" << rect_buffer.partial_upload
            << " rect_non_dirty=" << rect_buffer.non_dirty_state_upload
            << " rect_uploaded=" << rect_buffer.uploaded_bytes
            << " rect_buffer_bytes=" << rect_buffer.buffer_bytes
            << " row_stable=" << rect_buffer.row_stable_layout
            << " active=" << rect_buffer.active_instance_count
            << " instances=" << rect_buffer.instance_count
            << " rect_row_capacity=" << report.render.rect_row_capacity
            << " frame_background_rects=" << report.frame_build.frame_background_rects
            << " frame_rows=" << report.frame_build.frame_row_descriptors
            << '\n';
    }

    bool ok = true;
    ok &= check(prepared,
        "atlas rect row-stable styled blank background update reaches a prepared frame");
    ok &= check(rect_buffer.row_stable_layout &&
            rect_buffer.active_instance_count < rect_buffer.instance_count &&
            report.render.rect_row_capacity >= 8,
        "atlas rect row-stable styled blank background update reserves stable row slots");
    ok &= check(report.frame_build.frame_background_rects > 1 &&
            report.frame_build.frame_row_descriptors == 0 &&
            report.frame_build.frame_layer_descriptors == 5,
        "atlas rect row-stable styled blank background update skips unused descriptors");
    ok &= check(rect_buffer.partial_upload &&
            !rect_buffer.full_upload &&
            !rect_buffer.non_dirty_state_upload &&
            rect_buffer.uploaded_bytes > 0 &&
            rect_buffer.uploaded_bytes < rect_buffer.buffer_bytes,
        "atlas rect row-stable styled blank background update patches dirty-row background bytes");

    surface.update();
    window.requestUpdate();
    app.processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);
    const QImage rendered = window.grabWindow();
    const qreal dpr = pixel_window_device_pixel_ratio(window);
    const term::terminal_cell_metrics_t metrics =
        pixel_metrics(dpr, 18.0, QStringLiteral("monospace"));
    const auto row_center_pixel = [&](int row) {
        const QRect sample_rect = logical_rect_to_pixels_for_dpr(
            inset_rect(pixel_cell_rect(row, 0, 8, metrics), 2.0, 2.0),
            dpr).intersected(rendered.rect());
        if (rendered.isNull() || sample_rect.isEmpty()) {
            return QColor();
        }
        return rendered.pixelColor(sample_rect.center());
    };
    const QColor row_0_pixel = row_center_pixel(0);
    const QColor row_1_pixel = row_center_pixel(1);
    const QColor row_2_pixel = row_center_pixel(2);
    const QColor dark_gray(0x30, 0x30, 0x30);
    const QColor middle_gray(0x50, 0x50, 0x50);
    if (rendered.isNull() ||
        pixel_delta(row_0_pixel, dark_gray) > 4 ||
        pixel_delta(row_1_pixel, middle_gray) > 4 ||
        pixel_delta(row_2_pixel, dark_gray) > 4)
    {
        std::cerr << "atlas rect row-stable styled blank background pixels"
            << " image_null=" << rendered.isNull()
            << " row0=" << row_0_pixel.red() << ','
            << row_0_pixel.green() << ',' << row_0_pixel.blue()
            << " row1=" << row_1_pixel.red() << ','
            << row_1_pixel.green() << ',' << row_1_pixel.blue()
            << " row2=" << row_2_pixel.red() << ','
            << row_2_pixel.green() << ',' << row_2_pixel.blue()
            << " dpr=" << dpr
            << " cell=" << metrics.width << 'x' << metrics.height
            << '\n';
    }
    ok &= check(!rendered.isNull() &&
            pixel_delta(row_0_pixel, dark_gray) <= 4 &&
            pixel_delta(row_1_pixel, middle_gray) <= 4 &&
            pixel_delta(row_2_pixel, dark_gray) <= 4,
        "atlas rect row-stable styled blank background update paints expected pixels");
    return ok;
}

bool test_atlas_glyph_row_stable_wide_update(QGuiApplication& app)
{
    const term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            946U,
            QString::fromUtf8("\xe7\x95\x8c"),
            2,
            true,
            {{0, 3}});
    const term::Terminal_render_snapshot mutated =
        make_atlas_row_stable_text_snapshot(
            947U,
            QString::fromUtf8("\xe8\xaa\x9e"),
            2,
            true,
            {{1, 1}});
    return run_atlas_glyph_row_stable_report_case(
        app,
        "wide glyph row",
        baseline,
        mutated,
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.glyph_text_row_capacity > 0 &&
                render_summary.shaped_text_runs > 0;
        });
}

bool test_atlas_glyph_row_stable_combining_update(QGuiApplication& app)
{
    const term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            950U,
            QString::fromUtf8("e\xcc\x81"),
            1,
            false,
            {{0, 3}});
    const term::Terminal_render_snapshot mutated =
        make_atlas_row_stable_text_snapshot(
            951U,
            QString::fromUtf8("o\xcc\x82"),
            1,
            false,
            {{1, 1}});
    return run_atlas_glyph_row_stable_report_case(
        app,
        "combining-mark row",
        baseline,
        mutated,
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.glyph_text_row_capacity > 0 &&
                render_summary.shaped_text_runs > 0;
        });
}

bool test_atlas_glyph_row_stable_cursor_dirty_update(QGuiApplication& app)
{
    term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            960U,
            QStringLiteral("."),
            1,
            false,
            {{0, 3}});
    term::Terminal_render_snapshot mutated =
        make_atlas_row_stable_text_snapshot(
            961U,
            QStringLiteral("!"),
            1,
            false,
            {{1, 1}});
    baseline.cursor.position      = {1, 0};
    baseline.cursor.visible       = true;
    baseline.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    baseline.cursor.blink_enabled = false;
    mutated.cursor.position      = {1, 0};
    mutated.cursor.visible       = true;
    mutated.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    mutated.cursor.blink_enabled = false;

    return run_atlas_glyph_row_stable_report_case(
        app,
        "cursor glyph row",
        baseline,
        mutated,
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.glyph_cursor_text_row_capacity > 0 &&
                !render_summary.non_dirty_cursor_invalidation;
        });
}

bool test_atlas_glyph_row_stable_cursor_clean_row_promoted(QGuiApplication& app)
{
    term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            970U,
            QStringLiteral("."),
            1,
            false,
            {{0, 3}});
    term::Terminal_render_snapshot mutated =
        make_atlas_row_stable_text_snapshot(
            971U,
            QStringLiteral("!"),
            1,
            false,
            {{1, 1}});
    baseline.cells.push_back(
        make_pixel_cell(
            0,
            1,
            QStringLiteral("D"),
            1,
            term::k_default_terminal_style_id));
    mutated.cells.push_back(
        make_pixel_cell(
            0,
            1,
            QStringLiteral("D"),
            1,
            term::k_default_terminal_style_id));
    baseline.cursor.position      = {0, 0};
    baseline.cursor.visible       = true;
    baseline.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    baseline.cursor.blink_enabled = false;
    mutated.cursor.position      = {0, 1};
    mutated.cursor.visible       = true;
    mutated.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    mutated.cursor.blink_enabled = false;

    return run_atlas_glyph_row_stable_report_case(
        app,
        "clean-row cursor glyph",
        baseline,
        mutated,
        [](const term::Qsg_atlas_render_summary& render_summary) {
            const term::Qsg_atlas_buffer_update_summary& rect_buffer =
                render_summary.rect_buffer;
            return rect_buffer.full_upload &&
                !rect_buffer.partial_upload &&
                rect_buffer.non_dirty_state_upload &&
                rect_buffer.uploaded_bytes == rect_buffer.buffer_bytes &&
                render_summary.glyph_cursor_text_row_capacity > 0 &&
                !render_summary.non_dirty_cursor_invalidation;
        });
}

term::Terminal_render_snapshot make_atlas_prepared_text_reuse_snapshot(
    std::uint64_t sequence,
    QString       middle_text,
    std::uint64_t middle_content_generation,
    std::vector<term::Terminal_render_dirty_row_range>
                  dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot =
        make_atlas_row_stable_text_snapshot(
            sequence,
            std::move(middle_text),
            std::move(dirty_row_ranges));
    for (term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == 0 && cell.position.column == 0) {
            cell = make_pixel_cell(
                0,
                0,
                QStringLiteral("\u00e1"),
                1,
                term::k_default_terminal_style_id);
        }
        else if (cell.position.row == 2 && cell.position.column == 0) {
            cell = make_pixel_cell(
                2,
                0,
                QStringLiteral("\u00e7"),
                1,
                term::k_default_terminal_style_id);
        }
    }
    if (snapshot.visible_line_provenance.size() > 1U) {
        snapshot.visible_line_provenance[1].content_generation =
            middle_content_generation;
    }
    return snapshot;
}

bool prepare_seeded_atlas_text_surface(
    QGuiApplication&                         app,
    QQuickWindow&                            window,
    VNM_TerminalSurface&                     surface,
    const term::Terminal_render_snapshot&    snapshot,
    term::Qsg_atlas_frame_report&            out_report)
{
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(snapshot));
    window.show();
    const bool rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    out_report = term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    return rendered && pump_atlas_seeded_glyph_slots(app, window, surface, out_report);
}

bool pump_prepared_text_reuse_report(
    QGuiApplication&                         app,
    QQuickWindow&                            window,
    VNM_TerminalSurface&                     surface,
    const term::Terminal_render_snapshot&    snapshot,
    std::uint64_t                            previous_prepare_count,
    term::Qsg_atlas_frame_report&            out_report)
{
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(snapshot));
    out_report = term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    return pump_next_atlas_report(
        app,
        window,
        surface,
        previous_prepare_count,
        out_report);
}

bool check_prepared_text_reuse_drawn_report(
    const term::Qsg_atlas_frame_report& report,
    const char*                         name)
{
    (void)name;
    const term::Qsg_atlas_frame_build_summary& frame_build =
        report.frame_build;
    const term::Qsg_atlas_render_summary& render = report.render;

    bool ok = true;
    ok &= check(report.render_count > 0U && report.drew,
        "atlas prepared text reuse drew a frame");
    ok &= check(frame_build.frame_text_runs > 0 &&
            frame_build.glyph_instances > 0,
        "atlas prepared text reuse emitted atlas glyph instances");
    ok &= check(render.glyph_buffer_instances > 0 &&
            render.glyph_buffer.active_instance_count > 0 &&
            render.glyph_draw_calls > 0,
        "atlas prepared text reuse submitted glyph buffer draw work");
    ok &= check(frame_build.glyph_missed_instances == 0 &&
            frame_build.glyph_coverage_failures == 0 &&
            frame_build.glyph_atlas_insert_failures == 0,
        "atlas prepared text reuse had no glyph misses");
    return ok;
}

bool test_atlas_prepared_text_reuse(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    const term::Terminal_render_snapshot baseline =
        make_atlas_prepared_text_reuse_snapshot(
            980U,
            QStringLiteral("\u00e9"),
            1U,
            {{0, 3}});

    term::Qsg_atlas_frame_report baseline_report;
    if (!prepare_seeded_atlas_text_surface(
            app,
            window,
            surface,
            baseline,
            baseline_report))
    {
        std::cerr << "FAIL: atlas prepared text reuse could not seed baseline"
            << " prepare_count=" << baseline_report.prepare_count
            << " render_count=" << baseline_report.render_count
            << " seeded_slots="
            << baseline_report.render.glyph_buffer.seeded_slots
            << '\n';
        return false;
    }

    term::Qsg_atlas_frame_report unchanged_report;
    term::Terminal_render_snapshot unchanged =
        make_atlas_prepared_text_reuse_snapshot(
            981U,
            QStringLiteral("\u00e9"),
            1U,
            {});
    const bool unchanged_prepared = pump_prepared_text_reuse_report(
        app,
        window,
        surface,
        unchanged,
        baseline_report.prepare_count,
        unchanged_report);

    term::Qsg_atlas_frame_report dirty_report;
    term::Terminal_render_snapshot dirty =
        make_atlas_prepared_text_reuse_snapshot(
            982U,
            QStringLiteral("\u00ea"),
            2U,
            {{1, 1}});
    const bool dirty_prepared = pump_prepared_text_reuse_report(
        app,
        window,
        surface,
        dirty,
        unchanged_report.prepare_count,
        dirty_report);

    term::Qsg_atlas_frame_report pruned_report;
    term::Terminal_render_snapshot pruned =
        make_atlas_prepared_text_reuse_snapshot(
            983U,
            QStringLiteral("\u00ea"),
            2U,
            {{2, 1}});
    pruned.cells.erase(
        std::remove_if(
            pruned.cells.begin(),
            pruned.cells.end(),
            [](const term::Terminal_render_cell& cell) {
                return cell.position.row == 2;
            }),
        pruned.cells.end());
    const bool prune_prepared = pump_prepared_text_reuse_report(
        app,
        window,
        surface,
        pruned,
        dirty_report.prepare_count,
        pruned_report);

    const term::Qsg_atlas_producer_summary& unchanged_producer =
        unchanged_report.producer;
    const term::Qsg_atlas_producer_summary& dirty_producer =
        dirty_report.producer;
    const term::Qsg_atlas_producer_summary& pruned_producer =
        pruned_report.producer;

    bool ok = true;
    ok &= check(unchanged_prepared,
        "atlas prepared text reuse renders unchanged retained content");
    ok &= check_prepared_text_reuse_drawn_report(
        unchanged_report,
        "unchanged retained frame");
    ok &= check(unchanged_producer.shaped_runs_built == 0 &&
            unchanged_producer.shaped_runs_reused >= 3 &&
            unchanged_producer.shaped_glyph_records_reused >= 3,
        "atlas prepared text reuse avoids reshaping unchanged retained runs");
    ok &= check(dirty_prepared,
        "atlas prepared text reuse renders one dirty text row");
    ok &= check_prepared_text_reuse_drawn_report(
        dirty_report,
        "dirty retained frame");
    ok &= check(dirty_producer.shaped_runs_built == 1 &&
            dirty_producer.shaped_runs_reused >= 2,
        "atlas prepared text reuse bounds reshaping to the changed retained row");
    ok &= check(prune_prepared,
        "atlas prepared text reuse renders after visible text removal");
    ok &= check_prepared_text_reuse_drawn_report(
        pruned_report,
        "pruned retained frame");
    ok &= check(pruned_producer.shape_cache_pruned >= 1,
        "atlas prepared text reuse prunes entries not seen in the visible frame");
    return ok;
}

bool run_atlas_report_case(
    QGuiApplication& app,
    const char*      name,
    const std::function<void(VNM_TerminalSurface&, term::Terminal_render_snapshot&)>&
                     mutate,
    const std::function<bool(const term::Qsg_atlas_render_summary&)>&
                     expected,
    bool             set_snapshot_before_mutate = false)
{
    QQuickWindow window;
    window.resize(280, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_report_snapshot(900U)));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    const term::Qsg_atlas_frame_report baseline_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered) {
        std::cerr << "FAIL: atlas report case " << name
            << " lost usable QRhi render state after backend probe\n";
        return false;
    }

    term::Terminal_render_snapshot mutated = make_atlas_report_snapshot(901U);
    mutated.dirty_row_ranges.clear();
    if (set_snapshot_before_mutate) {
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            surface,
            std::make_shared<const term::Terminal_render_snapshot>(mutated));
        mutate(surface, mutated);
    }
    else
    {
        mutate(surface, mutated);
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            surface,
            std::make_shared<const term::Terminal_render_snapshot>(mutated));
    }

    const std::uint64_t previous_prepare_count = baseline_report.prepare_count;
    term::Qsg_atlas_frame_report report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report(
        app,
        window,
        surface,
        previous_prepare_count,
        report);
    const bool reported = prepared && expected(report.render);
    if (!reported) {
        const term::Qsg_atlas_render_summary& render_summary = report.render;
        std::cerr << "atlas report case " << name
            << " prepare_count=" << report.prepare_count
            << " previous_prepare_count=" << previous_prepare_count
            << " selection=" << render_summary.non_dirty_selection_invalidation
            << " cursor=" << render_summary.non_dirty_cursor_invalidation
            << " preedit=" << render_summary.non_dirty_preedit_invalidation
            << " options=" << render_summary.non_dirty_options_invalidation
            << " visual_bell=" << render_summary.non_dirty_visual_bell_invalidation
            << " rect_full=" << render_summary.rect_buffer.full_upload
            << " glyph_full=" << render_summary.glyph_buffer.full_upload
            << '\n';
    }

    bool ok = true;
    ok &= check(reported, std::string("atlas report invalidation case ") + name);
    ok &= check(report.render.rect_buffer.full_upload ||
            report.render.glyph_buffer.full_upload,
        std::string("atlas report full-uploads at least one instance buffer for ") +
            name);
    return ok;
}

void configure_atlas_prepare_transaction_surface(
    QQuickWindow&        window,
    VNM_TerminalSurface& surface)
{
    window.resize(280, 160);
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(220.0, 110.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
}

void configure_atlas_prepare_transaction_forced_msdf_surface(
    QQuickWindow&        window,
    VNM_TerminalSurface& surface)
{
    window.resize(420, 140);
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 90.0));
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::MSDF);
}

void configure_atlas_prepare_transaction_auto_msdf_surface(
    QQuickWindow&        window,
    VNM_TerminalSurface& surface)
{
    window.resize(420, 140);
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 90.0));
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    surface.set_color_scheme(QStringLiteral("Campbell"));
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::AUTO);
}

bool seed_atlas_prepare_transaction_baseline(
    QGuiApplication&                      app,
    QQuickWindow&                         window,
    VNM_TerminalSurface&                  surface,
    const term::Terminal_render_snapshot& baseline,
    const char*                           name,
    term::Qsg_atlas_frame_report&         report)
{
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(baseline));
    window.show();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        atlas_report_render_state_ready);
    report = term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool seeded =
        baseline_rendered &&
        pump_atlas_seeded_glyph_slots(app, window, surface, report);
    if (!seeded) {
        std::cerr << "FAIL: atlas prepare transaction " << name
            << " could not seed glyph buffer slots"
            << " prepare_count=" << report.prepare_count
            << " seeded_slots=" << report.render.glyph_buffer.seeded_slots
            << " frames_in_flight="
            << report.render.glyph_buffer.rhi_frames_in_flight
            << '\n';
    }
    return seeded;
}

bool seed_atlas_prepare_transaction_msdf_baseline(
    QGuiApplication&              app,
    QQuickWindow&                 window,
    VNM_TerminalSurface&          surface,
    std::uint64_t                 sequence,
    const char*                   name,
    term::Qsg_atlas_frame_report& report,
    bool&                         skipped)
{
    skipped = false;
#if !VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_ENABLED
    std::cerr << "SKIP: atlas MSDF transaction " << name
        << " requires the MSDF text renderer test dependency\n";
    skipped = true;
    (void)app;
    (void)window;
    (void)surface;
    (void)sequence;
    (void)report;
    return false;
#else
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(sequence, false)));
    window.show();
    const int expected_glyphs =
        atlas_msdf_resource_stability_expected_glyphs();
    const bool baseline_rendered = pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& current_report) {
            return
                atlas_report_render_state_ready(current_report) &&
                current_report.render.msdf_text_renderer_active &&
                current_report.render.msdf_text_resources_ready &&
                current_report.render.msdf_text_texture_ready &&
                current_report.render.msdf_text_glyph_instances ==
                    expected_glyphs;
        });
    report = term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    if (!baseline_rendered) {
        if (!report.render.msdf_text_renderer_enabled ||
            window_uses_known_software_adapter(window))
        {
            std::cerr << "SKIP: atlas MSDF transaction " << name
                << " cannot activate forced MSDF on this backend\n";
            skipped = true;
            return false;
        }

        std::cerr << "FAIL: atlas MSDF transaction " << name
            << " could not reach a ready MSDF baseline"
            << " prepare_count=" << report.prepare_count
            << " render_count=" << report.render_count
            << " active=" << report.render.msdf_text_renderer_active
            << " resources_ready=" << report.render.msdf_text_resources_ready
            << " texture_ready=" << report.render.msdf_text_texture_ready
            << " msdf_instances="
            << report.render.msdf_text_glyph_instances
            << '\n';
        return false;
    }

    return true;
#endif
}

bool atlas_buffer_has_no_accepted_upload(
    const term::Qsg_atlas_buffer_update_summary& buffer)
{
    return
        !buffer.full_upload      &&
        !buffer.partial_upload   &&
        !buffer.skipped_upload   &&
        buffer.uploaded_bytes == 0;
}

bool atlas_failed_prepare_has_no_buffer_upload(
    const term::Qsg_atlas_frame_report& report)
{
    return
        atlas_buffer_has_no_accepted_upload(report.render.rect_buffer)  &&
        atlas_buffer_has_no_accepted_upload(report.render.glyph_buffer) &&
        atlas_buffer_has_no_accepted_upload(report.render.msdf_text_buffer);
}

bool test_atlas_failed_prepare_forces_next_sparse_full_upload(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_surface(window, surface);

    const term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            19830U,
            QStringLiteral("B"),
            {{0, 3}});
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_baseline(
            app,
            window,
            surface,
            baseline,
            "failed sparse upload",
            baseline_report))
    {
        return false;
    }

    term::qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(19831U);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_text_snapshot(
                19831U,
                QStringLiteral("BC"),
                {{1, 1}})));
    term::Qsg_atlas_frame_report failed_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool failed_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19831U,
        failed_report);

    const term::Terminal_render_snapshot next_sparse =
        make_atlas_row_stable_text_snapshot(
            19832U,
            QStringLiteral("BD"),
            {{2, 1}});
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(next_sparse));
    term::Qsg_atlas_frame_report next_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool next_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        failed_report.prepare_count,
        19832U,
        next_report);
    const term::Qsg_atlas_buffer_update_summary& glyph_buffer =
        next_report.render.glyph_buffer;

    bool ok = true;
    ok &= check(failed_prepared && atlas_failed_prepare_has_no_buffer_upload(failed_report),
        "atlas failed sparse prepare records no accepted buffer upload");
    ok &= check(next_prepared && atlas_report_render_state_ready(next_report),
        "atlas sparse frame after failed prepare renders");
    ok &= check(glyph_buffer.full_upload &&
            glyph_buffer.full_repaint_upload &&
            !glyph_buffer.partial_upload &&
            next_report.frame_build.frame_row_descriptors == 0 &&
            next_report.frame_build.frame_layer_descriptors == 5,
        "atlas sparse frame after failed prepare uses a conservative full upload");
    term::qsg_atlas_clear_resource_prepare_failure_for_testing();
    return ok;
}

bool test_atlas_failed_prepare_preserves_font_epoch_basis(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_surface(window, surface);

    const term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            19833U,
            QStringLiteral("B"),
            {{0, 3}});
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_baseline(
            app,
            window,
            surface,
            baseline,
            "font epoch",
            baseline_report))
    {
        return false;
    }

    term::qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(19834U);
    surface.set_font_size(20.0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_text_snapshot(
                19834U,
                QStringLiteral("BC"),
                {{1, 1}})));
    term::Qsg_atlas_frame_report failed_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool failed_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19834U,
        failed_report);

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_row_stable_text_snapshot(
                19835U,
                QStringLiteral("BD"),
                {{2, 1}})));
    term::Qsg_atlas_frame_report next_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool next_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        failed_report.prepare_count,
        19835U,
        next_report);

    bool ok = true;
    ok &= check(failed_prepared && atlas_failed_prepare_has_no_buffer_upload(failed_report),
        "atlas failed font-epoch prepare records no accepted buffer upload");
    ok &= check(next_prepared && atlas_report_render_state_ready(next_report),
        "atlas frame after failed font-epoch prepare renders");
    ok &= check(next_report.render.font_epoch_invalidation &&
            next_report.render.glyph_buffer.full_upload &&
            next_report.render.glyph_buffer.full_repaint_upload,
        "atlas frame after failed font-epoch prepare keeps the old epoch basis");
    term::qsg_atlas_clear_resource_prepare_failure_for_testing();
    return ok;
}

bool test_atlas_failed_prepare_preserves_cursor_layer_basis(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_surface(window, surface);

    term::Terminal_render_snapshot baseline =
        make_atlas_row_stable_text_snapshot(
            19836U,
            QStringLiteral("B"),
            {{0, 3}});
    baseline.cursor.visible       = true;
    baseline.cursor.blink_enabled = false;
    baseline.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    baseline.cursor.position      = {0, 0};

    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_baseline(
            app,
            window,
            surface,
            baseline,
            "cursor layer",
            baseline_report))
    {
        return false;
    }

    term::Terminal_render_snapshot failed = baseline;
    failed.metadata.sequence = 19837U;
    failed.dirty_row_ranges.clear();
    failed.cursor.position = {1, 0};
    term::qsg_atlas_fail_resource_prepare_for_snapshot_sequence_for_testing(19837U);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(failed));
    term::Qsg_atlas_frame_report failed_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool failed_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19837U,
        failed_report);

    term::Terminal_render_snapshot next = failed;
    next.metadata.sequence = 19838U;
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(next));
    term::Qsg_atlas_frame_report next_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool next_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        failed_report.prepare_count,
        19838U,
        next_report);

    bool ok = true;
    ok &= check(failed_prepared && atlas_failed_prepare_has_no_buffer_upload(failed_report),
        "atlas failed cursor prepare records no accepted buffer upload");
    ok &= check(next_prepared && atlas_report_render_state_ready(next_report),
        "atlas frame after failed cursor prepare renders");
    ok &= check(next_report.render.non_dirty_cursor_invalidation &&
            next_report.render.rect_buffer.full_upload &&
            next_report.render.rect_buffer.non_dirty_state_upload,
        "atlas frame after failed cursor prepare keeps the old cursor-row basis");
    term::qsg_atlas_clear_resource_prepare_failure_for_testing();
    return ok;
}

bool test_atlas_forced_msdf_prepare_resource_failure_does_not_commit(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_forced_msdf_surface(window, surface);

    bool skipped = false;
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_msdf_baseline(
            app,
            window,
            surface,
            19839U,
            "prepare resources",
            baseline_report,
            skipped))
    {
        return skipped;
    }

    term::qsg_atlas_fail_msdf_resource_prepare_for_snapshot_sequence_for_testing(
        19840U);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(19840U, true)));

    term::Qsg_atlas_frame_report failed_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool failed_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19840U,
        failed_report);
    term::qsg_atlas_clear_msdf_resource_failures_for_testing();

    bool ok = true;
    ok &= check(failed_prepared,
        "atlas forced MSDF prepare-resource failure reports the failed sequence");
    ok &= check(failed_prepared &&
            !atlas_report_render_state_ready(failed_report),
        "atlas forced MSDF prepare-resource failure does not render ready");
    ok &= check(failed_prepared &&
            atlas_failed_prepare_has_no_buffer_upload(failed_report),
        "atlas forced MSDF prepare-resource failure records no accepted buffer upload");
    return ok;
}

bool test_atlas_forced_msdf_buffer_failure_does_not_commit(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_forced_msdf_surface(window, surface);

    bool skipped = false;
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_msdf_baseline(
            app,
            window,
            surface,
            19841U,
            "instance buffer",
            baseline_report,
            skipped))
    {
        return skipped;
    }

    term::qsg_atlas_fail_msdf_text_buffer_update_for_snapshot_sequence_for_testing(
        19842U);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(19842U, true)));

    term::Qsg_atlas_frame_report failed_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool failed_prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19842U,
        failed_report);
    term::qsg_atlas_clear_msdf_resource_failures_for_testing();

    bool ok = true;
    ok &= check(failed_prepared,
        "atlas forced MSDF instance-buffer failure reports the failed sequence");
    ok &= check(failed_prepared &&
            !atlas_report_render_state_ready(failed_report),
        "atlas forced MSDF instance-buffer failure does not render ready");
    ok &= check(failed_prepared &&
            atlas_failed_prepare_has_no_buffer_upload(failed_report),
        "atlas forced MSDF instance-buffer failure records no accepted buffer upload");
    return ok;
}

bool test_atlas_auto_msdf_prepare_resource_failure_falls_back(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_auto_msdf_surface(window, surface);

    bool skipped = false;
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_msdf_baseline(
            app,
            window,
            surface,
            19843U,
            "auto fallback",
            baseline_report,
            skipped))
    {
        return skipped;
    }

    term::qsg_atlas_fail_msdf_resource_prepare_for_snapshot_sequence_for_testing(
        19844U);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(19844U, true)));

    term::Qsg_atlas_frame_report fallback_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        19844U,
        fallback_report);
    term::qsg_atlas_clear_msdf_resource_failures_for_testing();

    const term::Qsg_atlas_render_summary& render = fallback_report.render;
    bool ok = true;
    ok &= check(prepared && atlas_report_render_state_ready(fallback_report),
        "atlas AUTO MSDF prepare-resource failure renders via fallback");
    ok &= check(render.text_renderer_fallback_used &&
            render.effective_text_renderer ==
                term::Terminal_text_renderer_kind::GLYPH &&
            render.glyph_draw_calls > 0 &&
            render.glyph_buffer_instances > 0,
        "atlas AUTO MSDF prepare-resource failure uses glyph fallback");
    ok &= check(!render.msdf_text_resources_ready &&
            render.msdf_text_missed_supported_runs > 0,
        "atlas AUTO MSDF prepare-resource failure preserves MSDF diagnostics");
    return ok;
}

bool test_atlas_auto_msdf_buffer_failure_falls_back(
    QGuiApplication& app)
{
    QQuickWindow window;
    VNM_TerminalSurface surface;
    configure_atlas_prepare_transaction_auto_msdf_surface(window, surface);

    bool skipped = false;
    term::Qsg_atlas_frame_report baseline_report;
    if (!seed_atlas_prepare_transaction_msdf_baseline(
            app,
            window,
            surface,
            19845U,
            "auto instance-buffer fallback",
            baseline_report,
            skipped))
    {
        return skipped;
    }

    constexpr std::uint64_t k_fallback_sequence = 19846U;
    term::qsg_atlas_fail_msdf_text_buffer_update_for_snapshot_sequence_for_testing(
        k_fallback_sequence);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_atlas_msdf_resource_stability_snapshot(
                k_fallback_sequence,
                true)));

    term::Qsg_atlas_frame_report fallback_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const bool prepared = pump_next_atlas_report_for_sequence(
        app,
        window,
        surface,
        baseline_report.prepare_count,
        k_fallback_sequence,
        fallback_report);
    term::qsg_atlas_clear_msdf_resource_failures_for_testing();

    const int expected_glyphs =
        atlas_msdf_resource_stability_expected_glyphs();
    const term::Qsg_atlas_render_summary& render = fallback_report.render;
    bool ok = true;
    ok &= check(prepared && atlas_report_render_state_ready(fallback_report),
        "atlas AUTO MSDF instance-buffer failure renders via fallback");
    ok &= check(render.text_renderer_fallback_used &&
            render.effective_text_renderer ==
                term::Terminal_text_renderer_kind::GLYPH &&
            render.glyph_draw_calls > 0 &&
            render.glyph_buffer_instances > 0,
        "atlas AUTO MSDF instance-buffer failure uses glyph fallback");
    ok &= check(!render.msdf_text_renderer_active &&
            !render.msdf_text_resources_ready &&
            render.msdf_text_draw_calls == 0,
        "atlas AUTO MSDF instance-buffer failure disables MSDF renderer state");
    ok &= check(render.msdf_text_supported_runs >=
                baseline_report.render.msdf_text_supported_runs &&
            render.msdf_text_missed_supported_runs >
                baseline_report.render.msdf_text_missed_supported_runs &&
            render.msdf_text_missed_supported_glyphs >= expected_glyphs,
        "atlas AUTO MSDF instance-buffer failure preserves MSDF diagnostics");
    return ok;
}

bool atlas_report_backend_usable(
    QGuiApplication& app,
    qreal            device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    Pixel_parity_fixture probe = make_layout_parity_base_fixture(
        "atlas_backend_probe",
        {3, 8},
        899U,
        metrics,
        dpr);
    probe.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    const Pixel_render_result atlas =
        render_pixel_atlas_fixture(app, probe);
    const bool rendered =
        atlas.ready &&
        atlas_report_render_state_ready(atlas.atlas_report);
    if (!rendered) {
        std::cerr << "SKIP: atlas report backend check did not reach usable QRhi "
            << "render state"
            << " prepare_count=" << atlas.atlas_report.prepare_count
            << " render_count=" << atlas.atlas_report.render_count
            << " rhi_non_null=" << atlas.atlas_report.rhi_non_null
            << '\n';
    }
    return rendered;
}

term::Terminal_render_snapshot make_warm_lazy_seed_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({2, 16}, sequence);
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 2, QString::fromUtf8("\xce\xa9"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 4, QString::fromUtf8("\xd0\x91"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 6, QStringLiteral("\u2500"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 8, QStringLiteral("\u2588"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 10, QStringLiteral("\u2801"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 12, QString::fromUtf8("\xe4\xb8\xad"), 2, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_continuation_cell(0, 13, term::k_default_terminal_style_id));
    return snapshot;
}

term::Terminal_render_snapshot make_warm_lazy_outside_seed_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_render_snapshot snapshot =
        make_pixel_base_snapshot({2, 16}, sequence);
    snapshot.cells.push_back(
        make_pixel_cell(0, 0, QString::fromUtf8("\xe6\xbc\xa2"), 2, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_continuation_cell(0, 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 3, QString::fromUcs4(U"\U0001F9EA"), 2, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_continuation_cell(0, 4, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 6, QStringLiteral("\u2603"), 1, term::k_default_terminal_style_id));
    snapshot.cells.push_back(
        make_pixel_cell(0, 8, QStringLiteral("\u1e9e"), 1, term::k_default_terminal_style_id));
    return snapshot;
}

bool pump_warm_lazy_report_after(
    QGuiApplication&                 app,
    QQuickWindow&                    window,
    VNM_TerminalSurface&             surface,
    std::uint64_t                    previous_prepare_count,
    term::Qsg_atlas_frame_report&    out_report)
{
    return pump_until(
        app,
        window,
        surface,
        [&](const term::Qsg_atlas_frame_report& report) {
            if (report.prepare_count <= previous_prepare_count ||
                !atlas_report_render_state_ready(report))
            {
                return false;
            }

            out_report = report;
            return true;
        },
        160);
}

void print_warm_lazy_summary(
    const char*                                label,
    const term::Qsg_atlas_warm_lazy_summary&  summary)
{
    std::cerr << "warm/lazy " << label
        << " completed=" << summary.warm_completed
        << " broad_seed_skipped=" << summary.warm_broad_seed_skipped
        << " seed_strings=" << summary.warm_seed_strings
        << " shaped=" << summary.warm_shaped_glyph_records
        << " covered=" << summary.warm_covered_glyph_records
        << " skipped=" << summary.warm_skipped_glyph_records
        << " environment_skipped="
        << summary.warm_environment_skipped_glyph_records
        << " failed_records=" << summary.warm_failed_glyph_records
        << " missing_indexes=" << summary.warm_missing_string_indexes
        << " invalid_indexes=" << summary.warm_invalid_string_indexes
        << " unsupported=" << summary.warm_unsupported_images
        << " warm_failed_inserts=" << summary.warm_failed_inserts
        << " lazy_attempts=" << summary.lazy_insert_attempts
        << " lazy_inserts=" << summary.lazy_inserts
        << " lazy_failed=" << summary.lazy_failed_inserts
        << " lazy_frames=" << summary.lazy_frames
        << " incomplete_frames=" << summary.incomplete_frames
        << '\n';
}

int test_atlas_warm_lazy_smoke(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas warm/lazy smoke");
    if (backend_status != 0) {
        return backend_status;
    }

    const qreal device_pixel_ratio =
        pixel_probe_render_window_device_pixel_ratio(app);
    if (!atlas_report_backend_usable(app, device_pixel_ratio)) {
        return k_unsupported_backend_skip_return_code;
    }

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    const QSizeF logical_size = pixel_logical_size({2, 16}, metrics);

    QQuickWindow window;
    window.setColor(QColor(1, 2, 3));
    window.resize(pixel_window_logical_pixel_size(logical_size));

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(logical_size);
    surface.set_font_family(QString());
    surface.set_font_size(18.0);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
        surface,
        true);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_warm_lazy_seed_snapshot(991U)));

    window.show();
    app.processEvents(QEventLoop::AllEvents, 50);

    term::Qsg_atlas_frame_report seed_report;
    if (!pump_warm_lazy_report_after(app, window, surface, 0U, seed_report)) {
        std::cerr << "FAIL: atlas warm/lazy smoke did not render seed fixture\n";
        return 1;
    }

    const term::Qsg_atlas_warm_lazy_summary& seed = seed_report.warm_lazy;
    if (seed.warm_broad_seed_skipped) {
        bool ok = true;
        ok &= check(!seed.warm_completed,
            "software warm/lazy smoke does not report completed prewarm");
        ok &= check(
            seed.warm_seed_strings ==
                static_cast<int>(term::k_qsg_atlas_warm_seed_strings.size()),
            "software warm/lazy smoke preserves the source-controlled seed count");
        ok &= check(seed.warm_shaped_glyph_records == 0 &&
                seed.warm_covered_glyph_records == 0    &&
                seed.warm_insert_attempts == 0          &&
                seed.warm_inserts == 0                  &&
                seed.warm_failed_inserts == 0           &&
                seed.warm_failed_glyph_records == 0     &&
                seed.warm_unsupported_images == 0,
            "software warm/lazy smoke skips broad prewarm without hidden warm failures");
        if (!ok) {
            print_warm_lazy_summary("seed", seed);
            return 1;
        }
        std::cerr << "SKIP: atlas warm/lazy smoke broad prewarm is disabled "
            << "on this software renderer\n";
        return k_unsupported_backend_skip_return_code;
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        std::make_shared<const term::Terminal_render_snapshot>(
            make_warm_lazy_outside_seed_snapshot(992U)));
    term::Qsg_atlas_frame_report lazy_report;
    if (!pump_warm_lazy_report_after(
            app,
            window,
            surface,
            seed_report.prepare_count,
            lazy_report))
    {
        std::cerr << "FAIL: atlas warm/lazy smoke did not render lazy fixture\n";
        return 1;
    }

    const term::Qsg_atlas_warm_lazy_summary& lazy = lazy_report.warm_lazy;
    bool ok = true;
    ok &= check(seed.warm_completed, "warm/lazy smoke completes atlas prewarm");
    ok &= check(
        seed.warm_seed_strings ==
            static_cast<int>(term::k_qsg_atlas_warm_seed_strings.size()),
        "warm/lazy smoke reports the source-controlled seed count");
    ok &= check(seed.warm_shaped_glyph_records > 0,
        "warm/lazy smoke reports shaped warm glyph records");
    ok &= check(seed.warm_covered_glyph_records > 0,
        "warm/lazy smoke reports covered warm glyph records");
    ok &= check(seed.warm_failed_glyph_records == 0 &&
            seed.warm_missing_string_indexes == 0   &&
            seed.warm_invalid_string_indexes == 0   &&
            seed.warm_unsupported_images == 0,
        "warm/lazy smoke reports no hidden warm seed failures");
    ok &= check(seed.warm_insert_attempts > 0 && seed.warm_inserts > 0,
        "warm/lazy smoke reports warm cache insertions");
    ok &= check(seed.warm_failed_inserts == 0,
        "warm/lazy smoke reports zero warm failed inserts");
    ok &= check(seed.warm_elapsed_ms >= 0.0,
        "warm/lazy smoke reports warm elapsed time");
    ok &= check(seed.lazy_insert_attempts == 0 &&
            seed.lazy_inserts == 0          &&
            seed.lazy_failed_inserts == 0   &&
            seed.lazy_frames == 0,
        "warm/lazy smoke seed fixture has no frame-time lazy insertions");
    ok &= check(seed.incomplete_frames == 0 &&
            seed_report.frame_build.glyph_missed_instances == 0 &&
            seed_report.frame_build.glyph_coverage_failures == 0 &&
            seed_report.frame_build.glyph_atlas_insert_failures == 0,
        "warm/lazy smoke seed fixture reports complete glyph coverage");

    ok &= check(lazy.warm_completed && lazy.warm_epoch == seed.warm_epoch,
        "warm/lazy smoke keeps the same completed warm epoch");
    ok &= check(lazy.warm_failed_glyph_records == 0 &&
            lazy.warm_missing_string_indexes == 0   &&
            lazy.warm_invalid_string_indexes == 0   &&
            lazy.warm_unsupported_images == 0,
        "warm/lazy smoke keeps warm seed failure counters at zero");
    ok &= check(lazy.lazy_insert_attempts > seed.lazy_insert_attempts,
        "warm/lazy smoke records outside-seed frame-time lazy attempts");
    ok &= check(lazy.lazy_inserts > seed.lazy_inserts,
        "warm/lazy smoke inserts outside-seed probes lazily");
    ok &= check(lazy.lazy_failed_inserts == 0,
        "warm/lazy smoke reports zero lazy failed inserts");
    ok &= check(lazy.lazy_frames > seed.lazy_frames,
        "warm/lazy smoke counts the lazy-insertion frame");
    ok &= check(lazy.lazy_elapsed_ms >= seed.lazy_elapsed_ms &&
            lazy.lazy_max_insert_us > 0,
        "warm/lazy smoke reports lazy insertion latency");
    ok &= check(lazy.incomplete_frames == 0 &&
            lazy_report.frame_build.glyph_missed_instances == 0 &&
            lazy_report.frame_build.glyph_coverage_failures == 0 &&
            lazy_report.frame_build.glyph_atlas_insert_failures == 0,
        "warm/lazy smoke lazy fixture reports complete glyph coverage");

    window.hide();
    app.processEvents(QEventLoop::AllEvents, 50);
    if (!ok) {
        print_warm_lazy_summary("seed", seed);
        print_warm_lazy_summary("lazy", lazy);
    }
    return ok ? 0 : 1;
}

struct Lcd_probe_family
{
    QString label;
    QString text;
    term::Glyph_image_presentation presentation = term::Glyph_image_presentation::TEXT;
};

struct Lcd_glyph_probe_record
{
    QString family_label;
    QString font_family;
    QString font_style;
    quint32 glyph_index           = 0U;
    int     glyph_run_index       = 0;
    int     glyph_index_in_run    = 0;
    int     image_format_id       = static_cast<int>(QImage::Format_Invalid);
    QString image_format_name;
    QSize   image_size;
    QString requested_presentation;
    QString coverage_candidate;
    bool    production_tile_valid = false;
};

enum class Raster_variant_kind
{
    ATLAS_CURRENT_PHYSICAL,
    ATLAS_FLOOR_PHYSICAL,
    ATLAS_ROUND_PHYSICAL,
    ATLAS_CEIL_PHYSICAL,
    QT_TEXT_LAYOUT,
};

struct Raster_variant_glyph_image_stats
{
    qreal actual_raster_pixel_size = 0.0;
    int   glyph_image_count        = 0;
    int   valid_tile_count         = 0;
    QSize max_glyph_image_size;
};

struct Raster_variant_probe_record
{
    QString               sample;
    QString               variant;
    QString               comparison_scope;
    QSize                 image_size;
    qreal                 nominal_raster_pixel_size = 0.0;
    qreal                 actual_raster_pixel_size = 0.0;
    int                   glyph_image_count        = 0;
    int                   valid_tile_count         = 0;
    QSize                 max_glyph_image_size;
    Pixel_diff_stats      diff_to_qt_text_layout;
    Pixel_image_ink_stats ink;
    int                   distinct_ink_rgb_colors = 0;
};

struct Raster_variant_probe_result
{
    QImage                                    sheet;
    std::vector<Raster_variant_probe_record> records;
};

struct Metrics_placement_font_metrics
{
    qreal m_advance          = 0.0;
    qreal average_char_width = 0.0;
    qreal ascent             = 0.0;
    qreal descent            = 0.0;
    qreal height             = 0.0;
    qreal line_spacing       = 0.0;
};

struct Metrics_placement_delta
{
    qreal raw_logical      = 0.0;
    qreal snapped_logical  = 0.0;
    qreal delta_logical    = 0.0;
    qreal delta_physical   = 0.0;
};

struct Metrics_placement_qt_glyph_record
{
    qsizetype source_string_start = 0;
    quint32   glyph_index         = 0U;
    QPointF   layout_origin;
};

struct Metrics_placement_glyph_record
{
    qsizetype source_string_start = 0;
    qsizetype source_string_end   = 0;
    quint32   glyph_index         = 0U;
    QPointF   qt_layout_origin;
    qreal     qt_advance          = 0.0;
    QPointF   terminal_cell_origin;
    QPointF   production_origin;
    qreal     terminal_minus_qt_x = 0.0;
    qreal     production_minus_terminal_x = 0.0;
    int       owner_column        = 0;
    int       owner_cell_span     = 1;
    bool      production_origin_matches_terminal = false;
    bool      terminal_origin_physical_snapped   = false;
    bool      production_origin_physical_snapped = false;
};

struct Metrics_placement_probe_result
{
    QString sample_text;
    qreal   device_pixel_ratio = 1.0;
    Metrics_placement_font_metrics natural_metrics;
    Metrics_placement_font_metrics cell_stable_metrics;
    term::terminal_cell_metrics_t  snapped_metrics;
    Metrics_placement_delta        width_delta;
    Metrics_placement_delta        ascent_delta;
    Metrics_placement_delta        descent_delta;
    Metrics_placement_delta        height_delta;
    Metrics_placement_delta        line_spacing_delta;
    qreal                          qtext_layout_natural_width = 0.0;
    qreal                          qfont_horizontal_advance   = 0.0;
    int                            terminal_cell_count        = 0;
    qreal                          terminal_run_width         = 0.0;
    qreal                          terminal_minus_qtext_width = 0.0;
    int                            matched_glyph_count        = 0;
    qreal                          max_abs_x_delta           = 0.0;
    qreal                          mean_abs_x_delta          = 0.0;
    qreal                          first_x_delta             = 0.0;
    qreal                          last_x_delta              = 0.0;
    qreal                          mean_qt_advance           = 0.0;
    qreal                          terminal_cell_advance     = 0.0;
    bool                           qtext_line_valid          = false;
    bool                           production_shape_valid    = false;
    std::vector<Metrics_placement_glyph_record>
                                   glyphs;
};

QString lcd_probe_printable_ascii_text()
{
    QString text;
    text.reserve(95);
    for (ushort code_unit = 0x20U; code_unit <= 0x7eU; ++code_unit) {
        text.append(QChar(code_unit));
    }
    return text;
}

QString lcd_probe_box_line_text()
{
    return QStringLiteral(
        "\u2500\u2501\u2502\u2503\u250c\u2510\u2514\u2518"
        "\u253c\u256d\u256e\u256f\u2570");
}

QString lcd_probe_braille_text()
{
    return QStringLiteral(
        "\u2801\u2803\u2807\u2817\u2837\u2877\u28ff");
}

QString lcd_probe_cjk_text()
{
    return QStringLiteral("\u4e16\u754c\u65e5\u672c\u8a9e\u6f22\u5b57");
}

QString lcd_probe_emoji_text()
{
    QString text = QString::fromUcs4(U"\U0001F600");
    text.append(QLatin1Char(' '));
    text.append(QString::fromUcs4(U"\U0001F9EA"));
    text.append(QLatin1Char(' '));
    text.append(QStringLiteral("\u2764\ufe0f"));
    return text;
}

std::vector<Lcd_probe_family> make_lcd_probe_families()
{
    return {
        {QStringLiteral("process"), QStringLiteral("Process:")},
        {QStringLiteral("dense_ascii"), lcd_probe_printable_ascii_text()},
        {QStringLiteral("box_line"), lcd_probe_box_line_text()},
        {QStringLiteral("braille"), lcd_probe_braille_text()},
        {QStringLiteral("cjk"), lcd_probe_cjk_text()},
        {QStringLiteral("combining_acute"), QStringLiteral("e\u0301")},
        {QStringLiteral("emoji_color"),
            lcd_probe_emoji_text(),
            term::Glyph_image_presentation::COLOR},
        {QStringLiteral("cursor_selection"),
            QStringLiteral("Cursor selection variants")},
    };
}

QString lcd_probe_image_format_name(QImage::Format format)
{
    switch (format) {
        case QImage::Format_Invalid:
            return QStringLiteral("Format_Invalid");
        case QImage::Format_Mono:
            return QStringLiteral("Format_Mono");
        case QImage::Format_MonoLSB:
            return QStringLiteral("Format_MonoLSB");
        case QImage::Format_Indexed8:
            return QStringLiteral("Format_Indexed8");
        case QImage::Format_RGB32:
            return QStringLiteral("Format_RGB32");
        case QImage::Format_ARGB32:
            return QStringLiteral("Format_ARGB32");
        case QImage::Format_ARGB32_Premultiplied:
            return QStringLiteral("Format_ARGB32_Premultiplied");
        case QImage::Format_RGB888:
            return QStringLiteral("Format_RGB888");
        case QImage::Format_RGBX8888:
            return QStringLiteral("Format_RGBX8888");
        case QImage::Format_RGBA8888:
            return QStringLiteral("Format_RGBA8888");
        case QImage::Format_RGBA8888_Premultiplied:
            return QStringLiteral("Format_RGBA8888_Premultiplied");
        case QImage::Format_BGR888:
            return QStringLiteral("Format_BGR888");
        case QImage::Format_Alpha8:
            return QStringLiteral("Format_Alpha8");
        case QImage::Format_Grayscale8:
            return QStringLiteral("Format_Grayscale8");
        default:
            return QStringLiteral("Format_%1")
                .arg(static_cast<int>(format));
    }
}

void append_lcd_probe_cell(
    term::Terminal_render_snapshot& snapshot,
    int                             row,
    int                             column,
    QString                         text,
    int                             display_width,
    term::Terminal_style_id         style_id,
    bool                            add_wide_continuations = false)
{
    snapshot.cells.push_back(
        make_pixel_cell(row, column, std::move(text), display_width, style_id));
    if (!add_wide_continuations) {
        return;
    }

    for (int offset = 1; offset < display_width; ++offset) {
        snapshot.cells.push_back(
            make_pixel_continuation_cell(row, column + offset, style_id));
    }
}

Pixel_parity_fixture make_lcd_capability_probe_fixture(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_capability_probe",
        false,
        {10, 104},
        980U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    const term::Terminal_style_id accent = 2U;
    const term::Terminal_style_id warm   = 3U;
    const term::Terminal_style_id inverse = 4U;
    fixture.snapshot.styles.push_back(rgb_style(0xffe6edf3U, 0xff101820U));
    fixture.snapshot.styles.push_back(rgb_style(0xff8be9fdU, 0xff101820U));
    fixture.snapshot.styles.push_back(rgb_style(0xffffd166U, 0xff101820U));
    fixture.snapshot.styles.push_back(rgb_style(
        0xff101820U,
        0xffe6edf3U,
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::INVERSE)));

    append_lcd_probe_cell(
        fixture.snapshot,
        0,
        0,
        QStringLiteral("Process: lcd-subpixel glyph atlas probe"),
        40,
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        1,
        0,
        QStringLiteral("ASCII:"),
        6,
        accent);
    append_lcd_probe_cell(
        fixture.snapshot,
        1,
        8,
        lcd_probe_printable_ascii_text(),
        95,
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        2,
        0,
        QStringLiteral("Box:"),
        4,
        accent);
    append_lcd_probe_cell(
        fixture.snapshot,
        2,
        6,
        lcd_probe_box_line_text(),
        lcd_probe_box_line_text().size(),
        warm);
    append_lcd_probe_cell(
        fixture.snapshot,
        3,
        0,
        QStringLiteral("Braille:"),
        8,
        accent);
    append_lcd_probe_cell(
        fixture.snapshot,
        3,
        10,
        lcd_probe_braille_text(),
        lcd_probe_braille_text().size(),
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        4,
        0,
        QStringLiteral("CJK:"),
        4,
        accent);
    int cjk_column = 6;
    for (const QChar character : lcd_probe_cjk_text()) {
        append_lcd_probe_cell(
            fixture.snapshot,
            4,
            cjk_column,
            QString(character),
            2,
            normal,
            true);
        cjk_column += 2;
    }
    append_lcd_probe_cell(
        fixture.snapshot,
        5,
        0,
        QStringLiteral("Combining:"),
        10,
        accent);
    append_lcd_probe_cell(
        fixture.snapshot,
        5,
        12,
        QStringLiteral("e\u0301"),
        1,
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        6,
        0,
        QStringLiteral("Emoji:"),
        6,
        accent);
    int emoji_column = 8;
    for (const QString& emoji : {
            QString::fromUcs4(U"\U0001F600"),
            QString::fromUcs4(U"\U0001F9EA"),
            QStringLiteral("\u2764\ufe0f"),
        })
    {
        append_lcd_probe_cell(
            fixture.snapshot,
            6,
            emoji_column,
            emoji,
            2,
            warm,
            true);
        emoji_column += 3;
    }
    append_lcd_probe_cell(
        fixture.snapshot,
        7,
        0,
        QStringLiteral("Selection: normal region"),
        24,
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        8,
        0,
        QStringLiteral("Selection inverse"),
        17,
        inverse);
    append_lcd_probe_cell(
        fixture.snapshot,
        9,
        0,
        QStringLiteral("Cursor:"),
        7,
        accent);
    append_lcd_probe_cell(
        fixture.snapshot,
        9,
        8,
        QStringLiteral("C"),
        1,
        normal);
    append_lcd_probe_cell(
        fixture.snapshot,
        9,
        10,
        QStringLiteral("block text variant"),
        18,
        normal);

    fixture.snapshot.selection_spans.push_back({
        {{7, 11}, {7, 24}, term::Terminal_selection_mode::NORMAL},
        7,
        11,
        13,
    });
    fixture.snapshot.selection_spans.push_back({
        {{8, 0}, {8, 17}, term::Terminal_selection_mode::NORMAL},
        8,
        0,
        17,
    });
    fixture.snapshot.cursor.position      = {9, 8};
    fixture.snapshot.cursor.visible       = true;
    fixture.snapshot.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    fixture.snapshot.cursor.blink_enabled = false;

    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    append_text_glyph_masks(fixture, frame.text_runs);
    append_text_glyph_masks(
        fixture,
        frame.cursor_text_runs,
        pixel_render_options().cursor_color);
    return fixture;
}

Pixel_parity_fixture make_lcd_single_w_probe_fixture(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_single_w_probe",
        false,
        {3, 3},
        986U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    fixture.snapshot.styles.push_back(rgb_style(
        k_lcd_w_probe_foreground_rgba,
        k_lcd_w_probe_background_rgba));
    fixture.snapshot.cells.push_back(
        make_pixel_cell(1, 1, QStringLiteral("W"), 1, normal));
    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_repeated_w_probe_fixture(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_repeated_w_probe",
        false,
        {3, 18},
        988U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    fixture.snapshot.styles.push_back(rgb_style(
        k_lcd_w_probe_foreground_rgba,
        k_lcd_w_probe_background_rgba));
    for (int index = 0; index < k_lcd_repeated_w_cell_count; ++index) {
        fixture.snapshot.cells.push_back(
            make_pixel_cell(
                k_lcd_repeated_w_row,
                k_lcd_repeated_w_first_column +
                    index * k_lcd_repeated_w_column_stride,
                QStringLiteral("W"),
                1,
                normal));
    }
    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_contiguous_x_probe_fixture(
    qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics =
        pixel_metrics(dpr, k_lcd_contiguous_x_font_size);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_contiguous_x_probe",
        false,
        {3, k_lcd_contiguous_x_first_column +
            k_lcd_contiguous_x_cell_count + 1},
        992U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    fixture.snapshot.styles.push_back(rgb_style(
        k_lcd_w_probe_foreground_rgba,
        k_lcd_w_probe_background_rgba));
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_contiguous_x_row,
            k_lcd_contiguous_x_first_column,
            QString(
                k_lcd_contiguous_x_cell_count,
                QLatin1Char('X')),
            k_lcd_contiguous_x_cell_count,
            normal));
    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_intensity_x_probe_fixture(
    qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics =
        pixel_metrics(dpr, k_lcd_intensity_x_font_size);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_intensity_x_probe",
        false,
        {k_lcd_intensity_x_first_row + k_lcd_intensity_x_rows + 1,
            k_lcd_intensity_x_first_column + k_lcd_intensity_x_columns + 1},
        994U,
        metrics,
        dpr);
    for (int row_index = 0; row_index < k_lcd_intensity_x_rows; ++row_index) {
        const term::Terminal_style_id style_id =
            static_cast<term::Terminal_style_id>(row_index + 1);
        fixture.snapshot.styles.push_back(rgb_style(
            k_lcd_intensity_x_foreground_rgba[
                static_cast<std::size_t>(row_index)],
            k_lcd_w_probe_background_rgba));
        fixture.snapshot.cells.push_back(
            make_pixel_cell(
                k_lcd_intensity_x_first_row + row_index,
                k_lcd_intensity_x_first_column,
                QString(
                    k_lcd_intensity_x_columns,
                    QLatin1Char(lcd_intensity_x_row_glyph(row_index))),
                k_lcd_intensity_x_columns,
                style_id));
    }
    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_fragmented_x_probe_fixture(
    qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(
        dpr,
        k_lcd_fragmented_x_font_size,
        QStringLiteral("monospace"));

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_fragmented_x_probe",
        false,
        {3, k_lcd_fragmented_x_first_column +
            k_lcd_fragmented_x_columns + 1},
        995U,
        metrics,
        dpr);
    fixture.snapshot.styles.reserve(
        static_cast<std::size_t>(k_lcd_fragmented_x_columns + 1));
    fixture.snapshot.hyperlinks.reserve(
        static_cast<std::size_t>(k_lcd_fragmented_x_columns));
    fixture.snapshot.cells.reserve(
        static_cast<std::size_t>(k_lcd_fragmented_x_columns));

    for (int column_index = 0; column_index < k_lcd_fragmented_x_columns;
        ++column_index)
    {
        const term::Terminal_style_id style_id =
            static_cast<term::Terminal_style_id>(
                fixture.snapshot.styles.size());
        const quint32 foreground =
            k_lcd_fragmented_x_foreground_rgba[
                static_cast<std::size_t>(column_index) %
                    k_lcd_fragmented_x_foreground_rgba.size()];
        fixture.snapshot.styles.push_back(rgb_style(
            foreground,
            k_lcd_w_probe_background_rgba));

        const term::Terminal_hyperlink_id hyperlink_id =
            static_cast<term::Terminal_hyperlink_id>(1000U + column_index);
        term::Terminal_render_hyperlink_metadata hyperlink;
        hyperlink.hyperlink_id = hyperlink_id;
        hyperlink.identity_key =
            QByteArrayLiteral("fragmented-x-") +
            QByteArray::number(static_cast<qlonglong>(column_index));
        hyperlink.uri =
            QByteArrayLiteral("vnm-terminal://fragmented-x/") +
            QByteArray::number(static_cast<qlonglong>(column_index));
        fixture.snapshot.hyperlinks.push_back(std::move(hyperlink));

        term::Terminal_render_cell cell = make_pixel_cell(
            k_lcd_fragmented_x_row,
            k_lcd_fragmented_x_first_column + column_index,
            QStringLiteral("X"),
            1,
            style_id);
        cell.hyperlink_id = hyperlink_id;
        fixture.snapshot.cells.push_back(std::move(cell));
    }

    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_repeated_on_probe_fixture(
    qreal device_pixel_ratio,
    qreal font_size = 18.0)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr, font_size);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_repeated_on_probe",
        false,
        {3, k_lcd_repeated_on_second_column + 4},
        990U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    fixture.snapshot.styles.push_back(rgb_style(
        k_lcd_w_probe_foreground_rgba,
        k_lcd_w_probe_background_rgba));
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_repeated_on_row,
            k_lcd_repeated_on_first_column,
            QStringLiteral("on"),
            2,
            normal));
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_repeated_on_row,
            k_lcd_repeated_on_second_column,
            QStringLiteral("on"),
            2,
            normal));
    append_text_glyph_masks(
        fixture,
        pixel_expected_frame(fixture).text_runs);
    return fixture;
}

Pixel_parity_fixture make_lcd_ascii_panel_probe_fixture(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);

    Pixel_parity_fixture fixture = make_pixel_parity_base_fixture(
        "lcd_ascii_panel_probe",
        false,
        {6, 42},
        989U,
        metrics,
        dpr);
    const term::Terminal_style_id normal = 1U;
    fixture.snapshot.styles.push_back(rgb_style(
        k_lcd_w_probe_foreground_rgba,
        k_lcd_w_probe_background_rgba));

    const QString representative = lcd_ascii_panel_representative_text();
    for (int index = 0; index < representative.size(); ++index) {
        fixture.snapshot.cells.push_back(
            make_pixel_cell(
                k_lcd_ascii_panel_representative_row,
                k_lcd_ascii_panel_representative_column +
                    index * k_lcd_ascii_panel_representative_column_stride,
                QString(representative.at(index)),
                1,
                normal));
    }

    const QString dense = lcd_ascii_panel_dense_text();
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_ascii_panel_dense_row,
            k_lcd_ascii_panel_dense_column,
            dense,
            dense.size(),
            normal));
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_ascii_panel_cursor_row,
            k_lcd_ascii_panel_cursor_column,
            lcd_ascii_panel_cursor_text(),
            1,
            normal));
    const QString user_text = lcd_ascii_panel_user_text();
    fixture.snapshot.cells.push_back(
        make_pixel_cell(
            k_lcd_ascii_panel_user_text_row,
            k_lcd_ascii_panel_user_text_column,
            user_text,
            user_text.size(),
            normal));
    fixture.snapshot.cursor.position = {
        k_lcd_ascii_panel_cursor_row,
        k_lcd_ascii_panel_cursor_column,
    };
    fixture.snapshot.cursor.visible = true;
    fixture.snapshot.cursor.shape = term::Terminal_cursor_shape::BLOCK;
    fixture.snapshot.cursor.blink_enabled = false;

    const term::Terminal_render_frame frame = pixel_expected_frame(fixture);
    append_text_glyph_masks(fixture, frame.text_runs);
    append_text_glyph_masks(
        fixture,
        frame.cursor_text_runs,
        pixel_render_options().cursor_color);
    return fixture;
}

std::vector<Lcd_glyph_probe_record> probe_lcd_subpixel_glyphs(
    const QFont& font,
    qreal        device_pixel_ratio)
{
    std::vector<Lcd_glyph_probe_record> records;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const std::vector<Lcd_probe_family> families = make_lcd_probe_families();

    for (const Lcd_probe_family& family : families) {
        const QString presentation_name = QString::fromLatin1(
            term::qsg_atlas_glyph_image_presentation_name(family.presentation));
        QTextLayout layout(family.text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.setCacheEnabled(false);

        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            layout.endLayout();
            records.push_back({
                family.label,
                QString(),
                QString(),
                0U,
                0,
                0,
                static_cast<int>(QImage::Format_Invalid),
                lcd_probe_image_format_name(QImage::Format_Invalid),
                QSize(),
                presentation_name,
                QStringLiteral("missing_text_layout_line"),
                false,
            });
            continue;
        }
        line.setLineWidth(1024.0 * 1024.0);
        line.setPosition(QPointF(0.0, 0.0));
        const QList<QGlyphRun> glyph_runs = line.glyphRuns(
            0,
            family.text.size(),
            QTextLayout::RetrieveGlyphIndexes |
                QTextLayout::RetrieveGlyphPositions);
        layout.endLayout();

        bool family_recorded = false;
        for (int run_index = 0; run_index < glyph_runs.size(); ++run_index) {
            const QGlyphRun& glyph_run = glyph_runs.at(run_index);
            const QList<quint32> glyph_indexes = glyph_run.glyphIndexes();
            QRawFont raw_font = glyph_run.rawFont();
            if (!raw_font.isValid()) {
                records.push_back({
                    family.label,
                    QString(),
                    QString(),
                    0U,
                    run_index,
                    0,
                    static_cast<int>(QImage::Format_Invalid),
                    lcd_probe_image_format_name(QImage::Format_Invalid),
                    QSize(),
                    presentation_name,
                    QStringLiteral("invalid_raw_font"),
                    false,
                });
                family_recorded = true;
                continue;
            }

            QRawFont raster_font = raw_font;
            raster_font.setPixelSize(
                term::qsg_atlas_physical_pixel_size(raw_font, dpr));
            for (int glyph_offset = 0;
                glyph_offset < glyph_indexes.size();
                ++glyph_offset)
            {
                const quint32 glyph_index = glyph_indexes.at(glyph_offset);
                const QRawFont::AntialiasingType antialiasing =
                    family.presentation == term::Glyph_image_presentation::TEXT
                        ? QRawFont::SubPixelAntialiasing
                        : QRawFont::PixelAntialiasing;
                const QImage glyph_image = glyph_index != 0U
                    ? raster_font.alphaMapForGlyph(
                        glyph_index,
                        antialiasing)
                    : QImage();
                const term::Glyph_rgba_tile tile =
                    term::qsg_atlas_rgba_tile_from_image(
                        glyph_image,
                        family.presentation);
                records.push_back({
                    family.label,
                    raw_font.familyName(),
                    raw_font.styleName(),
                    glyph_index,
                    run_index,
                    glyph_offset,
                    static_cast<int>(glyph_image.format()),
                    lcd_probe_image_format_name(glyph_image.format()),
                    glyph_image.size(),
                    presentation_name,
                    QString::fromLatin1(
                        term::qsg_atlas_glyph_coverage_kind_name(
                            term::qsg_atlas_classify_glyph_image_candidate(
                                glyph_image,
                                family.presentation))),
                    tile.is_valid(),
                });
                family_recorded = true;
            }
        }

        if (!family_recorded) {
            records.push_back({
                family.label,
                QString(),
                QString(),
                0U,
                0,
                0,
                static_cast<int>(QImage::Format_Invalid),
                lcd_probe_image_format_name(QImage::Format_Invalid),
                QSize(),
                presentation_name,
                QString::fromLatin1(
                    term::qsg_atlas_glyph_coverage_kind_name(
                        term::Glyph_coverage_kind::UNKNOWN)),
                false,
            });
        }
    }

    return records;
}

QString raster_variant_kind_name(Raster_variant_kind kind)
{
    switch (kind) {
        case Raster_variant_kind::ATLAS_CURRENT_PHYSICAL:
            return QStringLiteral("atlas_current_physical");
        case Raster_variant_kind::ATLAS_FLOOR_PHYSICAL:
            return QStringLiteral("atlas_floor_physical");
        case Raster_variant_kind::ATLAS_ROUND_PHYSICAL:
            return QStringLiteral("atlas_round_physical");
        case Raster_variant_kind::ATLAS_CEIL_PHYSICAL:
            return QStringLiteral("atlas_ceil_physical");
        case Raster_variant_kind::QT_TEXT_LAYOUT:
            return QStringLiteral("qt_text_layout");
    }

    return QStringLiteral("unknown");
}

std::vector<QString> raster_variant_probe_samples()
{
    return {
        QStringLiteral("e"),
        QStringLiteral("W"),
        QStringLiteral("OpenAI Codex (v0.137.0)"),
    };
}

std::vector<Raster_variant_kind> raster_variant_probe_kinds()
{
    return {
        Raster_variant_kind::QT_TEXT_LAYOUT,
        Raster_variant_kind::ATLAS_CURRENT_PHYSICAL,
        Raster_variant_kind::ATLAS_FLOOR_PHYSICAL,
        Raster_variant_kind::ATLAS_ROUND_PHYSICAL,
        Raster_variant_kind::ATLAS_CEIL_PHYSICAL,
    };
}

qreal raster_variant_pixel_size(
    qreal               physical_pixel_size,
    Raster_variant_kind kind)
{
    const qreal safe_size = std::isfinite(physical_pixel_size)
        ? std::max<qreal>(1.0, physical_pixel_size)
        : 1.0;
    switch (kind) {
        case Raster_variant_kind::ATLAS_FLOOR_PHYSICAL:
            return std::max<qreal>(1.0, std::floor(safe_size));
        case Raster_variant_kind::ATLAS_ROUND_PHYSICAL:
            return std::max<qreal>(1.0, std::round(safe_size));
        case Raster_variant_kind::ATLAS_CEIL_PHYSICAL:
            return std::max<qreal>(1.0, std::ceil(safe_size));
        case Raster_variant_kind::ATLAS_CURRENT_PHYSICAL:
        case Raster_variant_kind::QT_TEXT_LAYOUT:
            break;
    }
    return safe_size;
}

int raster_variant_text_cells(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return std::max(
        1,
        term::measure_utf8_width(
            QByteArrayView(utf8.constData(), utf8.size())).cells);
}

QString raster_variant_comparison_scope(const QString& text)
{
    return raster_variant_text_cells(text) == 1
        ? QStringLiteral("glyph-local")
        : QStringLiteral("layout-sensitive-terminal-cell-run");
}

QSizeF raster_variant_sample_logical_size(
    const QString&                 text,
    term::terminal_cell_metrics_t  metrics)
{
    constexpr qreal k_padding = 8.0;
    const int cells = std::max(12, raster_variant_text_cells(text) + 2);
    return QSizeF(
        k_padding * 2.0 + metrics.width  * static_cast<qreal>(cells),
        k_padding * 2.0 + metrics.height);
}

term::Terminal_render_text_run raster_variant_sample_run(
    const QString&                 text,
    term::terminal_cell_metrics_t  metrics,
    QColor                         foreground,
    QColor                         background)
{
    constexpr qreal k_padding = 8.0;
    const int cells = std::max(1, raster_variant_text_cells(text));
    const QRectF rect(
        k_padding,
        k_padding,
        metrics.width * static_cast<qreal>(cells),
        metrics.height);

    term::Terminal_render_text_run run;
    run.text            = text;
    run.rect            = rect;
    run.baseline_origin = QPointF(rect.left(), rect.top() + metrics.ascent);
    run.foreground      = foreground;
    run.background      = background;
    return run;
}

QRectF raster_variant_probe_region(
    const term::Terminal_render_text_run& run,
    term::terminal_cell_metrics_t         metrics)
{
    return run.rect.adjusted(
        -metrics.width,
        -metrics.height * 0.5,
        metrics.width,
        metrics.height * 0.5);
}

QImage make_raster_variant_base_image(QSizeF logical_size, qreal dpr, QColor background)
{
    QImage image(
        pixel_window_physical_pixel_size(logical_size, dpr),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(pixel_normalized_device_pixel_ratio(dpr));
    image.fill(background);
    return image;
}

Raster_variant_glyph_image_stats raster_variant_glyph_image_stats(
    const QString&                 text,
    const QFont&                   font,
    term::terminal_cell_metrics_t  metrics,
    qreal                          device_pixel_ratio,
    Raster_variant_kind            kind,
    QColor                         foreground,
    QColor                         background)
{
    Raster_variant_glyph_image_stats stats;
    if (kind == Raster_variant_kind::QT_TEXT_LAYOUT) {
        return stats;
    }

    const term::Terminal_render_text_run run =
        raster_variant_sample_run(text, metrics, foreground, background);
    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(
            run,
            font,
            metrics,
            device_pixel_ratio,
            0,
            false);
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.glyph_index == 0U) {
            continue;
        }

        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(
            raster_variant_pixel_size(record.physical_pixel_size, kind));
        if (stats.actual_raster_pixel_size <= 0.0) {
            stats.actual_raster_pixel_size = raster_font.pixelSize();
        }

        const term::Glyph_image_presentation presentation =
            atlas_reference_presentation_for_source_range(
                run.text,
                record.source_string_start,
                record.source_string_end);
        const QRawFont::AntialiasingType antialiasing =
            presentation == term::Glyph_image_presentation::TEXT
                ? QRawFont::SubPixelAntialiasing
                : QRawFont::PixelAntialiasing;
        const QImage glyph_image = raster_font.alphaMapForGlyph(
            record.glyph_index,
            antialiasing);
        if (glyph_image.isNull()) {
            continue;
        }

        ++stats.glyph_image_count;
        stats.max_glyph_image_size = QSize(
            std::max(stats.max_glyph_image_size.width(), glyph_image.width()),
            std::max(stats.max_glyph_image_size.height(), glyph_image.height()));
        const term::Glyph_rgba_tile tile =
            term::qsg_atlas_rgba_tile_from_image(glyph_image, presentation);
        if (tile.is_valid()) {
            ++stats.valid_tile_count;
        }
    }
    return stats;
}

QImage render_atlas_raster_variant_sample(
    const QString&                 text,
    const QFont&                   font,
    term::terminal_cell_metrics_t  metrics,
    qreal                          device_pixel_ratio,
    Raster_variant_kind            kind,
    QColor                         foreground,
    QColor                         background)
{
    const QSizeF logical_size = raster_variant_sample_logical_size(text, metrics);
    const term::Terminal_render_text_run run =
        raster_variant_sample_run(text, metrics, foreground, background);
    if (kind == Raster_variant_kind::QT_TEXT_LAYOUT) {
        return render_qpainter_text_layout_image(
            run,
            font,
            logical_size,
            device_pixel_ratio);
    }

    QImage image =
        make_raster_variant_base_image(logical_size, device_pixel_ratio, background);
    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(
            run,
            font,
            metrics,
            device_pixel_ratio,
            0,
            false);
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        QRawFont raster_font = record.raw_font;
        raster_font.setPixelSize(
            raster_variant_pixel_size(record.physical_pixel_size, kind));
        paint_atlas_rgba_reference_glyph(
            image,
            record,
            raster_font,
            run,
            device_pixel_ratio);
    }
    return image;
}

int count_distinct_ink_rgb_colors(
    const QImage& image,
    QColor        background,
    QRectF        logical_region,
    qreal         device_pixel_ratio)
{
    constexpr int k_ink_delta_threshold = 8;

    std::set<QRgb> colors;
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QRect region = logical_rect_to_pixels(logical_region, dpr)
        .intersected(image.rect());
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel_delta(pixel, background) <= k_ink_delta_threshold) {
                continue;
            }
            colors.insert(qRgb(pixel.red(), pixel.green(), pixel.blue()));
        }
    }
    return static_cast<int>(colors.size());
}

QImage raster_variant_sheet_image(
    const std::vector<std::pair<Raster_variant_probe_record, QImage>>& rows,
    QColor                                                            background)
{
    constexpr int k_padding     = 12;
    constexpr int k_label_width = 360;
    int sample_width  = 1;
    int sample_height = 1;
    for (const auto& row : rows) {
        sample_width  = std::max(sample_width,  row.second.width());
        sample_height = std::max(sample_height, row.second.height());
    }

    const int row_height = sample_height + k_padding;
    QImage sheet(
        QSize(
            k_label_width + sample_width + k_padding * 3,
            k_padding + row_height * static_cast<int>(rows.size())),
        QImage::Format_ARGB32_Premultiplied);
    sheet.fill(background);

    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor(220, 230, 238));
    for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
        const Raster_variant_probe_record& record = rows[static_cast<std::size_t>(index)].first;
        QImage image = rows[static_cast<std::size_t>(index)].second;
        image.setDevicePixelRatio(1.0);
        const int top = k_padding + index * row_height;
        painter.drawText(
            QRect(0, top, k_label_width, sample_height),
            Qt::AlignRight | Qt::AlignVCenter,
            record.sample + QStringLiteral(" / ") + record.variant);
        painter.drawImage(QPoint(k_label_width + k_padding, top), image);
    }
    return sheet;
}

Raster_variant_probe_result render_lcd_raster_variant_probe(qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    const QColor background(13, 24, 31);
    const QColor foreground(238, 244, 248);
    const std::vector<QString> samples = raster_variant_probe_samples();
    const std::vector<Raster_variant_kind> variants =
        raster_variant_probe_kinds();

    Raster_variant_probe_result result;
    std::vector<std::pair<Raster_variant_probe_record, QImage>> rows;
    const qreal nominal_physical_pixel_size =
        term::qsg_atlas_physical_pixel_size(font, dpr);
    for (const QString& sample : samples) {
        const term::Terminal_render_text_run run =
            raster_variant_sample_run(sample, metrics, foreground, background);
        const QRectF probe_region = raster_variant_probe_region(run, metrics);
        const QImage qt_reference =
            render_atlas_raster_variant_sample(
                sample,
                font,
                metrics,
                dpr,
                Raster_variant_kind::QT_TEXT_LAYOUT,
                foreground,
                background);
        for (const Raster_variant_kind variant : variants) {
            const QImage image = render_atlas_raster_variant_sample(
                sample,
                font,
                metrics,
                dpr,
                variant,
                foreground,
                background);
            Raster_variant_probe_record record;
            record.sample = sample;
            record.variant = raster_variant_kind_name(variant);
            record.comparison_scope = raster_variant_comparison_scope(sample);
            record.image_size = image.size();
            record.nominal_raster_pixel_size =
                raster_variant_pixel_size(nominal_physical_pixel_size, variant);
            const Raster_variant_glyph_image_stats glyph_stats =
                raster_variant_glyph_image_stats(
                    sample,
                    font,
                    metrics,
                    dpr,
                    variant,
                    foreground,
                    background);
            record.actual_raster_pixel_size =
                glyph_stats.actual_raster_pixel_size;
            record.glyph_image_count    = glyph_stats.glyph_image_count;
            record.valid_tile_count     = glyph_stats.valid_tile_count;
            record.max_glyph_image_size = glyph_stats.max_glyph_image_size;
            record.diff_to_qt_text_layout =
                compare_regions(image, qt_reference, {probe_region}, dpr);
            record.ink = measure_image_ink(image, background, probe_region, dpr);
            record.distinct_ink_rgb_colors =
                count_distinct_ink_rgb_colors(
                    image,
                    background,
                    probe_region,
                    dpr);
            result.records.push_back(record);
            rows.push_back({std::move(record), image});
        }
    }
    result.sheet = raster_variant_sheet_image(rows, background);
    return result;
}

bool raster_variant_probe_record_is_usable(
    const Raster_variant_probe_record& record)
{
    if (record.image_size.isEmpty() ||
        record.diff_to_qt_text_layout.compared_pixels <= 0 ||
        record.ink.ink_pixels <= 0 ||
        record.distinct_ink_rgb_colors <= 0)
    {
        return false;
    }

    if (record.variant == raster_variant_kind_name(
            Raster_variant_kind::QT_TEXT_LAYOUT))
    {
        return true;
    }

    return
        record.actual_raster_pixel_size > 0.0 &&
        record.glyph_image_count > 0          &&
        record.valid_tile_count > 0           &&
        !record.max_glyph_image_size.isEmpty();
}

bool raster_variant_probe_records_are_usable(
    const Raster_variant_probe_result& result)
{
    const std::size_t expected_records =
        raster_variant_probe_samples().size() *
        raster_variant_probe_kinds().size();
    if (result.records.size() != expected_records) {
        return false;
    }

    for (const Raster_variant_probe_record& record : result.records) {
        if (!raster_variant_probe_record_is_usable(record)) {
            return false;
        }
    }
    return true;
}

QString lcd_metrics_placement_probe_text()
{
    return QStringLiteral("OpenAI Codex (v0.137.0)");
}

Metrics_placement_font_metrics metrics_placement_font_metrics(const QFont& font)
{
    const QFontMetricsF metrics(font);
    return {
        metrics.horizontalAdvance(QLatin1Char('M')),
        metrics.averageCharWidth(),
        metrics.ascent(),
        metrics.descent(),
        metrics.height(),
        metrics.lineSpacing(),
    };
}

Metrics_placement_delta metrics_placement_delta(
    qreal raw_logical,
    qreal snapped_logical,
    qreal device_pixel_ratio)
{
    return {
        raw_logical,
        snapped_logical,
        snapped_logical - raw_logical,
        (snapped_logical - raw_logical) *
            pixel_normalized_device_pixel_ratio(device_pixel_ratio),
    };
}

bool metrics_placement_point_is_finite(QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

std::vector<Metrics_placement_qt_glyph_record>
metrics_placement_qt_glyph_records(
    const QList<QGlyphRun>& glyph_runs,
    QPointF                 layout_origin,
    qsizetype               text_size)
{
    std::vector<Metrics_placement_qt_glyph_record> records;
    for (const QGlyphRun& glyph_run : glyph_runs) {
        const QList<quint32>   glyph_indexes  = glyph_run.glyphIndexes();
        const QList<QPointF>   positions      = glyph_run.positions();
        const QList<qsizetype> string_indexes = glyph_run.stringIndexes();
        const int glyph_count = std::min({
            glyph_indexes.size(),
            positions.size(),
            string_indexes.size(),
        });
        for (int index = 0; index < glyph_count; ++index) {
            const qsizetype source_start = string_indexes.at(index);
            if (source_start < 0 || source_start >= text_size) {
                continue;
            }

            records.push_back({
                source_start,
                glyph_indexes.at(index),
                layout_origin + positions.at(index),
            });
        }
    }

    std::sort(
        records.begin(),
        records.end(),
        [](const auto& left, const auto& right) {
            if (left.source_string_start != right.source_string_start) {
                return left.source_string_start < right.source_string_start;
            }
            if (left.layout_origin.x() != right.layout_origin.x()) {
                return left.layout_origin.x() < right.layout_origin.x();
            }
            return left.glyph_index < right.glyph_index;
        });

    return records;
}

qsizetype metrics_placement_source_range_end(
    const std::vector<Metrics_placement_qt_glyph_record>& records,
    std::size_t                                           glyph_offset,
    qsizetype                                             source_start,
    qsizetype                                             text_size)
{
    for (std::size_t next = glyph_offset + 1; next < records.size(); ++next) {
        const qsizetype next_index = records.at(next).source_string_start;
        if (next_index > source_start && next_index <= text_size) {
            return next_index;
        }
    }
    return text_size;
}

const term::Qsg_atlas_shaped_glyph_record*
metrics_placement_find_shaped_record(
    const term::Qsg_atlas_shaped_text_run_result& shaped,
    qsizetype                                     source_start)
{
    for (const term::Qsg_atlas_shaped_glyph_record& record : shaped.records) {
        if (record.source_string_start == source_start) {
            return &record;
        }
    }
    return nullptr;
}

Metrics_placement_probe_result render_lcd_metrics_placement_probe(
    qreal device_pixel_ratio)
{
    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const QString text = lcd_metrics_placement_probe_text();
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const QFont ascii_font = term::qsg_atlas_cell_stable_ascii_layout_font(font);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    const QColor background(13, 24, 31);
    const QColor foreground(238, 244, 248);
    const term::Terminal_render_text_run run =
        raster_variant_sample_run(text, metrics, foreground, background);

    Metrics_placement_probe_result result;
    result.sample_text        = text;
    result.device_pixel_ratio = dpr;
    result.natural_metrics    = metrics_placement_font_metrics(font);
    result.cell_stable_metrics = metrics_placement_font_metrics(ascii_font);
    result.snapped_metrics    = metrics;
    result.width_delta = metrics_placement_delta(
        result.natural_metrics.m_advance,
        metrics.width,
        dpr);
    result.ascent_delta = metrics_placement_delta(
        result.natural_metrics.ascent,
        metrics.ascent,
        dpr);
    result.descent_delta = metrics_placement_delta(
        result.natural_metrics.descent,
        metrics.descent,
        dpr);
    result.height_delta = metrics_placement_delta(
        result.natural_metrics.height,
        metrics.height,
        dpr);
    result.line_spacing_delta = metrics_placement_delta(
        result.natural_metrics.line_spacing,
        metrics.height,
        dpr);
    result.terminal_cell_count = raster_variant_text_cells(text);
    result.terminal_run_width =
        static_cast<qreal>(result.terminal_cell_count) * metrics.width;
    result.terminal_cell_advance = metrics.width;

    QTextLayout layout(text, font);
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
    line.setLineWidth(1024.0 * 1024.0);
    line.setPosition(QPointF(0.0, 0.0));
    result.qtext_line_valid = line.textStart() == 0 &&
        line.textLength() == text.size();
    result.qtext_layout_natural_width = line.naturalTextWidth();
    result.qfont_horizontal_advance =
        QFontMetricsF(font).horizontalAdvance(text);
    result.terminal_minus_qtext_width =
        result.terminal_run_width - result.qtext_layout_natural_width;
    const qreal line_ascent = line.ascent();
    const QList<QGlyphRun> glyph_runs = line.glyphRuns(
        0,
        text.size(),
        QTextLayout::RetrieveGlyphIndexes   |
            QTextLayout::RetrieveGlyphPositions |
            QTextLayout::RetrieveStringIndexes);
    layout.endLayout();

    const term::Qsg_atlas_shaped_text_run_result shaped =
        term::qsg_atlas_shape_text_run(
            run,
            ascii_font,
            metrics,
            dpr,
            0,
            false);
    result.production_shape_valid =
        shaped.missing_string_indexes == 0 &&
        shaped.invalid_string_indexes == 0;

    const QPointF layout_origin(
        run.baseline_origin.x(),
        run.baseline_origin.y() - line_ascent);
    const std::vector<Metrics_placement_qt_glyph_record> qt_glyphs =
        metrics_placement_qt_glyph_records(
            glyph_runs,
            layout_origin,
            text.size());
    double abs_x_delta_sum = 0.0;
    double qt_advance_sum  = 0.0;
    for (std::size_t index = 0; index < qt_glyphs.size(); ++index) {
        const Metrics_placement_qt_glyph_record& qt_glyph = qt_glyphs.at(index);
        const term::Qsg_atlas_shaped_glyph_record* const shaped_record =
            metrics_placement_find_shaped_record(
                shaped,
                qt_glyph.source_string_start);
        if (shaped_record == nullptr) {
            continue;
        }

        Metrics_placement_glyph_record record;
        record.source_string_start = qt_glyph.source_string_start;
        record.source_string_end =
            metrics_placement_source_range_end(
                qt_glyphs,
                index,
                qt_glyph.source_string_start,
                text.size());
        record.glyph_index = qt_glyph.glyph_index;
        record.qt_layout_origin = qt_glyph.layout_origin;
        const qreal next_origin_x =
            index + 1 < qt_glyphs.size()
                ? qt_glyphs.at(index + 1).layout_origin.x()
                : layout_origin.x() + result.qtext_layout_natural_width;
        record.qt_advance = next_origin_x - record.qt_layout_origin.x();
        record.production_origin = shaped_record->glyph_origin;
        record.terminal_cell_origin = QPointF(
            run.rect.left() +
                static_cast<qreal>(qt_glyph.source_string_start) * metrics.width,
            record.production_origin.y());
        record.terminal_minus_qt_x =
            record.terminal_cell_origin.x() - record.qt_layout_origin.x();
        record.production_minus_terminal_x =
            record.production_origin.x() -
                record.terminal_cell_origin.x();
        record.owner_column    = shaped_record->owner_column;
        record.owner_cell_span = shaped_record->owner_cell_span;
        record.production_origin_matches_terminal =
            std::abs(record.production_minus_terminal_x) <= 0.001;
        record.terminal_origin_physical_snapped =
            point_is_physical_pixel_snapped(record.terminal_cell_origin, dpr);
        record.production_origin_physical_snapped =
            point_is_physical_pixel_snapped(record.production_origin, dpr);

        result.max_abs_x_delta = std::max(
            result.max_abs_x_delta,
            std::abs(record.terminal_minus_qt_x));
        abs_x_delta_sum += std::abs(record.terminal_minus_qt_x);
        qt_advance_sum  += record.qt_advance;
        if (result.glyphs.empty()) {
            result.first_x_delta = record.terminal_minus_qt_x;
        }
        result.last_x_delta = record.terminal_minus_qt_x;
        result.glyphs.push_back(record);
    }

    result.matched_glyph_count = static_cast<int>(result.glyphs.size());
    if (result.matched_glyph_count > 0) {
        result.mean_abs_x_delta =
            abs_x_delta_sum / static_cast<double>(result.matched_glyph_count);
        result.mean_qt_advance =
            qt_advance_sum / static_cast<double>(result.matched_glyph_count);
    }
    return result;
}

bool metrics_placement_probe_is_usable(
    const Metrics_placement_probe_result& result)
{
    const auto finite_positive = [](qreal value) {
        return std::isfinite(value) && value > 0.0;
    };

    if (!result.qtext_line_valid ||
        !result.production_shape_valid ||
        result.terminal_cell_count <= 0 ||
        result.matched_glyph_count != result.terminal_cell_count ||
        result.glyphs.size() != static_cast<std::size_t>(result.terminal_cell_count) ||
        !finite_positive(result.natural_metrics.m_advance) ||
        !finite_positive(result.snapped_metrics.width) ||
        !finite_positive(result.qtext_layout_natural_width) ||
        !finite_positive(result.terminal_run_width))
    {
        return false;
    }

    qsizetype previous_source_start = -1;
    for (const Metrics_placement_glyph_record& record : result.glyphs) {
        if (record.source_string_start < 0 ||
            record.source_string_start <= previous_source_start ||
            record.source_string_end <= record.source_string_start ||
            record.source_string_end > result.sample_text.size() ||
            record.glyph_index == 0U ||
            !metrics_placement_point_is_finite(record.qt_layout_origin) ||
            !metrics_placement_point_is_finite(record.terminal_cell_origin) ||
            !metrics_placement_point_is_finite(record.production_origin) ||
            !std::isfinite(record.qt_advance) ||
            !std::isfinite(record.terminal_minus_qt_x) ||
            !std::isfinite(record.production_minus_terminal_x) ||
            !record.production_origin_matches_terminal ||
            !record.terminal_origin_physical_snapped ||
            !record.production_origin_physical_snapped)
        {
            return false;
        }
        previous_source_start = record.source_string_start;
    }
    return true;
}

void print_lcd_probe_records(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    for (const Lcd_glyph_probe_record& record : records) {
        const QByteArray family = record.family_label.toUtf8();
        const QByteArray font_family = record.font_family.toUtf8();
        const QByteArray font_style = record.font_style.toUtf8();
        const QByteArray format = record.image_format_name.toUtf8();
        const QByteArray presentation = record.requested_presentation.toUtf8();
        const QByteArray candidate = record.coverage_candidate.toUtf8();
        std::cout << "LCD glyph probe"
            << " family=" << family.constData()
            << " glyph_index=" << record.glyph_index
            << " glyph_run=" << record.glyph_run_index
            << " glyph_offset=" << record.glyph_index_in_run
            << " font_family=" << font_family.constData()
            << " font_style=" << font_style.constData()
            << " image_format=" << format.constData()
            << '(' << record.image_format_id << ')'
            << " image_size=" << record.image_size.width()
            << 'x' << record.image_size.height()
            << " requested_presentation=" << presentation.constData()
            << " coverage_candidate=" << candidate.constData()
            << " production_tile=" << record.production_tile_valid
            << '\n';
    }
}

bool lcd_probe_image_has_pixels(const QImage& image)
{
    return !image.isNull() && image.width() > 0 && image.height() > 0;
}

bool lcd_probe_image_has_visible_variation(const QImage& image)
{
    if (!lcd_probe_image_has_pixels(image)) {
        return false;
    }

    const QColor base = image.pixelColor(0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (pixel_delta(image.pixelColor(x, y), base) > 8) {
                return true;
            }
        }
    }
    return false;
}

bool lcd_probe_record_has_concrete_coverage(const Lcd_glyph_probe_record& record)
{
    return
        record.image_size.width()  > 0 &&
        record.image_size.height() > 0 &&
        record.coverage_candidate != QStringLiteral("unknown") &&
        record.coverage_candidate != QStringLiteral("missing_text_layout_line") &&
        record.coverage_candidate != QStringLiteral("invalid_raw_font");
}

bool lcd_probe_source_format_can_carry_color(QImage::Format format)
{
    switch (format) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
        case QImage::Format_RGB888:
        case QImage::Format_BGR888:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
        case QImage::Format_RGB30:
        case QImage::Format_BGR30:
        case QImage::Format_A2BGR30_Premultiplied:
        case QImage::Format_A2RGB30_Premultiplied:
        case QImage::Format_RGBA64:
        case QImage::Format_RGBA64_Premultiplied:
            return true;
        default:
            return false;
    }
}

bool lcd_probe_records_have_supported_classifier_results(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    return std::none_of(
        records.begin(),
        records.end(),
        [](const Lcd_glyph_probe_record& record) {
            return
                lcd_probe_record_has_concrete_coverage(record) &&
                (record.coverage_candidate == QStringLiteral("ambiguous") ||
                    record.coverage_candidate == QStringLiteral("unsupported"));
        });
}

bool lcd_probe_color_capable_records_keep_color_kind(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    return std::all_of(
        records.begin(),
        records.end(),
        [](const Lcd_glyph_probe_record& record) {
            if (record.requested_presentation != QStringLiteral("color")) {
                return true;
            }

            const QImage::Format source_format =
                static_cast<QImage::Format>(record.image_format_id);
            if (!lcd_probe_source_format_can_carry_color(source_format)) {
                return true;
            }

            return record.coverage_candidate == QStringLiteral("color_image");
        });
}

bool lcd_probe_has_valid_production_tile_for_family(
    const std::vector<Lcd_glyph_probe_record>& records,
    QStringView                                family_label)
{
    return std::any_of(
        records.begin(),
        records.end(),
        [&](const Lcd_glyph_probe_record& record) {
            return
                record.family_label == family_label &&
                record.production_tile_valid;
        });
}

std::vector<QString> missing_lcd_probe_family_coverage_labels(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    std::vector<QString> missing;
    for (const Lcd_probe_family& family : make_lcd_probe_families()) {
        const bool family_has_coverage = std::any_of(
            records.begin(),
            records.end(),
            [&family](const Lcd_glyph_probe_record& record) {
                return
                    record.family_label == family.label &&
                    lcd_probe_record_has_concrete_coverage(record);
            });
        if (!family_has_coverage) {
            missing.push_back(family.label);
        }
    }
    return missing;
}

bool lcd_probe_records_cover_required_families(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    return missing_lcd_probe_family_coverage_labels(records).empty();
}

void print_missing_lcd_probe_family_coverage_labels(
    const std::vector<Lcd_glyph_probe_record>& records)
{
    const std::vector<QString> missing =
        missing_lcd_probe_family_coverage_labels(records);
    for (const QString& label : missing) {
        const QByteArray label_bytes = label.toUtf8();
        std::cerr << "FAIL: LCD atlas probe missing concrete coverage for family "
            << label_bytes.constData() << '\n';
    }
}

void json_insert_u64(QJsonObject& object, const QString& name, std::uint64_t value)
{
    object.insert(name, static_cast<qint64>(value));
}

QJsonObject json_size_object(QSize size)
{
    QJsonObject object;
    object.insert(QStringLiteral("width"), size.width());
    object.insert(QStringLiteral("height"), size.height());
    return object;
}

QJsonObject json_sizef_object(QSizeF size)
{
    QJsonObject object;
    object.insert(QStringLiteral("width"), size.width());
    object.insert(QStringLiteral("height"), size.height());
    return object;
}

QJsonArray json_double_array(const std::vector<double>& values)
{
    QJsonArray array;
    for (double value : values) {
        array.append(value);
    }
    return array;
}

QJsonObject lcd_probe_glyph_stats_object(const Pixel_glyph_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("compared_pixels"), stats.compared_pixels);
    object.insert(QStringLiteral("diff_pixels"), stats.diff_pixels);
    object.insert(QStringLiteral("max_delta"), stats.max_delta);
    object.insert(
        QStringLiteral("reference_ink_pixels"),
        stats.reference_ink_pixels);
    object.insert(QStringLiteral("atlas_ink_pixels"), stats.atlas_ink_pixels);
    object.insert(QStringLiteral("perimeter_pixels"), stats.perimeter_pixels);
    object.insert(
        QStringLiteral("mask_count"),
        static_cast<int>(stats.masks.size()));
    return object;
}

QJsonObject lcd_probe_render_result_object(const Pixel_render_result& result)
{
    QJsonObject object;
    object.insert(QStringLiteral("ready"), result.ready);
    object.insert(
        QStringLiteral("image_size"),
        json_size_object(result.image.size()));
    object.insert(
        QStringLiteral("window_logical_size"),
        json_size_object(result.window_logical_size));
    object.insert(
        QStringLiteral("device_pixel_ratio"),
        result.device_pixel_ratio);
    object.insert(
        QStringLiteral("image_device_pixel_ratio"),
        result.image_device_pixel_ratio);
    object.insert(
        QStringLiteral("graphics_api"),
        graphics_api_name(result.graphics_api));
    object.insert(
        QStringLiteral("software_renderer"),
        result.software_renderer);
    return object;
}

QJsonObject pixel_diff_stats_object(const Pixel_diff_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("compared_pixels"), stats.compared_pixels);
    object.insert(QStringLiteral("diff_pixels"), stats.diff_pixels);
    object.insert(QStringLiteral("max_delta"), stats.max_delta);
    return object;
}

QJsonObject json_rect_object(const QRect& rect)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), rect.x());
    object.insert(QStringLiteral("y"), rect.y());
    object.insert(QStringLiteral("width"), rect.width());
    object.insert(QStringLiteral("height"), rect.height());
    return object;
}

QJsonObject json_rectf_object(const QRectF& rect)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), rect.x());
    object.insert(QStringLiteral("y"), rect.y());
    object.insert(QStringLiteral("width"), rect.width());
    object.insert(QStringLiteral("height"), rect.height());
    return object;
}

QJsonObject json_color_object(const QColor& color)
{
    QJsonObject object;
    object.insert(QStringLiteral("red"), color.red());
    object.insert(QStringLiteral("green"), color.green());
    object.insert(QStringLiteral("blue"), color.blue());
    object.insert(QStringLiteral("alpha"), color.alpha());
    return object;
}

QJsonObject lcd_visible_glyph_metrics_object(
    const Lcd_visible_glyph_metrics& metrics)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("has_visible_ink"),
        metrics.has_visible_ink());
    object.insert(QStringLiteral("bbox"), json_rect_object(metrics.bbox));
    object.insert(QStringLiteral("strong_pixels"), metrics.strong_pixels);
    object.insert(QStringLiteral("visible_left_x"), metrics.visible_left_x);
    object.insert(QStringLiteral("visible_right_x"), metrics.visible_right_x);
    object.insert(QStringLiteral("visible_top_y"), metrics.visible_top_y);
    object.insert(
        QStringLiteral("visible_baseline_y"),
        metrics.visible_baseline_y);
    object.insert(
        QStringLiteral("relative_visible_left_x"),
        metrics.relative_visible_left_x);
    object.insert(
        QStringLiteral("relative_visible_right_x"),
        metrics.relative_visible_right_x);
    object.insert(
        QStringLiteral("relative_visible_top_y"),
        metrics.relative_visible_top_y);
    object.insert(
        QStringLiteral("relative_visible_baseline_y"),
        metrics.relative_visible_baseline_y);
    object.insert(QStringLiteral("energy_sum"), metrics.energy_sum);
    object.insert(
        QStringLiteral("weighted_center_x"),
        metrics.weighted_center_x);
    object.insert(
        QStringLiteral("weighted_center_y"),
        metrics.weighted_center_y);
    object.insert(
        QStringLiteral("relative_weighted_center_x"),
        metrics.relative_weighted_center_x);
    object.insert(
        QStringLiteral("relative_weighted_center_y"),
        metrics.relative_weighted_center_y);
    object.insert(
        QStringLiteral("row_energy"),
        json_double_array(metrics.row_energy));
    object.insert(
        QStringLiteral("column_energy"),
        json_double_array(metrics.column_energy));
    return object;
}

QJsonObject pixel_image_ink_stats_object(const Pixel_image_ink_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("ink_pixels"), stats.ink_pixels);
    object.insert(QStringLiteral("bbox"), json_rect_object(stats.bbox));
    object.insert(QStringLiteral("center_x"), stats.center_x);
    object.insert(QStringLiteral("center_y"), stats.center_y);
    return object;
}

QJsonObject lcd_first_ink_column_stats_object(
    const Lcd_first_ink_column_stats& stats)
{
    QJsonArray first_column;
    for (const Lcd_first_ink_column_sample& sample : stats.first_column) {
        QJsonObject item;
        item.insert(QStringLiteral("y"), sample.y);
        item.insert(QStringLiteral("color"), json_color_object(sample.color));
        first_column.append(item);
    }

    QJsonObject object;
    object.insert(QStringLiteral("ink_pixels"), stats.ink_pixels);
    object.insert(QStringLiteral("bbox"), json_rect_object(stats.bbox));
    object.insert(QStringLiteral("first_ink_x"), stats.first_ink_x);
    object.insert(
        QStringLiteral("first_column_rows"),
        stats.first_column_rows);
    object.insert(
        QStringLiteral("distinct_first_column_rgb_colors"),
        stats.distinct_first_column_rgb_colors);
    object.insert(
        QStringLiteral("adjacent_equal_first_column_pairs"),
        stats.adjacent_equal_first_column_pairs);
    object.insert(
        QStringLiteral("longest_equal_first_column_run"),
        stats.longest_equal_first_column_run);
    object.insert(QStringLiteral("first_column"), first_column);
    return object;
}

QJsonObject lcd_horizontal_edge_transition_stats_object(
    const Lcd_horizontal_edge_transition_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("bbox"), json_rect_object(stats.bbox));
    object.insert(QStringLiteral("sampled_rows"), stats.sampled_rows);
    object.insert(
        QStringLiteral("rows_with_transitions"),
        stats.rows_with_transitions);
    object.insert(
        QStringLiteral("transition_count"),
        stats.transition_count);
    object.insert(
        QStringLiteral("max_transition_width"),
        stats.max_transition_width);
    object.insert(
        QStringLiteral("average_transition_width"),
        stats.average_transition_width);
    return object;
}

QJsonObject lcd_repeated_w_cell_stats_object(
    const Lcd_repeated_w_cell_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("cell_pixels"), json_rect_object(stats.cell_pixels));
    object.insert(QStringLiteral("ink_bbox"), json_rect_object(stats.ink_bbox));
    object.insert(QStringLiteral("ink_pixels"), stats.ink_pixels);
    object.insert(QStringLiteral("first_ink_x"), stats.first_ink_x);
    object.insert(
        QStringLiteral("relative_first_ink_x"),
        stats.relative_first_ink_x);
    object.insert(QStringLiteral("relative_bbox_x"), stats.relative_bbox_x);
    object.insert(QStringLiteral("relative_bbox_y"), stats.relative_bbox_y);
    object.insert(
        QStringLiteral("relative_bbox_width"),
        stats.relative_bbox_width);
    object.insert(
        QStringLiteral("relative_bbox_height"),
        stats.relative_bbox_height);
    return object;
}

QJsonObject lcd_repeated_w_stability_stats_object(
    const Lcd_repeated_w_stability_stats& stats)
{
    QJsonArray cells;
    for (const Lcd_repeated_w_cell_stats& cell : stats.cells) {
        cells.append(lcd_repeated_w_cell_stats_object(cell));
    }

    QJsonObject object;
    object.insert(QStringLiteral("cell_count"), stats.cell_count);
    object.insert(
        QStringLiteral("row"),
        k_lcd_repeated_w_row);
    object.insert(
        QStringLiteral("first_column"),
        k_lcd_repeated_w_first_column);
    object.insert(
        QStringLiteral("column_stride"),
        k_lcd_repeated_w_column_stride);
    object.insert(
        QStringLiteral("physical_cell_width"),
        stats.physical_cell_width);
    object.insert(
        QStringLiteral("physical_cell_height"),
        stats.physical_cell_height);
    object.insert(
        QStringLiteral("physical_cell_ascent"),
        stats.physical_cell_ascent);
    object.insert(
        QStringLiteral("cell_width_integer_delta"),
        stats.cell_width_integer_delta);
    object.insert(
        QStringLiteral("cell_height_integer_delta"),
        stats.cell_height_integer_delta);
    object.insert(
        QStringLiteral("cell_ascent_integer_delta"),
        stats.cell_ascent_integer_delta);
    object.insert(
        QStringLiteral("cell_width_integer"),
        stats.cell_width_integer);
    object.insert(
        QStringLiteral("cell_height_integer"),
        stats.cell_height_integer);
    object.insert(
        QStringLiteral("cell_ascent_integer"),
        stats.cell_ascent_integer);
    object.insert(QStringLiteral("compared_cells"), stats.compared_cells);
    object.insert(QStringLiteral("compared_pixels"), stats.compared_pixels);
    object.insert(QStringLiteral("diff_pixels"), stats.diff_pixels);
    object.insert(QStringLiteral("max_delta"), stats.max_delta);
    object.insert(
        QStringLiteral("max_diff_pixels_per_cell"),
        stats.max_diff_pixels_per_cell);
    object.insert(
        QStringLiteral("ink_mask_diff_pixels"),
        stats.ink_mask_diff_pixels);
    object.insert(
        QStringLiteral("max_ink_mask_diff_pixels_per_cell"),
        stats.max_ink_mask_diff_pixels_per_cell);
    object.insert(QStringLiteral("all_cells_have_ink"), stats.all_cells_have_ink);
    object.insert(
        QStringLiteral("max_ink_pixels_delta"),
        stats.max_ink_pixels_delta);
    object.insert(
        QStringLiteral("max_relative_first_ink_x_delta"),
        stats.max_relative_first_ink_x_delta);
    object.insert(
        QStringLiteral("max_relative_bbox_x_delta"),
        stats.max_relative_bbox_x_delta);
    object.insert(
        QStringLiteral("max_relative_bbox_y_delta"),
        stats.max_relative_bbox_y_delta);
    object.insert(
        QStringLiteral("max_relative_bbox_width_delta"),
        stats.max_relative_bbox_width_delta);
    object.insert(
        QStringLiteral("max_relative_bbox_height_delta"),
        stats.max_relative_bbox_height_delta);
    object.insert(QStringLiteral("cells"), cells);
    return object;
}

QJsonObject lcd_ascii_panel_cell_stats_object(
    const Lcd_ascii_panel_cell_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("label"), stats.label);
    object.insert(QStringLiteral("text"), stats.text);
    object.insert(QStringLiteral("row"), stats.row);
    object.insert(QStringLiteral("column"), stats.column);
    object.insert(QStringLiteral("display_width"), stats.display_width);
    object.insert(QStringLiteral("cursor_text"), stats.cursor_text);
    object.insert(QStringLiteral("cell_pixels"), json_rect_object(stats.cell_pixels));
    object.insert(QStringLiteral("ink_bbox"), json_rect_object(stats.ink_bbox));
    object.insert(QStringLiteral("ink_pixels"), stats.ink_pixels);
    object.insert(QStringLiteral("first_ink_x"), stats.first_ink_x);
    object.insert(
        QStringLiteral("relative_first_ink_x"),
        stats.relative_first_ink_x);
    object.insert(QStringLiteral("relative_bbox_x"), stats.relative_bbox_x);
    object.insert(QStringLiteral("relative_bbox_y"), stats.relative_bbox_y);
    object.insert(
        QStringLiteral("relative_bbox_width"),
        stats.relative_bbox_width);
    object.insert(
        QStringLiteral("relative_bbox_height"),
        stats.relative_bbox_height);
    object.insert(
        QStringLiteral("relative_bbox_center_x"),
        stats.relative_bbox_center_x);
    object.insert(
        QStringLiteral("relative_bbox_center_y"),
        stats.relative_bbox_center_y);
    object.insert(
        QStringLiteral("visible"),
        lcd_visible_glyph_metrics_object(stats.visible));
    object.insert(
        QStringLiteral("left_half_ink_pixels"),
        stats.left_half_ink_pixels);
    object.insert(
        QStringLiteral("right_half_ink_pixels"),
        stats.right_half_ink_pixels);
    object.insert(
        QStringLiteral("top_half_ink_pixels"),
        stats.top_half_ink_pixels);
    object.insert(
        QStringLiteral("bottom_half_ink_pixels"),
        stats.bottom_half_ink_pixels);
    object.insert(
        QStringLiteral("horizontal_edge_transitions"),
        lcd_horizontal_edge_transition_stats_object(stats.edge_transitions));
    return object;
}

QJsonObject lcd_ascii_panel_probe_stats_object(
    const Lcd_ascii_panel_probe_stats& stats)
{
    QJsonArray cells;
    for (const Lcd_ascii_panel_cell_stats& cell : stats.cells) {
        cells.append(lcd_ascii_panel_cell_stats_object(cell));
    }

    QJsonObject object;
    object.insert(
        QStringLiteral("expected_drawable_glyphs"),
        stats.expected_drawable_glyphs);
    object.insert(QStringLiteral("probed_cell_count"), stats.probed_cell_count);
    object.insert(
        QStringLiteral("representative_text"),
        lcd_ascii_panel_representative_text());
    object.insert(QStringLiteral("dense_text"), lcd_ascii_panel_dense_text());
    object.insert(
        QStringLiteral("cursor_text"),
        lcd_ascii_panel_cursor_text());
    object.insert(QStringLiteral("user_text"), lcd_ascii_panel_user_text());
    object.insert(
        QStringLiteral("physical_cell_width"),
        stats.physical_cell_width);
    object.insert(
        QStringLiteral("physical_cell_height"),
        stats.physical_cell_height);
    object.insert(
        QStringLiteral("physical_cell_ascent"),
        stats.physical_cell_ascent);
    object.insert(
        QStringLiteral("cell_width_integer_delta"),
        stats.cell_width_integer_delta);
    object.insert(
        QStringLiteral("cell_height_integer_delta"),
        stats.cell_height_integer_delta);
    object.insert(
        QStringLiteral("cell_ascent_integer_delta"),
        stats.cell_ascent_integer_delta);
    object.insert(
        QStringLiteral("cell_width_integer"),
        stats.cell_width_integer);
    object.insert(
        QStringLiteral("cell_height_integer"),
        stats.cell_height_integer);
    object.insert(
        QStringLiteral("cell_ascent_integer"),
        stats.cell_ascent_integer);
    object.insert(
        QStringLiteral("all_probe_cells_have_ink"),
        stats.all_probe_cells_have_ink);
    object.insert(
        QStringLiteral("all_probe_cells_have_visible_ink"),
        stats.all_probe_cells_have_visible_ink);
    object.insert(QStringLiteral("min_ink_pixels"), stats.min_ink_pixels);
    object.insert(
        QStringLiteral("min_visible_strong_pixels"),
        stats.min_visible_strong_pixels);
    object.insert(
        QStringLiteral("key_glyph_transition_count"),
        stats.key_glyph_transition_count);
    object.insert(
        QStringLiteral("max_key_glyph_transition_width"),
        stats.max_key_glyph_transition_width);
    object.insert(
        QStringLiteral("max_key_glyph_average_transition_width"),
        stats.max_key_glyph_average_transition_width);
    object.insert(QStringLiteral("cells"), cells);
    return object;
}

QJsonObject lcd_ascii_panel_placement_delta_object(
    const Lcd_ascii_panel_placement_delta& delta)
{
    QJsonObject object;
    object.insert(QStringLiteral("label"), delta.label);
    object.insert(
        QStringLiteral("reference_has_ink"),
        delta.reference_has_ink);
    object.insert(
        QStringLiteral("candidate_has_ink"),
        delta.candidate_has_ink);
    object.insert(
        QStringLiteral("reference_has_visible_ink"),
        delta.reference_has_visible_ink);
    object.insert(
        QStringLiteral("candidate_has_visible_ink"),
        delta.candidate_has_visible_ink);
    object.insert(QStringLiteral("left_delta"), delta.left_delta);
    object.insert(QStringLiteral("right_delta"), delta.right_delta);
    object.insert(QStringLiteral("top_delta"), delta.top_delta);
    object.insert(QStringLiteral("bottom_delta"), delta.bottom_delta);
    object.insert(
        QStringLiteral("first_ink_x_delta"),
        delta.first_ink_x_delta);
    object.insert(
        QStringLiteral("visible_left_delta"),
        delta.visible_left_delta);
    object.insert(
        QStringLiteral("visible_right_delta"),
        delta.visible_right_delta);
    object.insert(
        QStringLiteral("visible_top_delta"),
        delta.visible_top_delta);
    object.insert(
        QStringLiteral("visible_baseline_delta"),
        delta.visible_baseline_delta);
    object.insert(QStringLiteral("center_x_delta"), delta.center_x_delta);
    object.insert(QStringLiteral("center_y_delta"), delta.center_y_delta);
    object.insert(
        QStringLiteral("visible_weighted_center_x_delta"),
        delta.visible_weighted_center_x_delta);
    object.insert(
        QStringLiteral("visible_weighted_center_y_delta"),
        delta.visible_weighted_center_y_delta);
    object.insert(
        QStringLiteral("signed_top_delta"),
        delta.signed_top_delta);
    object.insert(
        QStringLiteral("signed_bottom_delta"),
        delta.signed_bottom_delta);
    object.insert(
        QStringLiteral("signed_center_y_delta"),
        delta.signed_center_y_delta);
    object.insert(
        QStringLiteral("signed_visible_top_delta"),
        delta.signed_visible_top_delta);
    object.insert(
        QStringLiteral("signed_visible_baseline_delta"),
        delta.signed_visible_baseline_delta);
    object.insert(
        QStringLiteral("signed_visible_weighted_center_y_delta"),
        delta.signed_visible_weighted_center_y_delta);
    return object;
}

QJsonObject lcd_ascii_panel_placement_stats_object(
    const Lcd_ascii_panel_placement_stats& stats)
{
    QJsonArray cells;
    for (const Lcd_ascii_panel_placement_delta& cell : stats.cells) {
        cells.append(lcd_ascii_panel_placement_delta_object(cell));
    }

    QJsonObject object;
    object.insert(QStringLiteral("expected_cells"), stats.expected_cells);
    object.insert(QStringLiteral("compared_cells"), stats.compared_cells);
    object.insert(
        QStringLiteral("all_cells_have_ink"),
        stats.all_cells_have_ink);
    object.insert(
        QStringLiteral("all_cells_have_visible_ink"),
        stats.all_cells_have_visible_ink);
    object.insert(QStringLiteral("max_left_delta"), stats.max_left_delta);
    object.insert(QStringLiteral("max_right_delta"), stats.max_right_delta);
    object.insert(QStringLiteral("max_top_delta"), stats.max_top_delta);
    object.insert(QStringLiteral("max_bottom_delta"), stats.max_bottom_delta);
    object.insert(
        QStringLiteral("max_first_ink_x_delta"),
        stats.max_first_ink_x_delta);
    object.insert(
        QStringLiteral("max_visible_left_delta"),
        stats.max_visible_left_delta);
    object.insert(
        QStringLiteral("max_visible_right_delta"),
        stats.max_visible_right_delta);
    object.insert(
        QStringLiteral("max_visible_top_delta"),
        stats.max_visible_top_delta);
    object.insert(
        QStringLiteral("max_visible_baseline_delta"),
        stats.max_visible_baseline_delta);
    object.insert(
        QStringLiteral("max_center_x_delta"),
        stats.max_center_x_delta);
    object.insert(
        QStringLiteral("max_center_y_delta"),
        stats.max_center_y_delta);
    object.insert(
        QStringLiteral("max_visible_weighted_center_x_delta"),
        stats.max_visible_weighted_center_x_delta);
    object.insert(
        QStringLiteral("max_visible_weighted_center_y_delta"),
        stats.max_visible_weighted_center_y_delta);
    object.insert(
        QStringLiteral("min_signed_top_delta"),
        stats.min_signed_top_delta);
    object.insert(
        QStringLiteral("max_signed_top_delta"),
        stats.max_signed_top_delta);
    object.insert(
        QStringLiteral("signed_top_delta_range"),
        stats.signed_top_delta_range);
    object.insert(
        QStringLiteral("min_signed_bottom_delta"),
        stats.min_signed_bottom_delta);
    object.insert(
        QStringLiteral("max_signed_bottom_delta"),
        stats.max_signed_bottom_delta);
    object.insert(
        QStringLiteral("signed_bottom_delta_range"),
        stats.signed_bottom_delta_range);
    object.insert(
        QStringLiteral("min_signed_center_y_delta"),
        stats.min_signed_center_y_delta);
    object.insert(
        QStringLiteral("max_signed_center_y_delta"),
        stats.max_signed_center_y_delta);
    object.insert(
        QStringLiteral("signed_center_y_delta_range"),
        stats.signed_center_y_delta_range);
    object.insert(
        QStringLiteral("min_signed_visible_top_delta"),
        stats.min_signed_visible_top_delta);
    object.insert(
        QStringLiteral("max_signed_visible_top_delta"),
        stats.max_signed_visible_top_delta);
    object.insert(
        QStringLiteral("signed_visible_top_delta_range"),
        stats.signed_visible_top_delta_range);
    object.insert(
        QStringLiteral("min_signed_visible_baseline_delta"),
        stats.min_signed_visible_baseline_delta);
    object.insert(
        QStringLiteral("max_signed_visible_baseline_delta"),
        stats.max_signed_visible_baseline_delta);
    object.insert(
        QStringLiteral("signed_visible_baseline_delta_range"),
        stats.signed_visible_baseline_delta_range);
    object.insert(
        QStringLiteral("min_signed_visible_weighted_center_y_delta"),
        stats.min_signed_visible_weighted_center_y_delta);
    object.insert(
        QStringLiteral("max_signed_visible_weighted_center_y_delta"),
        stats.max_signed_visible_weighted_center_y_delta);
    object.insert(
        QStringLiteral("signed_visible_weighted_center_y_delta_range"),
        stats.signed_visible_weighted_center_y_delta_range);
    object.insert(QStringLiteral("cells"), cells);
    return object;
}

QJsonObject lcd_ascii_panel_gutter_pair_stats_object(
    const Lcd_ascii_panel_gutter_pair_stats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("left_label"), stats.left_label);
    object.insert(QStringLiteral("right_label"), stats.right_label);
    object.insert(
        QStringLiteral("reference_visible"),
        stats.reference_visible);
    object.insert(
        QStringLiteral("candidate_visible"),
        stats.candidate_visible);
    object.insert(QStringLiteral("reference_gutter"), stats.reference_gutter);
    object.insert(QStringLiteral("candidate_gutter"), stats.candidate_gutter);
    object.insert(QStringLiteral("gutter_delta"), stats.gutter_delta);
    return object;
}

QJsonObject lcd_ascii_panel_gutter_stats_object(
    const Lcd_ascii_panel_gutter_stats& stats)
{
    QJsonArray pairs;
    for (const Lcd_ascii_panel_gutter_pair_stats& pair : stats.pairs) {
        pairs.append(lcd_ascii_panel_gutter_pair_stats_object(pair));
    }

    QJsonObject object;
    object.insert(QStringLiteral("expected_pairs"), stats.expected_pairs);
    object.insert(QStringLiteral("compared_pairs"), stats.compared_pairs);
    object.insert(
        QStringLiteral("all_pairs_visible"),
        stats.all_pairs_visible);
    object.insert(QStringLiteral("max_gutter_delta"), stats.max_gutter_delta);
    object.insert(QStringLiteral("pairs"), pairs);
    return object;
}

QJsonObject raster_variant_probe_record_object(
    const Raster_variant_probe_record& record)
{
    QJsonObject object;
    object.insert(QStringLiteral("sample"), record.sample);
    object.insert(QStringLiteral("variant"), record.variant);
    object.insert(QStringLiteral("comparison_scope"), record.comparison_scope);
    object.insert(
        QStringLiteral("image_size"),
        json_size_object(record.image_size));
    object.insert(
        QStringLiteral("nominal_raster_pixel_size"),
        record.nominal_raster_pixel_size);
    object.insert(
        QStringLiteral("actual_raster_pixel_size"),
        record.actual_raster_pixel_size);
    object.insert(
        QStringLiteral("glyph_image_count"),
        record.glyph_image_count);
    object.insert(
        QStringLiteral("valid_tile_count"),
        record.valid_tile_count);
    object.insert(
        QStringLiteral("max_glyph_image_size"),
        json_size_object(record.max_glyph_image_size));
    object.insert(
        QStringLiteral("diff_to_qt_text_layout"),
        pixel_diff_stats_object(record.diff_to_qt_text_layout));
    object.insert(
        QStringLiteral("ink"),
        pixel_image_ink_stats_object(record.ink));
    object.insert(
        QStringLiteral("distinct_ink_rgb_colors"),
        record.distinct_ink_rgb_colors);
    return object;
}

QJsonObject raster_variant_probe_object(
    const Raster_variant_probe_result& result)
{
    QJsonArray records;
    for (const Raster_variant_probe_record& record : result.records) {
        records.append(raster_variant_probe_record_object(record));
    }

    QJsonObject object;
    object.insert(
        QStringLiteral("sheet_image_size"),
        json_size_object(result.sheet.size()));
    object.insert(QStringLiteral("records"), records);
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic only: raster-size variants for glyph-quality classification"));
    return object;
}

QJsonObject msdf_single_w_probe_result_object(
    const Pixel_parity_fixture&        fixture,
    const Msdf_single_w_probe_result&  msdf_result,
    const Pixel_render_result&         raw_atlas_reference,
    const Pixel_render_result&         qt_text_reference)
{
    const Lcd_first_ink_column_stats first_ink_column =
        measure_lcd_first_ink_column(
            msdf_result.render.image,
            fixture.glyph_masks,
            fixture.device_pixel_ratio);

    QJsonObject object;
    object.insert(QStringLiteral("compiled"), msdf_result.compiled);
    object.insert(QStringLiteral("atlas_built"), msdf_result.atlas_built);
    object.insert(QStringLiteral("message"), msdf_result.message);
    object.insert(QStringLiteral("font_source"), msdf_result.font_source);
    object.insert(QStringLiteral("font_data_bytes"), msdf_result.font_data_bytes);
    object.insert(QStringLiteral("pixel_height"), msdf_result.pixel_height);
    object.insert(QStringLiteral("atlas_size"), msdf_result.atlas_size);
    object.insert(QStringLiteral("px_range"), msdf_result.px_range);
    object.insert(QStringLiteral("advance_x"), msdf_result.advance_x);
    object.insert(
        QStringLiteral("baseline_offset_px"),
        msdf_result.baseline_offset_px);
    object.insert(QStringLiteral("plane_rect"), json_rectf_object(msdf_result.plane_rect));
    object.insert(QStringLiteral("draw_rect"), json_rectf_object(msdf_result.draw_rect));
    object.insert(
        QStringLiteral("render"),
        lcd_probe_render_result_object(msdf_result.render));
    object.insert(
        QStringLiteral("first_ink_column"),
        lcd_first_ink_column_stats_object(first_ink_column));
    object.insert(
        QStringLiteral("horizontal_edge_transitions"),
        lcd_horizontal_edge_transition_stats_object(
            measure_lcd_horizontal_edge_transitions(
                msdf_result.render.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio,
                first_ink_column)));
    object.insert(
        QStringLiteral("comparison_to_raw_atlas_reference"),
        lcd_probe_glyph_stats_object(
            compare_glyph_regions(
                raw_atlas_reference.image,
                msdf_result.render.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio)));
    object.insert(
        QStringLiteral("comparison_to_qt_text_node"),
        lcd_probe_glyph_stats_object(
            compare_glyph_regions(
                qt_text_reference.image,
                msdf_result.render.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio)));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic only: CPU MSDF W render, not a terminal renderer path"));
    return object;
}

QJsonObject lcd_probe_report_object(const term::Qsg_atlas_frame_report& report);

QJsonObject lcd_single_w_probe_object(
    const Pixel_parity_fixture& fixture,
    const Pixel_render_result&  atlas,
    const Pixel_render_result&  raw_atlas_reference,
    const Pixel_render_result&  qt_text_reference,
    const Msdf_single_w_probe_result& msdf_result)
{
    const Lcd_first_ink_column_stats atlas_first_ink_column =
        measure_lcd_first_ink_column(
            atlas.image,
            fixture.glyph_masks,
            fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats raw_atlas_first_ink_column =
        measure_lcd_first_ink_column(
            raw_atlas_reference.image,
            fixture.glyph_masks,
            fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats qt_text_first_ink_column =
        measure_lcd_first_ink_column(
            qt_text_reference.image,
            fixture.glyph_masks,
            fixture.device_pixel_ratio);

    QJsonObject object;
    object.insert(
        QStringLiteral("fixture_logical_size"),
        json_sizef_object(fixture.logical_size));
    object.insert(
        QStringLiteral("atlas"),
        lcd_probe_render_result_object(atlas));
    object.insert(
        QStringLiteral("atlas_report"),
        lcd_probe_report_object(atlas.atlas_report));
    object.insert(
        QStringLiteral("raw_atlas_reference"),
        lcd_probe_render_result_object(raw_atlas_reference));
    object.insert(
        QStringLiteral("qt_text_node_reference"),
        lcd_probe_render_result_object(qt_text_reference));
    object.insert(
        QStringLiteral("atlas_first_ink_column"),
        lcd_first_ink_column_stats_object(atlas_first_ink_column));
    object.insert(
        QStringLiteral("atlas_horizontal_edge_transitions"),
        lcd_horizontal_edge_transition_stats_object(
            measure_lcd_horizontal_edge_transitions(
                atlas.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio,
                atlas_first_ink_column)));
    object.insert(
        QStringLiteral("raw_atlas_first_ink_column"),
        lcd_first_ink_column_stats_object(raw_atlas_first_ink_column));
    object.insert(
        QStringLiteral("raw_atlas_horizontal_edge_transitions"),
        lcd_horizontal_edge_transition_stats_object(
            measure_lcd_horizontal_edge_transitions(
                raw_atlas_reference.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio,
                raw_atlas_first_ink_column)));
    object.insert(
        QStringLiteral("qt_text_node_first_ink_column"),
        lcd_first_ink_column_stats_object(qt_text_first_ink_column));
    object.insert(
        QStringLiteral("qt_text_node_horizontal_edge_transitions"),
        lcd_horizontal_edge_transition_stats_object(
            measure_lcd_horizontal_edge_transitions(
                qt_text_reference.image,
                fixture.glyph_masks,
                fixture.device_pixel_ratio,
                qt_text_first_ink_column)));
    object.insert(
        QStringLiteral("msdf"),
        msdf_single_w_probe_result_object(
            fixture,
            msdf_result,
            raw_atlas_reference,
            qt_text_reference));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic only: isolates one W glyph for first-ink-column coverage inspection"));
    return object;
}

QJsonObject lcd_repeated_w_probe_object(
    const Pixel_parity_fixture& fixture,
    const Pixel_render_result&  atlas)
{
    const Lcd_repeated_w_stability_stats stability =
        measure_lcd_repeated_w_stability(
            fixture,
            atlas.image,
            k_lcd_repeated_w_row,
            k_lcd_repeated_w_first_column,
            k_lcd_repeated_w_column_stride,
            k_lcd_repeated_w_cell_count);

    QJsonObject object;
    object.insert(
        QStringLiteral("fixture_logical_size"),
        json_sizef_object(fixture.logical_size));
    object.insert(
        QStringLiteral("atlas"),
        lcd_probe_render_result_object(atlas));
    object.insert(
        QStringLiteral("atlas_report"),
        lcd_probe_report_object(atlas.atlas_report));
    object.insert(
        QStringLiteral("stability"),
        lcd_repeated_w_stability_stats_object(stability));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic gate: repeated W MSDF ownership, integer cells, stable crops, and no x drift"));
    return object;
}

QJsonObject lcd_ascii_panel_probe_object(
    const Pixel_parity_fixture&          fixture,
    const Pixel_render_result&           atlas,
    const Pixel_render_result&           raw_atlas_reference,
    const Pixel_render_result&           qt_text_reference,
    const Lcd_ascii_panel_probe_stats&   stats,
    const Lcd_ascii_panel_probe_stats&   raw_atlas_reference_stats,
    const Lcd_ascii_panel_probe_stats&   qt_text_reference_stats,
    const Lcd_ascii_panel_placement_stats& raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats& user_text_raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats& user_text_qt_text_placement,
    const Lcd_ascii_panel_gutter_stats&    user_text_raw_atlas_gutters,
    const Lcd_ascii_panel_gutter_stats&    user_text_qt_text_gutters)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("fixture_logical_size"),
        json_sizef_object(fixture.logical_size));
    object.insert(
        QStringLiteral("atlas"),
        lcd_probe_render_result_object(atlas));
    object.insert(
        QStringLiteral("atlas_report"),
        lcd_probe_report_object(atlas.atlas_report));
    object.insert(
        QStringLiteral("raw_atlas_reference"),
        lcd_probe_render_result_object(raw_atlas_reference));
    object.insert(
        QStringLiteral("qt_text_reference"),
        lcd_probe_render_result_object(qt_text_reference));
    object.insert(
        QStringLiteral("stats"),
        lcd_ascii_panel_probe_stats_object(stats));
    object.insert(
        QStringLiteral("raw_atlas_reference_stats"),
        lcd_ascii_panel_probe_stats_object(raw_atlas_reference_stats));
    object.insert(
        QStringLiteral("qt_text_reference_stats"),
        lcd_ascii_panel_probe_stats_object(qt_text_reference_stats));
    object.insert(
        QStringLiteral("raw_atlas_placement"),
        lcd_ascii_panel_placement_stats_object(raw_atlas_placement));
    object.insert(
        QStringLiteral("user_text_raw_atlas_placement"),
        lcd_ascii_panel_placement_stats_object(user_text_raw_atlas_placement));
    object.insert(
        QStringLiteral("user_text_qt_text_placement"),
        lcd_ascii_panel_placement_stats_object(user_text_qt_text_placement));
    object.insert(
        QStringLiteral("user_text_raw_atlas_gutters"),
        lcd_ascii_panel_gutter_stats_object(user_text_raw_atlas_gutters));
    object.insert(
        QStringLiteral("user_text_qt_text_gutters"),
        lcd_ascii_panel_gutter_stats_object(user_text_qt_text_gutters));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic gate: printable ASCII and cursor text MSDF ownership, "
            "integer cells, visible ink, conservative edge quality, raw-relative "
            "placement, and build_msdf visible baseline/gutters; Qt-relative "
            "placement/gutters are recorded diagnostics"));
    return object;
}

QJsonObject json_pointf_object(QPointF point)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), point.x());
    object.insert(QStringLiteral("y"), point.y());
    return object;
}

QJsonObject lcd_translated_repeated_w_probe_object(
    const Pixel_parity_fixture&            fixture,
    const Pixel_render_result&             atlas,
    QPointF                                host_translation,
    const Lcd_repeated_w_stability_stats&  stability,
    const Lcd_repeated_w_stability_stats&  origin_match)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("fixture_logical_size"),
        json_sizef_object(fixture.logical_size));
    object.insert(
        QStringLiteral("host_translation"),
        json_pointf_object(host_translation));
    object.insert(
        QStringLiteral("atlas"),
        lcd_probe_render_result_object(atlas));
    object.insert(
        QStringLiteral("atlas_report"),
        lcd_probe_report_object(atlas.atlas_report));
    object.insert(
        QStringLiteral("stability"),
        lcd_repeated_w_stability_stats_object(stability));
    object.insert(
        QStringLiteral("origin_match"),
        lcd_repeated_w_stability_stats_object(origin_match));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic gate: translated repeated W MSDF ownership, stable crops, and exact origin correspondence"));
    return object;
}

QJsonObject terminal_cell_metrics_object(term::terminal_cell_metrics_t metrics)
{
    QJsonObject object;
    object.insert(QStringLiteral("width"), metrics.width);
    object.insert(QStringLiteral("height"), metrics.height);
    object.insert(QStringLiteral("ascent"), metrics.ascent);
    object.insert(QStringLiteral("descent"), metrics.descent);
    return object;
}

QJsonObject metrics_placement_font_metrics_object(
    const Metrics_placement_font_metrics& metrics)
{
    QJsonObject object;
    object.insert(QStringLiteral("m_advance"), metrics.m_advance);
    object.insert(
        QStringLiteral("average_char_width"),
        metrics.average_char_width);
    object.insert(QStringLiteral("ascent"), metrics.ascent);
    object.insert(QStringLiteral("descent"), metrics.descent);
    object.insert(QStringLiteral("height"), metrics.height);
    object.insert(QStringLiteral("line_spacing"), metrics.line_spacing);
    return object;
}

QJsonObject metrics_placement_delta_object(
    const Metrics_placement_delta& delta)
{
    QJsonObject object;
    object.insert(QStringLiteral("raw_logical"), delta.raw_logical);
    object.insert(QStringLiteral("snapped_logical"), delta.snapped_logical);
    object.insert(QStringLiteral("delta_logical"), delta.delta_logical);
    object.insert(QStringLiteral("delta_physical"), delta.delta_physical);
    return object;
}

QJsonObject metrics_placement_glyph_record_object(
    const Metrics_placement_glyph_record& record)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("source_string_start"),
        static_cast<int>(record.source_string_start));
    object.insert(
        QStringLiteral("source_string_end"),
        static_cast<int>(record.source_string_end));
    object.insert(
        QStringLiteral("glyph_index"),
        static_cast<int>(record.glyph_index));
    object.insert(
        QStringLiteral("qt_layout_origin"),
        json_pointf_object(record.qt_layout_origin));
    object.insert(QStringLiteral("qt_advance"), record.qt_advance);
    object.insert(
        QStringLiteral("terminal_cell_origin"),
        json_pointf_object(record.terminal_cell_origin));
    object.insert(
        QStringLiteral("production_origin"),
        json_pointf_object(record.production_origin));
    object.insert(
        QStringLiteral("terminal_minus_qt_x"),
        record.terminal_minus_qt_x);
    object.insert(
        QStringLiteral("production_minus_terminal_x"),
        record.production_minus_terminal_x);
    object.insert(QStringLiteral("owner_column"), record.owner_column);
    object.insert(QStringLiteral("owner_cell_span"), record.owner_cell_span);
    object.insert(
        QStringLiteral("production_origin_matches_terminal"),
        record.production_origin_matches_terminal);
    object.insert(
        QStringLiteral("terminal_origin_physical_snapped"),
        record.terminal_origin_physical_snapped);
    object.insert(
        QStringLiteral("production_origin_physical_snapped"),
        record.production_origin_physical_snapped);
    return object;
}

QJsonObject metrics_placement_probe_object(
    const Metrics_placement_probe_result& result)
{
    QJsonArray glyphs;
    for (const Metrics_placement_glyph_record& record : result.glyphs) {
        glyphs.append(metrics_placement_glyph_record_object(record));
    }

    QJsonObject deltas;
    deltas.insert(
        QStringLiteral("width"),
        metrics_placement_delta_object(result.width_delta));
    deltas.insert(
        QStringLiteral("ascent"),
        metrics_placement_delta_object(result.ascent_delta));
    deltas.insert(
        QStringLiteral("descent"),
        metrics_placement_delta_object(result.descent_delta));
    deltas.insert(
        QStringLiteral("height"),
        metrics_placement_delta_object(result.height_delta));
    deltas.insert(
        QStringLiteral("line_spacing"),
        metrics_placement_delta_object(result.line_spacing_delta));

    QJsonObject summary;
    summary.insert(
        QStringLiteral("matched_glyph_count"),
        result.matched_glyph_count);
    summary.insert(QStringLiteral("max_abs_x_delta"), result.max_abs_x_delta);
    summary.insert(QStringLiteral("mean_abs_x_delta"), result.mean_abs_x_delta);
    summary.insert(QStringLiteral("first_x_delta"), result.first_x_delta);
    summary.insert(QStringLiteral("last_x_delta"), result.last_x_delta);
    summary.insert(QStringLiteral("mean_qt_advance"), result.mean_qt_advance);
    summary.insert(
        QStringLiteral("terminal_cell_advance"),
        result.terminal_cell_advance);

    QJsonObject object;
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic only: Qt natural layout measured against stable terminal cells"));
    object.insert(QStringLiteral("sample_text"), result.sample_text);
    object.insert(QStringLiteral("device_pixel_ratio"), result.device_pixel_ratio);
    object.insert(
        QStringLiteral("natural_font_metrics"),
        metrics_placement_font_metrics_object(result.natural_metrics));
    object.insert(
        QStringLiteral("cell_stable_font_metrics"),
        metrics_placement_font_metrics_object(result.cell_stable_metrics));
    object.insert(
        QStringLiteral("snapped_terminal_metrics"),
        terminal_cell_metrics_object(result.snapped_metrics));
    object.insert(QStringLiteral("metric_deltas"), deltas);
    object.insert(
        QStringLiteral("qtext_layout_natural_width"),
        result.qtext_layout_natural_width);
    object.insert(
        QStringLiteral("qfont_horizontal_advance"),
        result.qfont_horizontal_advance);
    object.insert(
        QStringLiteral("terminal_cell_count"),
        result.terminal_cell_count);
    object.insert(
        QStringLiteral("terminal_run_width"),
        result.terminal_run_width);
    object.insert(
        QStringLiteral("terminal_minus_qtext_width"),
        result.terminal_minus_qtext_width);
    object.insert(QStringLiteral("qtext_line_valid"), result.qtext_line_valid);
    object.insert(
        QStringLiteral("production_shape_valid"),
        result.production_shape_valid);
    object.insert(QStringLiteral("summary"), summary);
    object.insert(QStringLiteral("glyphs"), glyphs);
    return object;
}

QJsonObject lcd_probe_qt_text_reference_comparison_object(
    const Pixel_parity_fixture& fixture,
    const Pixel_render_result&  atlas,
    const Pixel_render_result&  qt_text_reference)
{
    const Pixel_glyph_stats glyphs = compare_glyph_regions(
        qt_text_reference.image,
        atlas.image,
        fixture.glyph_masks,
        fixture.device_pixel_ratio);

    QJsonObject object;
    object.insert(
        QStringLiteral("image_size_match"),
        qt_text_reference.image.size() == atlas.image.size());
    object.insert(
        QStringLiteral("render_dpr_match"),
        pixel_device_pixel_ratios_match(
            qt_text_reference.device_pixel_ratio,
            atlas.device_pixel_ratio));
    object.insert(
        QStringLiteral("image_dpr_match"),
        pixel_device_pixel_ratios_match(
            qt_text_reference.image_device_pixel_ratio,
            atlas.image_device_pixel_ratio));
    object.insert(
        QStringLiteral("glyph_mask_stats"),
        lcd_probe_glyph_stats_object(glyphs));
    object.insert(
        QStringLiteral("renderer_contract"),
        QStringLiteral(
            "diagnostic only: Qt text-node quality signal, not a pixel-equivalence target"));
    return object;
}

QJsonObject lcd_probe_record_object(const Lcd_glyph_probe_record& record)
{
    QJsonObject object;
    object.insert(QStringLiteral("family"), record.family_label);
    object.insert(QStringLiteral("font_family"), record.font_family);
    object.insert(QStringLiteral("font_style"), record.font_style);
    object.insert(
        QStringLiteral("glyph_index"),
        static_cast<int>(record.glyph_index));
    object.insert(QStringLiteral("glyph_run_index"), record.glyph_run_index);
    object.insert(QStringLiteral("glyph_index_in_run"), record.glyph_index_in_run);
    object.insert(QStringLiteral("image_format_id"), record.image_format_id);
    object.insert(QStringLiteral("image_format"), record.image_format_name);
    object.insert(
        QStringLiteral("image_size"),
        json_size_object(record.image_size));
    object.insert(
        QStringLiteral("requested_presentation"),
        record.requested_presentation);
    object.insert(QStringLiteral("coverage_candidate"), record.coverage_candidate);
    object.insert(
        QStringLiteral("production_tile_valid"),
        record.production_tile_valid);
    return object;
}

QJsonObject glyph_miss_diagnostic_object(
    const term::Qsg_atlas_glyph_miss_diagnostic& miss)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), miss.valid);
    object.insert(
        QStringLiteral("cause"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_miss_cause_name(miss.cause)));
    object.insert(
        QStringLiteral("coverage_kind"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_coverage_kind_name(
                miss.image.coverage_kind)));
    object.insert(
        QStringLiteral("presentation"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_image_presentation_name(
                miss.image.presentation)));
    object.insert(
        QStringLiteral("source_format"),
        static_cast<int>(miss.image.source_format));
    object.insert(
        QStringLiteral("source_size"),
        json_size_object(miss.image.source_size));
    object.insert(
        QStringLiteral("glyph_index"),
        static_cast<int>(miss.image.glyph_index));
    object.insert(QStringLiteral("fallback_face_id"), miss.image.fallback_face_id);
    object.insert(QStringLiteral("text_run_index"), miss.image.text_run_index);
    object.insert(QStringLiteral("glyph_run_index"), miss.image.glyph_run_index);
    object.insert(
        QStringLiteral("glyph_index_in_run"),
        miss.image.glyph_index_in_run);
    object.insert(
        QStringLiteral("source_string_start"),
        static_cast<int>(miss.image.source_string_start));
    object.insert(
        QStringLiteral("source_string_end"),
        static_cast<int>(miss.image.source_string_end));
    object.insert(QStringLiteral("tile_size"), json_size_object(miss.tile_size));
    object.insert(
        QStringLiteral("tile_bytes_per_line"),
        miss.tile_bytes_per_line);
    object.insert(QStringLiteral("atlas_page_count"), miss.atlas_page_count);
    object.insert(QStringLiteral("atlas_page_budget"), miss.atlas_page_budget);
    object.insert(
        QStringLiteral("atlas_page_size"),
        json_size_object(miss.atlas_page_size));
    return object;
}

QJsonObject warm_lazy_summary_object(
    const term::Qsg_atlas_warm_lazy_summary& summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("warm_completed"), summary.warm_completed);
    object.insert(
        QStringLiteral("warm_broad_seed_skipped"),
        summary.warm_broad_seed_skipped);
    json_insert_u64(object, QStringLiteral("warm_epoch"), summary.warm_epoch);
    object.insert(QStringLiteral("warm_seed_strings"), summary.warm_seed_strings);
    object.insert(
        QStringLiteral("warm_shaped_glyph_records"),
        summary.warm_shaped_glyph_records);
    object.insert(
        QStringLiteral("warm_covered_glyph_records"),
        summary.warm_covered_glyph_records);
    object.insert(
        QStringLiteral("warm_skipped_glyph_records"),
        summary.warm_skipped_glyph_records);
    object.insert(
        QStringLiteral("warm_environment_skipped_glyph_records"),
        summary.warm_environment_skipped_glyph_records);
    object.insert(
        QStringLiteral("warm_failed_glyph_records"),
        summary.warm_failed_glyph_records);
    object.insert(
        QStringLiteral("warm_missing_string_indexes"),
        summary.warm_missing_string_indexes);
    object.insert(
        QStringLiteral("warm_invalid_string_indexes"),
        summary.warm_invalid_string_indexes);
    object.insert(
        QStringLiteral("warm_unsupported_images"),
        summary.warm_unsupported_images);
    object.insert(QStringLiteral("warm_cache_hits"), summary.warm_cache_hits);
    object.insert(
        QStringLiteral("warm_insert_attempts"),
        summary.warm_insert_attempts);
    object.insert(QStringLiteral("warm_inserts"), summary.warm_inserts);
    object.insert(
        QStringLiteral("warm_failed_inserts"),
        summary.warm_failed_inserts);
    object.insert(QStringLiteral("warm_elapsed_ms"), summary.warm_elapsed_ms);
    object.insert(
        QStringLiteral("warm_page_pressure"),
        summary.warm_page_pressure);
    object.insert(
        QStringLiteral("lazy_insert_attempts"),
        summary.lazy_insert_attempts);
    object.insert(QStringLiteral("lazy_inserts"), summary.lazy_inserts);
    object.insert(
        QStringLiteral("lazy_failed_inserts"),
        summary.lazy_failed_inserts);
    object.insert(QStringLiteral("lazy_elapsed_ms"), summary.lazy_elapsed_ms);
    object.insert(QStringLiteral("lazy_max_insert_us"), summary.lazy_max_insert_us);
    object.insert(QStringLiteral("lazy_frames"), summary.lazy_frames);
    object.insert(QStringLiteral("incomplete_frames"), summary.incomplete_frames);
    return object;
}

QJsonObject lcd_probe_report_object(const term::Qsg_atlas_frame_report& report)
{
    QJsonObject capabilities;
    capabilities.insert(
        QStringLiteral("command_buffer_non_null"),
        report.command_buffer_non_null);
    capabilities.insert(
        QStringLiteral("render_target_non_null"),
        report.render_target_non_null);
    capabilities.insert(QStringLiteral("rhi_non_null"), report.rhi_non_null);
    capabilities.insert(
        QStringLiteral("msdf_text_renderer_enabled"),
        report.msdf_text_renderer_enabled);
    capabilities.insert(
        QStringLiteral("msdf_text_renderer_compiled"),
        report.msdf_text_renderer_compiled);
    capabilities.insert(
        QStringLiteral("msdf_text_renderer_active"),
        report.msdf_text_renderer_active);
    capabilities.insert(
        QStringLiteral("msdf_text_shader_package_available"),
        report.msdf_text_shader_package_available);
    capabilities.insert(
        QStringLiteral("msdf_text_atlas_built"),
        report.msdf_text_atlas_built);
    capabilities.insert(
        QStringLiteral("msdf_text_atlas_ready"),
        report.msdf_text_atlas_ready);
    capabilities.insert(
        QStringLiteral("msdf_text_texture_ready"),
        report.msdf_text_texture_ready);
    capabilities.insert(
        QStringLiteral("msdf_text_resources_ready"),
        report.msdf_text_resources_ready);
    capabilities.insert(
        QStringLiteral("msdf_text_supported_runs"),
        report.msdf_text_supported_runs);
    capabilities.insert(
        QStringLiteral("msdf_text_runs"),
        report.msdf_text_runs);
    capabilities.insert(
        QStringLiteral("msdf_text_glyph_instances"),
        report.msdf_text_glyph_instances);
    capabilities.insert(
        QStringLiteral("msdf_text_draw_calls"),
        report.msdf_text_draw_calls);
    capabilities.insert(
        QStringLiteral("msdf_text_missed_supported_runs"),
        report.msdf_text_missed_supported_runs);
    capabilities.insert(
        QStringLiteral("msdf_text_missed_supported_glyphs"),
        report.msdf_text_missed_supported_glyphs);
    capabilities.insert(
        QStringLiteral("msdf_text_message"),
        report.msdf_text_message);
    capabilities.insert(
        QStringLiteral("msdf_text_font_data_bytes"),
        report.msdf_text_font_data_bytes);
    capabilities.insert(
        QStringLiteral("msdf_text_pixel_height"),
        report.msdf_text_pixel_height);
    capabilities.insert(
        QStringLiteral("msdf_text_atlas_size"),
        report.msdf_text_atlas_size);
    capabilities.insert(
        QStringLiteral("msdf_text_px_range"),
        report.msdf_text_px_range);
    capabilities.insert(
        QStringLiteral("coverage_texture_created"),
        report.coverage_texture_created);
    capabilities.insert(
        QStringLiteral("coverage_upload_recorded"),
        report.coverage_upload_recorded);
    capabilities.insert(
        QStringLiteral("raw_font_rasterized"),
        report.raw_font_rasterized);
    capabilities.insert(
        QStringLiteral("raw_font_rasterized_in_prepare"),
        report.raw_font_rasterized_in_prepare);

    QJsonObject frame_build;
    frame_build.insert(
        QStringLiteral("emoji_presentation_runs"),
        report.frame_build.emoji_presentation_runs);
    frame_build.insert(
        QStringLiteral("glyph_coverage_failures"),
        report.frame_build.glyph_coverage_failures);
    frame_build.insert(
        QStringLiteral("glyph_atlas_insert_failures"),
        report.frame_build.glyph_atlas_insert_failures);
    frame_build.insert(
        QStringLiteral("glyph_missed_instances"),
        report.frame_build.glyph_missed_instances);
    frame_build.insert(
        QStringLiteral("max_glyph_instance_page"),
        report.frame_build.max_glyph_instance_page);
    frame_build.insert(
        QStringLiteral("distinct_glyph_faces"),
        report.frame_build.distinct_glyph_faces);
    frame_build.insert(
        QStringLiteral("fallback_glyph_faces"),
        report.frame_build.fallback_glyph_faces);
    frame_build.insert(
        QStringLiteral("snapped_origin_failures"),
        report.frame_build.snapped_origin_failures);

    QJsonObject coverage;
    coverage.insert(
        QStringLiteral("grayscale_masks"),
        report.frame_build.glyph_coverage.grayscale_masks);
    coverage.insert(
        QStringLiteral("lcd_rgb_masks"),
        report.frame_build.glyph_coverage.lcd_rgb_masks);
    coverage.insert(
        QStringLiteral("lcd_bgr_masks"),
        report.frame_build.glyph_coverage.lcd_bgr_masks);
    coverage.insert(
        QStringLiteral("color_images"),
        report.frame_build.glyph_coverage.color_images);
    coverage.insert(
        QStringLiteral("ambiguous_images"),
        report.frame_build.glyph_coverage.ambiguous_images);
    coverage.insert(
        QStringLiteral("unsupported_images"),
        report.frame_build.glyph_coverage.unsupported_images);
    coverage.insert(
        QStringLiteral("missed_images"),
        report.frame_build.glyph_coverage.missed_images);
    frame_build.insert(QStringLiteral("coverage"), coverage);
    frame_build.insert(
        QStringLiteral("first_glyph_miss"),
        glyph_miss_diagnostic_object(report.frame_build.first_glyph_miss));

    QJsonObject render;
    render.insert(QStringLiteral("draw_calls"), report.render.draw_calls);
    render.insert(
        QStringLiteral("rect_draw_calls"),
        report.render.rect_draw_calls);
    render.insert(
        QStringLiteral("glyph_draw_calls"),
        report.render.glyph_draw_calls);
    render.insert(
        QStringLiteral("msdf_text_draw_calls"),
        report.render.msdf_text_draw_calls);
    render.insert(
        QStringLiteral("atlas_page_count"),
        report.render.atlas_page_count);
    render.insert(
        QStringLiteral("atlas_page_budget"),
        report.render.atlas_page_budget);
    json_insert_u64(
        render,
        QStringLiteral("atlas_page_bytes"),
        report.render.atlas_page_bytes);
    json_insert_u64(
        render,
        QStringLiteral("atlas_allocated_bytes"),
        report.render.atlas_allocated_bytes);
    json_insert_u64(
        render,
        QStringLiteral("atlas_budget_bytes"),
        report.render.atlas_budget_bytes);
    json_insert_u64(
        render,
        QStringLiteral("atlas_used_bytes"),
        report.render.atlas_used_bytes);
    json_insert_u64(
        render,
        QStringLiteral("atlas_failed_inserts"),
        report.render.atlas_failed_inserts);
    render.insert(
        QStringLiteral("coverage_texture_uploaded"),
        report.render.coverage_texture_uploaded);
    render.insert(
        QStringLiteral("coverage_texture_skipped"),
        report.render.coverage_texture_skipped);
    render.insert(
        QStringLiteral("msdf_text_texture_uploaded"),
        report.render.msdf_text_texture_uploaded);
    render.insert(
        QStringLiteral("msdf_text_texture_ready"),
        report.render.msdf_text_texture_ready);
    render.insert(
        QStringLiteral("atlas_page_pressure"),
        report.render.atlas_page_pressure);
    render.insert(
        QStringLiteral("shaped_text_runs"),
        report.render.shaped_text_runs);
    render.insert(
        QStringLiteral("shaped_glyph_records"),
        report.render.shaped_glyph_records);
    render.insert(
        QStringLiteral("shaped_missing_string_indexes"),
        report.render.shaped_missing_string_indexes);
    render.insert(
        QStringLiteral("shaped_invalid_string_indexes"),
        report.render.shaped_invalid_string_indexes);
    render.insert(
        QStringLiteral("glyph_buffer_instances"),
        report.render.glyph_buffer_instances);
    render.insert(
        QStringLiteral("sampler_mode"),
        QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(
                report.render.glyph_sampler_mode)));
    render.insert(
        QStringLiteral("msdf_text_sampler_mode"),
        QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(
                report.render.msdf_text_sampler_mode)));
    render.insert(
        QStringLiteral("glyph_shader_package_available"),
        report.render.glyph_shader_package_available);
    render.insert(
        QStringLiteral("msdf_text_shader_package_available"),
        report.render.msdf_text_shader_package_available);
    render.insert(
        QStringLiteral("msdf_text_atlas_built"),
        report.render.msdf_text_atlas_built);
    render.insert(
        QStringLiteral("msdf_text_atlas_ready"),
        report.render.msdf_text_atlas_ready);
    render.insert(
        QStringLiteral("msdf_text_resources_ready"),
        report.render.msdf_text_resources_ready);
    render.insert(
        QStringLiteral("msdf_text_supported_runs"),
        report.render.msdf_text_supported_runs);
    render.insert(
        QStringLiteral("msdf_text_runs"),
        report.render.msdf_text_runs);
    render.insert(
        QStringLiteral("msdf_text_glyph_instances"),
        report.render.msdf_text_glyph_instances);
    render.insert(
        QStringLiteral("msdf_text_missed_supported_runs"),
        report.render.msdf_text_missed_supported_runs);
    render.insert(
        QStringLiteral("msdf_text_missed_supported_glyphs"),
        report.render.msdf_text_missed_supported_glyphs);
    render.insert(
        QStringLiteral("msdf_text_message"),
        report.render.msdf_text_message);
    render.insert(
        QStringLiteral("msdf_text_font_data_bytes"),
        report.render.msdf_text_font_data_bytes);
    render.insert(
        QStringLiteral("msdf_text_pixel_height"),
        report.render.msdf_text_pixel_height);
    render.insert(
        QStringLiteral("msdf_text_atlas_size"),
        report.render.msdf_text_atlas_size);
    render.insert(
        QStringLiteral("msdf_text_px_range"),
        report.render.msdf_text_px_range);
    render.insert(
        QStringLiteral("dual_source_probe_shader_package_available"),
        report.render.dual_source_probe_shader_package_available);
    render.insert(
        QStringLiteral("dual_source_blend_factors_available"),
        report.render.dual_source_blend_factors_available);
    render.insert(
        QStringLiteral("dual_source_blend_factors_runtime_probe"),
        report.render.dual_source_blend_factors_runtime_probe);
    render.insert(
        QStringLiteral("msdf_text_renderer_enabled"),
        report.render.msdf_text_renderer_enabled);
    render.insert(
        QStringLiteral("msdf_text_renderer_compiled"),
        report.render.msdf_text_renderer_compiled);
    render.insert(
        QStringLiteral("msdf_text_renderer_active"),
        report.render.msdf_text_renderer_active);

    QJsonObject cache;
    json_insert_u64(cache, QStringLiteral("lookups"), report.cache.lookups);
    json_insert_u64(cache, QStringLiteral("hits"), report.cache.hits);
    json_insert_u64(cache, QStringLiteral("inserts"), report.cache.inserts);
    json_insert_u64(
        cache,
        QStringLiteral("failed_inserts"),
        report.cache.failed_inserts);
    json_insert_u64(
        cache,
        QStringLiteral("page_bytes"),
        report.cache.page_bytes);
    json_insert_u64(
        cache,
        QStringLiteral("allocated_bytes"),
        report.cache.allocated_bytes);
    json_insert_u64(
        cache,
        QStringLiteral("budget_bytes"),
        report.cache.budget_bytes);
    json_insert_u64(
        cache,
        QStringLiteral("used_bytes"),
        report.cache.used_bytes);
    cache.insert(QStringLiteral("page_count"), report.cache.page_count);
    cache.insert(QStringLiteral("page_budget"), report.cache.page_budget);
    cache.insert(
        QStringLiteral("page_size"),
        json_size_object(report.cache.page_size));

    QJsonObject page_pressure;
    json_insert_u64(
        page_pressure,
        QStringLiteral("render_atlas_failed_inserts"),
        report.render.atlas_failed_inserts);
    json_insert_u64(
        page_pressure,
        QStringLiteral("cache_failed_inserts"),
        report.cache.failed_inserts);
    page_pressure.insert(
        QStringLiteral("frame_build_glyph_atlas_insert_failures"),
        report.frame_build.glyph_atlas_insert_failures);

    QJsonObject object;
    json_insert_u64(
        object,
        QStringLiteral("capture_count"),
        report.capture_count);
    json_insert_u64(
        object,
        QStringLiteral("prepare_count"),
        report.prepare_count);
    json_insert_u64(
        object,
        QStringLiteral("render_count"),
        report.render_count);
    object.insert(QStringLiteral("drew"), report.drew);
    object.insert(
        QStringLiteral("viewport_size"),
        json_size_object(report.viewport_rect.size()));
    object.insert(QStringLiteral("capabilities"), capabilities);
    object.insert(QStringLiteral("frame_build"), frame_build);
    object.insert(QStringLiteral("render"), render);
    object.insert(QStringLiteral("cache"), cache);
    object.insert(
        QStringLiteral("warm_lazy"),
        warm_lazy_summary_object(report.warm_lazy));
    object.insert(QStringLiteral("page_pressure"), page_pressure);
    return object;
}

QJsonObject make_lcd_probe_metadata(
    const Pixel_parity_fixture&                  fixture,
    const Pixel_parity_fixture&                  single_w_fixture,
    const Pixel_parity_fixture&                  repeated_w_fixture,
    const Pixel_parity_fixture&                  ascii_panel_fixture,
    const Pixel_render_result&                   atlas,
    const Pixel_render_result&                   reference,
    const Pixel_render_result&                   qt_text_reference,
    const Pixel_render_result&                   single_w_atlas,
    const Pixel_render_result&                   single_w_raw_atlas_reference,
    const Pixel_render_result&                   single_w_qt_text_reference,
    const Pixel_render_result&                   repeated_w_atlas,
    const Pixel_render_result&                   translated_repeated_w_atlas,
    const Pixel_render_result&                   ascii_panel_atlas,
    const Pixel_render_result&                   ascii_panel_raw_atlas_reference,
    const Pixel_render_result&                   ascii_panel_qt_text_reference,
    QPointF                                      repeated_w_translated_host,
    const Lcd_repeated_w_stability_stats&        translated_repeated_w_stability,
    const Lcd_repeated_w_stability_stats&        translated_w_origin_match,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_stats,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_raw_atlas_reference_stats,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_qt_text_reference_stats,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_user_text_raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_user_text_qt_text_placement,
    const Lcd_ascii_panel_gutter_stats&          ascii_panel_user_text_raw_atlas_gutters,
    const Lcd_ascii_panel_gutter_stats&          ascii_panel_user_text_qt_text_gutters,
    const Msdf_single_w_probe_result&            single_w_msdf,
    const Raster_variant_probe_result&           raster_variants,
    const Metrics_placement_probe_result&        metrics_placement,
    const std::vector<Lcd_glyph_probe_record>&   probe_records,
    const char*                                  backend)
{
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const QFontInfo font_info(font);
    const QRawFont raw_font = QRawFont::fromFont(font);

    QJsonObject font_object;
    font_object.insert(QStringLiteral("requested_family"), font.family());
    font_object.insert(QStringLiteral("actual_family"), font_info.family());
    font_object.insert(QStringLiteral("style"), raw_font.styleName());
    font_object.insert(QStringLiteral("pixel_size"), raw_font.pixelSize());
    font_object.insert(QStringLiteral("point_size"), font.pointSizeF());

    QJsonArray probe_array;
    for (const Lcd_glyph_probe_record& record : probe_records) {
        probe_array.append(lcd_probe_record_object(record));
    }

    QJsonObject images;
    images.insert(
        QStringLiteral("window_logical_size"),
        json_size_object(atlas.window_logical_size));
    images.insert(
        QStringLiteral("fixture_logical_size"),
        json_sizef_object(fixture.logical_size));
    images.insert(
        QStringLiteral("atlas_image_size"),
        json_size_object(atlas.image.size()));
    images.insert(
        QStringLiteral("reference_image_size"),
        json_size_object(reference.image.size()));
    images.insert(
        QStringLiteral("reference_renderer"),
        QStringLiteral("cpu_atlas_rgba_reference"));
    images.insert(
        QStringLiteral("qt_text_reference_image_size"),
        json_size_object(qt_text_reference.image.size()));
    images.insert(
        QStringLiteral("qt_text_reference_renderer"),
        QStringLiteral("qsg_text_node_qt_rendering"));

    QJsonObject graphics;
    graphics.insert(QStringLiteral("requested_backend"), QString::fromLatin1(backend));
    graphics.insert(
        QStringLiteral("qsg_rhi_backend"),
        QString::fromLocal8Bit(qgetenv("QSG_RHI_BACKEND")));
    graphics.insert(
        QStringLiteral("graphics_api"),
        graphics_api_name(atlas.graphics_api));
    graphics.insert(QStringLiteral("software_renderer"), atlas.software_renderer);
    graphics.insert(
        QStringLiteral("driver_info_available"),
        atlas.driver_info_available);
    graphics.insert(
        QStringLiteral("driver_device_name"),
        atlas.driver_device_name);
    json_insert_u64(
        graphics,
        QStringLiteral("driver_device_id"),
        atlas.driver_device_id);
    json_insert_u64(
        graphics,
        QStringLiteral("driver_vendor_id"),
        atlas.driver_vendor_id);
    graphics.insert(
        QStringLiteral("driver_device_type"),
        atlas.driver_device_type);

    QJsonObject build;
    build.insert(
        QStringLiteral("configuration"),
        QStringLiteral(VNM_TERMINAL_TEST_BUILD_TYPE));
    build.insert(
        QStringLiteral("profiling_enabled"),
        static_cast<bool>(VNM_TERMINAL_PROFILING_ENABLED));
    build.insert(
        QStringLiteral("debug_build"),
        k_lcd_probe_debug_build);
    build.insert(
        QStringLiteral("compiler"),
        QString::fromLatin1(k_lcd_probe_compiler));

    QJsonObject capability_interpretation;
    capability_interpretation.insert(
        QStringLiteral("dual_source_blend_probe_method"),
        QStringLiteral("qrhi_dual_output_pipeline_create"));
    capability_interpretation.insert(
        QStringLiteral("dual_source_blend_runtime_probe"),
        atlas.atlas_report.render.dual_source_blend_factors_runtime_probe);
    capability_interpretation.insert(
        QStringLiteral("dual_source_probe_shader_package_available"),
        atlas.atlas_report.render.dual_source_probe_shader_package_available);
    capability_interpretation.insert(
        QStringLiteral("dual_source_blend_available"),
        atlas.atlas_report.render.dual_source_blend_factors_available);

    QJsonObject object;
    object.insert(QStringLiteral("qt_version"), QStringLiteral(QT_VERSION_STR));
    object.insert(QStringLiteral("build"), build);
    object.insert(QStringLiteral("font"), font_object);
    object.insert(QStringLiteral("device_pixel_ratio"), atlas.device_pixel_ratio);
    object.insert(
        QStringLiteral("image_device_pixel_ratio"),
        atlas.image_device_pixel_ratio);
    object.insert(QStringLiteral("graphics"), graphics);
    object.insert(QStringLiteral("images"), images);
    object.insert(
        QStringLiteral("cpu_atlas_reference"),
        lcd_probe_render_result_object(reference));
    object.insert(
        QStringLiteral("qt_text_node_reference"),
        lcd_probe_render_result_object(qt_text_reference));
    object.insert(
        QStringLiteral("qt_text_node_comparison"),
        lcd_probe_qt_text_reference_comparison_object(
            fixture,
            atlas,
            qt_text_reference));
    object.insert(
        QStringLiteral("raster_variant_probe"),
        raster_variant_probe_object(raster_variants));
    object.insert(
        QStringLiteral("single_w_probe"),
        lcd_single_w_probe_object(
            single_w_fixture,
            single_w_atlas,
            single_w_raw_atlas_reference,
            single_w_qt_text_reference,
            single_w_msdf));
    object.insert(
        QStringLiteral("repeated_w_probe"),
        lcd_repeated_w_probe_object(
            repeated_w_fixture,
            repeated_w_atlas));
    object.insert(
        QStringLiteral("translated_repeated_w_probe"),
        lcd_translated_repeated_w_probe_object(
            repeated_w_fixture,
            translated_repeated_w_atlas,
            repeated_w_translated_host,
            translated_repeated_w_stability,
            translated_w_origin_match));
    object.insert(
        QStringLiteral("ascii_panel_probe"),
        lcd_ascii_panel_probe_object(
            ascii_panel_fixture,
            ascii_panel_atlas,
            ascii_panel_raw_atlas_reference,
            ascii_panel_qt_text_reference,
            ascii_panel_stats,
            ascii_panel_raw_atlas_reference_stats,
            ascii_panel_qt_text_reference_stats,
            ascii_panel_raw_atlas_placement,
            ascii_panel_user_text_raw_atlas_placement,
            ascii_panel_user_text_qt_text_placement,
            ascii_panel_user_text_raw_atlas_gutters,
            ascii_panel_user_text_qt_text_gutters));
    object.insert(
        QStringLiteral("metrics_placement_probe"),
        metrics_placement_probe_object(metrics_placement));
    object.insert(QStringLiteral("atlas_report"), lcd_probe_report_object(atlas.atlas_report));
    object.insert(
        QStringLiteral("capability_interpretation"),
        capability_interpretation);
    object.insert(
        QStringLiteral("classifier_source"),
        QStringLiteral("shared_qsg_atlas_classify_glyph_image_candidate_with_presentation"));
    object.insert(QStringLiteral("capability_probe"), probe_array);
    return object;
}

bool save_lcd_probe_image_artifact(
    const QImage&   image,
    const QString&  path,
    const char*     label)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        const QByteArray path_bytes = path.toLocal8Bit();
        std::cerr << "FAIL: LCD atlas probe could not write " << label
            << " artifact " << path_bytes.constData() << ": "
            << file.errorString().toUtf8().constData() << '\n';
        return false;
    }

    if (!image.save(&file, "PNG")) {
        const QByteArray path_bytes = path.toLocal8Bit();
        std::cerr << "FAIL: LCD atlas probe could not encode " << label
            << " artifact " << path_bytes.constData() << '\n';
        return false;
    }

    if (!file.commit()) {
        const QByteArray path_bytes = path.toLocal8Bit();
        std::cerr << "FAIL: LCD atlas probe could not commit " << label
            << " artifact " << path_bytes.constData() << ": "
            << file.errorString().toUtf8().constData() << '\n';
        return false;
    }

    return true;
}

bool write_lcd_probe_artifacts(
    const Pixel_parity_fixture&                  fixture,
    const Pixel_parity_fixture&                  single_w_fixture,
    const Pixel_parity_fixture&                  repeated_w_fixture,
    const Pixel_parity_fixture&                  ascii_panel_fixture,
    const Pixel_render_result&                   atlas,
    const Pixel_render_result&                   reference,
    const Pixel_render_result&                   qt_text_reference,
    const Pixel_render_result&                   single_w_atlas,
    const Pixel_render_result&                   single_w_raw_atlas_reference,
    const Pixel_render_result&                   single_w_qt_text_reference,
    const Pixel_render_result&                   repeated_w_atlas,
    const Pixel_render_result&                   translated_repeated_w_atlas,
    const Pixel_render_result&                   ascii_panel_atlas,
    const Pixel_render_result&                   ascii_panel_raw_atlas_reference,
    const Pixel_render_result&                   ascii_panel_qt_text_reference,
    QPointF                                      repeated_w_translated_host,
    const Lcd_repeated_w_stability_stats&        translated_repeated_w_stability,
    const Lcd_repeated_w_stability_stats&        translated_w_origin_match,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_stats,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_raw_atlas_reference_stats,
    const Lcd_ascii_panel_probe_stats&           ascii_panel_qt_text_reference_stats,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_user_text_raw_atlas_placement,
    const Lcd_ascii_panel_placement_stats&       ascii_panel_user_text_qt_text_placement,
    const Lcd_ascii_panel_gutter_stats&          ascii_panel_user_text_raw_atlas_gutters,
    const Lcd_ascii_panel_gutter_stats&          ascii_panel_user_text_qt_text_gutters,
    const Msdf_single_w_probe_result&            single_w_msdf,
    const Raster_variant_probe_result&           raster_variants,
    const Metrics_placement_probe_result&        metrics_placement,
    const std::vector<Lcd_glyph_probe_record>&   probe_records,
    const char*                                  backend)
{
    const QByteArray artifact_dir_bytes =
        qgetenv("VNM_TERMINAL_LCD_ATLAS_ARTIFACT_DIR");
    if (artifact_dir_bytes.isEmpty()) {
        std::cerr << "FAIL: LCD atlas probe requires "
            << "VNM_TERMINAL_LCD_ATLAS_ARTIFACT_DIR for artifact output\n";
        return false;
    }

    QDir artifact_dir(QString::fromLocal8Bit(artifact_dir_bytes));
    if (!artifact_dir.exists() && !artifact_dir.mkpath(QStringLiteral("."))) {
        std::cerr << "FAIL: LCD atlas probe could not create artifact dir "
            << artifact_dir_bytes.constData() << '\n';
        return false;
    }

    const QString atlas_path =
        artifact_dir.filePath(QStringLiteral("lcd_capability_probe_atlas.png"));
    const QString reference_path =
        artifact_dir.filePath(QStringLiteral("lcd_capability_probe_reference.png"));
    const QString qt_text_reference_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_qt_text_reference.png"));
    const QString single_w_atlas_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_single_w_atlas.png"));
    const QString single_w_raw_atlas_reference_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_single_w_raw_atlas_reference.png"));
    const QString single_w_qt_text_reference_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_single_w_qt_text_reference.png"));
    const QString single_w_msdf_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_single_w_msdf.png"));
    const QString repeated_w_atlas_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_repeated_w_atlas.png"));
    const QString translated_repeated_w_atlas_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_translated_repeated_w_atlas.png"));
    const QString ascii_panel_atlas_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_ascii_panel_atlas.png"));
    const QString ascii_panel_raw_atlas_reference_path =
        artifact_dir.filePath(
            QStringLiteral(
                "lcd_capability_probe_ascii_panel_raw_atlas_reference.png"));
    const QString ascii_panel_qt_text_reference_path =
        artifact_dir.filePath(
            QStringLiteral(
                "lcd_capability_probe_ascii_panel_qt_text_reference.png"));
    const QString raster_variant_path =
        artifact_dir.filePath(
            QStringLiteral("lcd_capability_probe_raster_variants.png"));
    const QString metadata_path =
        artifact_dir.filePath(QStringLiteral("lcd_capability_probe_metadata.json"));

    bool ok = true;
    ok &= save_lcd_probe_image_artifact(
        atlas.image,
        atlas_path,
        "atlas");
    ok &= save_lcd_probe_image_artifact(
        reference.image,
        reference_path,
        "reference");
    ok &= save_lcd_probe_image_artifact(
        qt_text_reference.image,
        qt_text_reference_path,
        "Qt text reference");
    ok &= save_lcd_probe_image_artifact(
        single_w_atlas.image,
        single_w_atlas_path,
        "single-W atlas");
    ok &= save_lcd_probe_image_artifact(
        single_w_raw_atlas_reference.image,
        single_w_raw_atlas_reference_path,
        "single-W raw atlas reference");
    ok &= save_lcd_probe_image_artifact(
        single_w_qt_text_reference.image,
        single_w_qt_text_reference_path,
        "single-W Qt text reference");
    if (single_w_msdf.render.ready) {
        ok &= save_lcd_probe_image_artifact(
            single_w_msdf.render.image,
            single_w_msdf_path,
            "single-W MSDF");
    }
    ok &= save_lcd_probe_image_artifact(
        repeated_w_atlas.image,
        repeated_w_atlas_path,
        "repeated-W atlas");
    ok &= save_lcd_probe_image_artifact(
        translated_repeated_w_atlas.image,
        translated_repeated_w_atlas_path,
        "translated repeated-W atlas");
    ok &= save_lcd_probe_image_artifact(
        ascii_panel_atlas.image,
        ascii_panel_atlas_path,
        "ASCII panel atlas");
    ok &= save_lcd_probe_image_artifact(
        ascii_panel_raw_atlas_reference.image,
        ascii_panel_raw_atlas_reference_path,
        "ASCII panel raw atlas reference");
    ok &= save_lcd_probe_image_artifact(
        ascii_panel_qt_text_reference.image,
        ascii_panel_qt_text_reference_path,
        "ASCII panel Qt text reference");
    ok &= save_lcd_probe_image_artifact(
        raster_variants.sheet,
        raster_variant_path,
        "raster variant probe");

    QSaveFile metadata_file(metadata_path);
    if (!metadata_file.open(QIODevice::WriteOnly)) {
        const QByteArray path = metadata_path.toLocal8Bit();
        std::cerr << "FAIL: LCD atlas probe could not write metadata artifact "
            << path.constData() << ": "
            << metadata_file.errorString().toUtf8().constData() << '\n';
        ok = false;
    }
    else {
        const QJsonObject metadata = make_lcd_probe_metadata(
            fixture,
            single_w_fixture,
            repeated_w_fixture,
            ascii_panel_fixture,
            atlas,
            reference,
            qt_text_reference,
            single_w_atlas,
            single_w_raw_atlas_reference,
            single_w_qt_text_reference,
            repeated_w_atlas,
            translated_repeated_w_atlas,
            ascii_panel_atlas,
            ascii_panel_raw_atlas_reference,
            ascii_panel_qt_text_reference,
            repeated_w_translated_host,
            translated_repeated_w_stability,
            translated_w_origin_match,
            ascii_panel_stats,
            ascii_panel_raw_atlas_reference_stats,
            ascii_panel_qt_text_reference_stats,
            ascii_panel_raw_atlas_placement,
            ascii_panel_user_text_raw_atlas_placement,
            ascii_panel_user_text_qt_text_placement,
            ascii_panel_user_text_raw_atlas_gutters,
            ascii_panel_user_text_qt_text_gutters,
            single_w_msdf,
            raster_variants,
            metrics_placement,
            probe_records,
            backend);
        const QByteArray bytes =
            QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        if (metadata_file.write(bytes) != bytes.size() ||
            !metadata_file.commit())
        {
            const QByteArray path = metadata_path.toLocal8Bit();
            std::cerr << "FAIL: LCD atlas probe could not commit metadata artifact "
                << path.constData() << ": "
                << metadata_file.errorString().toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (ok) {
        const QByteArray atlas_path_bytes = atlas_path.toLocal8Bit();
        const QByteArray reference_path_bytes = reference_path.toLocal8Bit();
        const QByteArray qt_text_reference_path_bytes =
            qt_text_reference_path.toLocal8Bit();
        const QByteArray single_w_atlas_path_bytes =
            single_w_atlas_path.toLocal8Bit();
        const QByteArray single_w_raw_atlas_reference_path_bytes =
            single_w_raw_atlas_reference_path.toLocal8Bit();
        const QByteArray single_w_qt_text_reference_path_bytes =
            single_w_qt_text_reference_path.toLocal8Bit();
        const QByteArray single_w_msdf_path_bytes =
            single_w_msdf_path.toLocal8Bit();
        const QByteArray repeated_w_atlas_path_bytes =
            repeated_w_atlas_path.toLocal8Bit();
        const QByteArray translated_repeated_w_atlas_path_bytes =
            translated_repeated_w_atlas_path.toLocal8Bit();
        const QByteArray ascii_panel_atlas_path_bytes =
            ascii_panel_atlas_path.toLocal8Bit();
        const QByteArray ascii_panel_raw_atlas_reference_path_bytes =
            ascii_panel_raw_atlas_reference_path.toLocal8Bit();
        const QByteArray ascii_panel_qt_text_reference_path_bytes =
            ascii_panel_qt_text_reference_path.toLocal8Bit();
        const QByteArray raster_variant_path_bytes =
            raster_variant_path.toLocal8Bit();
        const QByteArray metadata_path_bytes = metadata_path.toLocal8Bit();
        std::cout << "LCD atlas probe artifacts"
            << " atlas=" << atlas_path_bytes.constData()
            << " reference=" << reference_path_bytes.constData()
            << " qt_text_reference="
            << qt_text_reference_path_bytes.constData()
            << " single_w_atlas="
            << single_w_atlas_path_bytes.constData()
            << " single_w_raw_atlas_reference="
            << single_w_raw_atlas_reference_path_bytes.constData()
            << " single_w_qt_text_reference="
            << single_w_qt_text_reference_path_bytes.constData()
            << " single_w_msdf="
            << (single_w_msdf.render.ready
                ? single_w_msdf_path_bytes.constData()
                : "not-compiled-or-not-ready")
            << " repeated_w_atlas="
            << repeated_w_atlas_path_bytes.constData()
            << " translated_repeated_w_atlas="
            << translated_repeated_w_atlas_path_bytes.constData()
            << " ascii_panel_atlas="
            << ascii_panel_atlas_path_bytes.constData()
            << " ascii_panel_raw_atlas_reference="
            << ascii_panel_raw_atlas_reference_path_bytes.constData()
            << " ascii_panel_qt_text_reference="
            << ascii_panel_qt_text_reference_path_bytes.constData()
            << " raster_variants="
            << raster_variant_path_bytes.constData()
            << " metadata=" << metadata_path_bytes.constData()
            << '\n';
    }

    return ok;
}

void print_lcd_atlas_probe_report(
    const Pixel_render_result& atlas,
    const char*                backend)
{
    const term::Qsg_atlas_frame_report& report = atlas.atlas_report;
    const term::Qsg_atlas_frame_build_summary& frame_build =
        report.frame_build;
    const term::Qsg_atlas_render_summary& render_summary = report.render;
    std::cout << "LCD atlas probe report"
        << " backend=" << backend
        << " graphics_api="
        << graphics_api_name(atlas.graphics_api).toUtf8().constData()
        << " software_renderer=" << atlas.software_renderer
        << " dpr=" << atlas.device_pixel_ratio
        << " image=" << atlas.image.width() << 'x' << atlas.image.height()
        << " prepare=" << report.prepare_count
        << " render=" << report.render_count
        << " drew=" << report.drew
        << " rhi=" << report.rhi_non_null
        << " coverage_texture_created=" << report.coverage_texture_created
        << " coverage_upload_recorded=" << report.coverage_upload_recorded
        << " glyph_misses=" << frame_build.glyph_missed_instances
        << " coverage_failures=" << frame_build.glyph_coverage_failures
        << " atlas_insert_failures="
        << frame_build.glyph_atlas_insert_failures
        << " max_glyph_instance_page="
        << frame_build.max_glyph_instance_page
        << " atlas_page_count=" << render_summary.atlas_page_count
        << " atlas_page_budget=" << render_summary.atlas_page_budget
        << " atlas_page_bytes=" << render_summary.atlas_page_bytes
        << " atlas_allocated_bytes=" << render_summary.atlas_allocated_bytes
        << " atlas_budget_bytes=" << render_summary.atlas_budget_bytes
        << " atlas_used_bytes=" << render_summary.atlas_used_bytes
        << " atlas_failed_inserts=" << render_summary.atlas_failed_inserts
        << " atlas_page_pressure=" << render_summary.atlas_page_pressure
        << " sampler_mode="
        << term::qsg_atlas_sampler_mode_name(
            render_summary.glyph_sampler_mode)
        << " msdf_active="
        << render_summary.msdf_text_renderer_active
        << " msdf_runs=" << render_summary.msdf_text_runs
        << " msdf_instances=" << render_summary.msdf_text_glyph_instances
        << " msdf_draws=" << render_summary.msdf_text_draw_calls
        << " msdf_missed_runs="
        << render_summary.msdf_text_missed_supported_runs
        << " msdf_missed_glyphs="
        << render_summary.msdf_text_missed_supported_glyphs
        << " msdf_sampler_mode="
        << term::qsg_atlas_sampler_mode_name(
            render_summary.msdf_text_sampler_mode)
        << " snapped_origin_failures="
        << frame_build.snapped_origin_failures
        << " grayscale_masks="
        << frame_build.glyph_coverage.grayscale_masks
        << " lcd_rgb_masks=" << frame_build.glyph_coverage.lcd_rgb_masks
        << " lcd_bgr_masks=" << frame_build.glyph_coverage.lcd_bgr_masks
        << " color_images=" << frame_build.glyph_coverage.color_images
        << " ambiguous_images="
        << frame_build.glyph_coverage.ambiguous_images
        << " unsupported_images="
        << frame_build.glyph_coverage.unsupported_images
        << " missed_images=" << frame_build.glyph_coverage.missed_images
        << '\n';
}

int test_lcd_capability_probe(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "LCD atlas capability probe");
    if (backend_status != 0) {
        return backend_status;
    }

    const qreal device_pixel_ratio =
        pixel_probe_render_window_device_pixel_ratio(app);
    Pixel_parity_fixture fixture =
        make_lcd_capability_probe_fixture(device_pixel_ratio);

    Pixel_render_result atlas = render_pixel_atlas_fixture(app, fixture);
    if (!atlas.ready || !atlas_report_render_state_ready(atlas.atlas_report)) {
        std::cerr << "SKIP: LCD atlas capability probe did not reach usable "
            << "QRhi render state on " << backend
            << " prepare_count=" << atlas.atlas_report.prepare_count
            << " render_count=" << atlas.atlas_report.render_count
            << " rhi_non_null=" << atlas.atlas_report.rhi_non_null
            << '\n';
        return k_unsupported_backend_skip_return_code;
    }

    // The LCD capability probe validates the subpixel MSDF text path, which
    // fundamentally depends on dual-source blending. Software/WARP devices
    // (e.g. the GitHub-hosted Windows runner's D3D11 device) do not provide it,
    // so the renderer correctly falls back to the grayscale glyph path and the
    // LCD pixel-parity assertions below do not apply. Treat the backend as
    // unsupported for this probe rather than reporting a failure.
    if (!atlas.atlas_report.render.dual_source_blend_factors_available) {
        std::cerr << "SKIP: LCD atlas capability probe requires dual-source "
            << "blending, which " << backend << " does not provide; the MSDF "
            << "text path falls back to grayscale and the LCD parity checks are "
            << "inapplicable\n";
        return k_unsupported_backend_skip_return_code;
    }

    const std::optional<QSize> reference_size =
        lcd_probe_image_has_pixels(atlas.image)
            ? std::optional<QSize>(atlas.image.size())
            : std::nullopt;
    const Pixel_render_result reference =
        render_atlas_rgba_reference_fixture(fixture, reference_size);
    const Pixel_render_result qt_text_reference =
        render_qsg_text_reference_fixture(app, fixture);
    const Pixel_parity_fixture single_w_fixture =
        make_lcd_single_w_probe_fixture(device_pixel_ratio);
    const Pixel_render_result single_w_atlas =
        render_pixel_atlas_fixture(app, single_w_fixture);
    const Pixel_render_result single_w_raw_atlas_reference =
        render_atlas_rgba_reference_fixture(
            single_w_fixture,
            single_w_atlas.image.size());
    const Pixel_render_result single_w_qt_text_reference =
        render_qsg_text_reference_fixture(app, single_w_fixture);
    const Msdf_single_w_probe_result single_w_msdf =
        render_msdf_single_w_probe_fixture(
            single_w_fixture,
            single_w_atlas.image.size());
    const Pixel_parity_fixture repeated_w_fixture =
        make_lcd_repeated_w_probe_fixture(device_pixel_ratio);
    const Pixel_render_result repeated_w_atlas =
        render_pixel_atlas_fixture(app, repeated_w_fixture);
    const QPointF repeated_w_translated_host(
        k_lcd_repeated_w_translated_host_x,
        k_lcd_repeated_w_translated_host_y);
    const Pixel_render_result translated_repeated_w_atlas =
        render_pixel_atlas_fixture(
            app,
            repeated_w_fixture,
            repeated_w_translated_host);
    const Pixel_parity_fixture contiguous_x_fixture =
        make_lcd_contiguous_x_probe_fixture(device_pixel_ratio);
    const Pixel_render_result contiguous_x_atlas =
        render_pixel_atlas_fixture(
            app,
            contiguous_x_fixture,
            QPointF(),
            k_lcd_contiguous_x_font_size);
    const Pixel_parity_fixture intensity_x_fixture =
        make_lcd_intensity_x_probe_fixture(device_pixel_ratio);
    const Pixel_render_result intensity_x_atlas =
        render_pixel_atlas_fixture(
            app,
            intensity_x_fixture,
            QPointF(),
            k_lcd_intensity_x_font_size);
    const Pixel_parity_fixture fragmented_x_fixture =
        make_lcd_fragmented_x_probe_fixture(device_pixel_ratio);
    const Pixel_render_result fragmented_x_atlas =
        render_pixel_atlas_fixture(
            app,
            fragmented_x_fixture,
            QPointF(),
            k_lcd_fragmented_x_font_size,
            QStringLiteral("monospace"));
    const Pixel_parity_fixture repeated_on_fixture =
        make_lcd_repeated_on_probe_fixture(device_pixel_ratio);
    const QPointF repeated_on_fractional_host =
        lcd_repeated_on_fractional_host(device_pixel_ratio);
    const Pixel_render_result repeated_on_atlas =
        render_pixel_atlas_fixture(
            app,
            repeated_on_fixture,
            repeated_on_fractional_host);
    constexpr qreal k_repeated_on_app_font_size =
        static_cast<qreal>(term::k_vnm_terminal_default_font_pixel_size);
    const Pixel_parity_fixture repeated_on_app_font_fixture =
        make_lcd_repeated_on_probe_fixture(
            device_pixel_ratio,
            k_repeated_on_app_font_size);
    const QPointF repeated_on_app_font_fractional_host =
        lcd_repeated_on_fractional_host(device_pixel_ratio);
    const Pixel_render_result repeated_on_app_font_atlas =
        render_pixel_atlas_fixture(
            app,
            repeated_on_app_font_fixture,
            repeated_on_app_font_fractional_host,
            k_repeated_on_app_font_size);
    const Pixel_parity_fixture repeated_on_live_app_fixture =
        make_lcd_repeated_on_probe_fixture(
            device_pixel_ratio,
            k_lcd_repeated_on_live_app_font_size);
    const QPointF repeated_on_live_app_host =
        lcd_repeated_on_live_app_host(device_pixel_ratio);
    const Pixel_render_result repeated_on_live_app_atlas =
        render_pixel_atlas_fixture(
            app,
            repeated_on_live_app_fixture,
            repeated_on_live_app_host,
            k_lcd_repeated_on_live_app_font_size);
    const Pixel_parity_fixture ascii_panel_fixture =
        make_lcd_ascii_panel_probe_fixture(device_pixel_ratio);
    const Pixel_render_result ascii_panel_atlas =
        render_pixel_atlas_fixture(app, ascii_panel_fixture);
    const Pixel_render_result ascii_panel_raw_atlas_reference =
        render_atlas_rgba_reference_fixture(
            ascii_panel_fixture,
            ascii_panel_atlas.image.size());
    const Pixel_render_result ascii_panel_qt_text_reference =
        render_qsg_text_reference_fixture(app, ascii_panel_fixture);
    const Raster_variant_probe_result raster_variants =
        render_lcd_raster_variant_probe(fixture.device_pixel_ratio);
    const Metrics_placement_probe_result metrics_placement =
        render_lcd_metrics_placement_probe(fixture.device_pixel_ratio);
    const QFont font = term::vnm_terminal_font(QString(), 18.0);
    const std::vector<Lcd_glyph_probe_record> probe_records =
        probe_lcd_subpixel_glyphs(font, atlas.device_pixel_ratio);
    const Pixel_glyph_stats qt_text_glyphs = compare_glyph_regions(
        qt_text_reference.image,
        atlas.image,
        fixture.glyph_masks,
        fixture.device_pixel_ratio);
    const Pixel_glyph_stats single_w_qt_text_glyphs = compare_glyph_regions(
        single_w_qt_text_reference.image,
        single_w_atlas.image,
        single_w_fixture.glyph_masks,
        single_w_fixture.device_pixel_ratio);
    const Pixel_glyph_stats single_w_msdf_raw_glyphs = compare_glyph_regions(
        single_w_raw_atlas_reference.image,
        single_w_msdf.render.image,
        single_w_fixture.glyph_masks,
        single_w_fixture.device_pixel_ratio);
    const Pixel_glyph_stats single_w_msdf_qt_text_glyphs = compare_glyph_regions(
        single_w_qt_text_reference.image,
        single_w_msdf.render.image,
        single_w_fixture.glyph_masks,
        single_w_fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats single_w_atlas_first_column =
        measure_lcd_first_ink_column(
            single_w_atlas.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats single_w_raw_atlas_first_column =
        measure_lcd_first_ink_column(
            single_w_raw_atlas_reference.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats single_w_qt_text_first_column =
        measure_lcd_first_ink_column(
            single_w_qt_text_reference.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio);
    const Lcd_first_ink_column_stats single_w_msdf_first_column =
        measure_lcd_first_ink_column(
            single_w_msdf.render.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio);
    const Lcd_horizontal_edge_transition_stats single_w_atlas_edges =
        measure_lcd_horizontal_edge_transitions(
            single_w_atlas.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio,
            single_w_atlas_first_column);
    const Lcd_horizontal_edge_transition_stats single_w_raw_atlas_edges =
        measure_lcd_horizontal_edge_transitions(
            single_w_raw_atlas_reference.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio,
            single_w_raw_atlas_first_column);
    const Lcd_horizontal_edge_transition_stats single_w_qt_text_edges =
        measure_lcd_horizontal_edge_transitions(
            single_w_qt_text_reference.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio,
            single_w_qt_text_first_column);
    const Lcd_horizontal_edge_transition_stats single_w_msdf_edges =
        measure_lcd_horizontal_edge_transitions(
            single_w_msdf.render.image,
            single_w_fixture.glyph_masks,
            single_w_fixture.device_pixel_ratio,
            single_w_msdf_first_column);
    const double single_w_atlas_msdf_ink_ratio = lcd_ink_pixel_ratio(
        single_w_atlas_first_column.ink_pixels,
        single_w_msdf_first_column.ink_pixels);
    const Lcd_repeated_w_stability_stats repeated_w_stability =
        measure_lcd_repeated_w_stability(
            repeated_w_fixture,
            repeated_w_atlas.image,
            k_lcd_repeated_w_row,
            k_lcd_repeated_w_first_column,
            k_lcd_repeated_w_column_stride,
            k_lcd_repeated_w_cell_count);
    const Lcd_repeated_w_stability_stats translated_repeated_w_stability =
        measure_lcd_repeated_w_stability(
            repeated_w_fixture,
            translated_repeated_w_atlas.image,
            k_lcd_repeated_w_row,
            k_lcd_repeated_w_first_column,
            k_lcd_repeated_w_column_stride,
            k_lcd_repeated_w_cell_count,
            repeated_w_translated_host);
    const Lcd_repeated_w_stability_stats translated_w_origin_match =
        compare_lcd_repeated_w_corresponding_cells(
            repeated_w_fixture,
            repeated_w_atlas.image,
            repeated_w_stability,
            translated_repeated_w_atlas.image,
            translated_repeated_w_stability);
    const Lcd_repeated_w_stability_stats contiguous_x_stability =
        measure_lcd_repeated_w_stability(
            contiguous_x_fixture,
            contiguous_x_atlas.image,
            k_lcd_contiguous_x_row,
            k_lcd_contiguous_x_first_column,
            k_lcd_contiguous_x_column_stride,
            k_lcd_contiguous_x_cell_count);
    const std::vector<Lcd_intensity_x_row_stability> intensity_x_stability =
        measure_lcd_intensity_x_stability(
            intensity_x_fixture,
            intensity_x_atlas.image);
    const Lcd_repeated_w_stability_stats fragmented_x_stability =
        measure_lcd_repeated_w_stability(
            fragmented_x_fixture,
            fragmented_x_atlas.image,
            k_lcd_fragmented_x_row,
            k_lcd_fragmented_x_first_column,
            k_lcd_fragmented_x_column_stride,
            k_lcd_fragmented_x_columns);
    const Lcd_repeated_on_stability_stats repeated_on_stability =
        measure_lcd_repeated_on_stability(
            repeated_on_fixture,
            repeated_on_atlas.image,
            repeated_on_fractional_host);
    const Lcd_repeated_on_stability_stats repeated_on_app_font_stability =
        measure_lcd_repeated_on_stability(
            repeated_on_app_font_fixture,
            repeated_on_app_font_atlas.image,
            repeated_on_app_font_fractional_host);
    const Lcd_repeated_on_stability_stats repeated_on_live_app_stability =
        measure_lcd_repeated_on_stability(
            repeated_on_live_app_fixture,
            repeated_on_live_app_atlas.image,
            repeated_on_live_app_host);
    const Lcd_ascii_panel_probe_stats ascii_panel_stats =
        measure_lcd_ascii_panel_probe(
            ascii_panel_fixture,
            ascii_panel_atlas.image);
    const Lcd_ascii_panel_probe_stats ascii_panel_raw_atlas_reference_stats =
        measure_lcd_ascii_panel_probe(
            ascii_panel_fixture,
            ascii_panel_raw_atlas_reference.image);
    const Lcd_ascii_panel_probe_stats ascii_panel_qt_text_reference_stats =
        measure_lcd_ascii_panel_probe(
            ascii_panel_fixture,
            ascii_panel_qt_text_reference.image);
    const Lcd_ascii_panel_placement_stats ascii_panel_raw_atlas_placement =
        compare_lcd_ascii_panel_cell_placement(
            ascii_panel_raw_atlas_reference_stats,
            ascii_panel_stats,
            lcd_ascii_panel_placement_probe_labels());
    const Lcd_ascii_panel_placement_stats
        ascii_panel_cursor_text_raw_atlas_placement =
            compare_lcd_ascii_panel_cell_placement(
                ascii_panel_raw_atlas_reference_stats,
                ascii_panel_stats,
                lcd_ascii_panel_cursor_text_placement_probe_labels());
    const Lcd_ascii_panel_placement_stats
        ascii_panel_user_text_raw_atlas_placement =
            compare_lcd_ascii_panel_cell_placement(
                ascii_panel_raw_atlas_reference_stats,
                ascii_panel_stats,
                lcd_ascii_panel_user_text_placement_probe_labels());
    const Lcd_ascii_panel_placement_stats
        ascii_panel_user_text_qt_text_placement =
            compare_lcd_ascii_panel_cell_placement(
                ascii_panel_qt_text_reference_stats,
                ascii_panel_stats,
                lcd_ascii_panel_user_text_visible_baseline_probe_labels());
    const Lcd_ascii_panel_gutter_stats
        ascii_panel_user_text_raw_atlas_gutters =
            compare_lcd_ascii_panel_visible_gutters(
                ascii_panel_raw_atlas_reference_stats,
                ascii_panel_stats,
                lcd_ascii_panel_user_text_targeted_visible_gutter_pairs());
    const Lcd_ascii_panel_gutter_stats
        ascii_panel_user_text_qt_text_gutters =
            compare_lcd_ascii_panel_visible_gutters(
                ascii_panel_qt_text_reference_stats,
                ascii_panel_stats,
                lcd_ascii_panel_user_text_targeted_visible_gutter_pairs());
    const Lcd_ascii_panel_cell_stats* const ascii_panel_m_cell =
        lcd_ascii_panel_cell(
            ascii_panel_stats,
            QStringLiteral("M"));
    const Lcd_ascii_panel_cell_stats* const ascii_panel_lower_m_cell =
        lcd_ascii_panel_cell(
            ascii_panel_stats,
            QStringLiteral("m"));
    const Lcd_ascii_panel_cell_stats* const ascii_panel_e_cell =
        lcd_ascii_panel_cell(
            ascii_panel_stats,
            QStringLiteral("e"));
    const Lcd_ascii_panel_cell_stats* const ascii_panel_l_cell =
        lcd_ascii_panel_cell(
            ascii_panel_stats,
            QStringLiteral("L"));
    const auto& repeated_w_render =
        repeated_w_atlas.atlas_report.render;
    const auto& translated_repeated_w_render =
        translated_repeated_w_atlas.atlas_report.render;
    const auto& contiguous_x_render =
        contiguous_x_atlas.atlas_report.render;
    const auto& intensity_x_render =
        intensity_x_atlas.atlas_report.render;
    const auto& fragmented_x_render =
        fragmented_x_atlas.atlas_report.render;
    const auto& repeated_on_render =
        repeated_on_atlas.atlas_report.render;
    const auto& repeated_on_app_font_render =
        repeated_on_app_font_atlas.atlas_report.render;
    const auto& repeated_on_live_app_render =
        repeated_on_live_app_atlas.atlas_report.render;
    const auto& ascii_panel_render =
        ascii_panel_atlas.atlas_report.render;

    print_lcd_atlas_probe_report(atlas, backend);
    std::cout << "LCD atlas Qt text-node reference"
        << " ready=" << qt_text_reference.ready
        << " image=" << qt_text_reference.image.width()
        << 'x' << qt_text_reference.image.height()
        << " render_dpr=" << qt_text_reference.device_pixel_ratio
        << " image_dpr=" << qt_text_reference.image_device_pixel_ratio
        << " glyph_compared_pixels=" << qt_text_glyphs.compared_pixels
        << " glyph_diff_pixels=" << qt_text_glyphs.diff_pixels
        << " glyph_max_delta=" << qt_text_glyphs.max_delta
        << " qt_text_ink_pixels=" << qt_text_glyphs.reference_ink_pixels
        << " atlas_ink_pixels=" << qt_text_glyphs.atlas_ink_pixels
        << '\n';
    std::cout << "LCD atlas single-W Qt text-node reference"
        << " atlas_ready=" << single_w_atlas.ready
        << " raw_atlas_ready=" << single_w_raw_atlas_reference.ready
        << " qt_text_ready=" << single_w_qt_text_reference.ready
        << " atlas_image=" << single_w_atlas.image.width()
        << 'x' << single_w_atlas.image.height()
        << " raw_atlas_image=" << single_w_raw_atlas_reference.image.width()
        << 'x' << single_w_raw_atlas_reference.image.height()
        << " qt_text_image=" << single_w_qt_text_reference.image.width()
        << 'x' << single_w_qt_text_reference.image.height()
        << " glyph_compared_pixels=" << single_w_qt_text_glyphs.compared_pixels
        << " glyph_diff_pixels=" << single_w_qt_text_glyphs.diff_pixels
        << " glyph_max_delta=" << single_w_qt_text_glyphs.max_delta
        << " qt_text_ink_pixels="
        << single_w_qt_text_glyphs.reference_ink_pixels
        << " atlas_ink_pixels=" << single_w_qt_text_glyphs.atlas_ink_pixels
        << '\n';
    std::cout << "LCD atlas single-W MSDF diagnostic"
        << " compiled=" << single_w_msdf.compiled
        << " atlas_built=" << single_w_msdf.atlas_built
        << " ready=" << single_w_msdf.render.ready
        << " image=" << single_w_msdf.render.image.width()
        << 'x' << single_w_msdf.render.image.height()
        << " raw_compared_pixels=" << single_w_msdf_raw_glyphs.compared_pixels
        << " raw_diff_pixels=" << single_w_msdf_raw_glyphs.diff_pixels
        << " raw_max_delta=" << single_w_msdf_raw_glyphs.max_delta
        << " qt_text_compared_pixels="
        << single_w_msdf_qt_text_glyphs.compared_pixels
        << " qt_text_diff_pixels=" << single_w_msdf_qt_text_glyphs.diff_pixels
        << " qt_text_max_delta=" << single_w_msdf_qt_text_glyphs.max_delta
        << " message=" << single_w_msdf.message.toUtf8().constData()
        << '\n';
    std::cout << "LCD atlas single-W first-ink-column"
        << " atlas_ink=" << single_w_atlas_first_column.ink_pixels
        << " atlas_rows=" << single_w_atlas_first_column.first_column_rows
        << " atlas_distinct="
        << single_w_atlas_first_column.distinct_first_column_rgb_colors
        << " atlas_adjacent_equal="
        << single_w_atlas_first_column.adjacent_equal_first_column_pairs
        << " atlas_longest_equal_run="
        << single_w_atlas_first_column.longest_equal_first_column_run
        << " raw_ink=" << single_w_raw_atlas_first_column.ink_pixels
        << " raw_rows=" << single_w_raw_atlas_first_column.first_column_rows
        << " raw_distinct="
        << single_w_raw_atlas_first_column.distinct_first_column_rgb_colors
        << " raw_adjacent_equal="
        << single_w_raw_atlas_first_column.adjacent_equal_first_column_pairs
        << " raw_longest_equal_run="
        << single_w_raw_atlas_first_column.longest_equal_first_column_run
        << " qt_rows=" << single_w_qt_text_first_column.first_column_rows
        << " qt_distinct="
        << single_w_qt_text_first_column.distinct_first_column_rgb_colors
        << " qt_adjacent_equal="
        << single_w_qt_text_first_column.adjacent_equal_first_column_pairs
        << " qt_longest_equal_run="
        << single_w_qt_text_first_column.longest_equal_first_column_run
        << " cpu_msdf_rows="
        << single_w_msdf_first_column.first_column_rows
        << " cpu_msdf_distinct="
        << single_w_msdf_first_column.distinct_first_column_rgb_colors
        << " cpu_msdf_adjacent_equal="
        << single_w_msdf_first_column.adjacent_equal_first_column_pairs
        << " cpu_msdf_longest_equal_run="
        << single_w_msdf_first_column.longest_equal_first_column_run
        << '\n';
    std::cout << "LCD atlas single-W MSDF edge quality"
        << " atlas_bbox=" << single_w_atlas_first_column.bbox.x() << ','
        << single_w_atlas_first_column.bbox.y() << ' '
        << single_w_atlas_first_column.bbox.width() << 'x'
        << single_w_atlas_first_column.bbox.height()
        << " cpu_msdf_bbox=" << single_w_msdf_first_column.bbox.x() << ','
        << single_w_msdf_first_column.bbox.y() << ' '
        << single_w_msdf_first_column.bbox.width() << 'x'
        << single_w_msdf_first_column.bbox.height()
        << " atlas_msdf_ink_ratio=" << single_w_atlas_msdf_ink_ratio
        << " atlas_edge_rows=" << single_w_atlas_edges.rows_with_transitions
        << " atlas_edge_count=" << single_w_atlas_edges.transition_count
        << " atlas_edge_max=" << single_w_atlas_edges.max_transition_width
        << " atlas_edge_avg=" << single_w_atlas_edges.average_transition_width
        << " raw_edge_max=" << single_w_raw_atlas_edges.max_transition_width
        << " raw_edge_avg=" << single_w_raw_atlas_edges.average_transition_width
        << " qt_edge_max=" << single_w_qt_text_edges.max_transition_width
        << " qt_edge_avg=" << single_w_qt_text_edges.average_transition_width
        << " cpu_msdf_edge_rows="
        << single_w_msdf_edges.rows_with_transitions
        << " cpu_msdf_edge_count=" << single_w_msdf_edges.transition_count
        << " cpu_msdf_edge_max=" << single_w_msdf_edges.max_transition_width
        << " cpu_msdf_edge_avg="
        << single_w_msdf_edges.average_transition_width
        << '\n';
    std::cout << "LCD atlas repeated-W stability"
        << " ready=" << repeated_w_atlas.ready
        << " image=" << repeated_w_atlas.image.width()
        << 'x' << repeated_w_atlas.image.height()
        << " msdf_active=" << repeated_w_render.msdf_text_renderer_active
        << " msdf_instances=" << repeated_w_render.msdf_text_glyph_instances
        << " msdf_draws=" << repeated_w_render.msdf_text_draw_calls
        << " glyph_draws=" << repeated_w_render.glyph_draw_calls
        << " glyph_instances=" << repeated_w_render.glyph_buffer_instances
        << " column_stride=" << k_lcd_repeated_w_column_stride
        << " physical_cell=" << repeated_w_stability.physical_cell_width
        << 'x' << repeated_w_stability.physical_cell_height
        << " ascent=" << repeated_w_stability.physical_cell_ascent
        << " integer_deltas="
        << repeated_w_stability.cell_width_integer_delta
        << ',' << repeated_w_stability.cell_height_integer_delta
        << ',' << repeated_w_stability.cell_ascent_integer_delta
        << " compared_cells=" << repeated_w_stability.compared_cells
        << " compared_pixels=" << repeated_w_stability.compared_pixels
        << " diff_pixels=" << repeated_w_stability.diff_pixels
        << " max_delta=" << repeated_w_stability.max_delta
        << " max_diff_per_cell="
        << repeated_w_stability.max_diff_pixels_per_cell
        << " ink_mask_diff_pixels="
        << repeated_w_stability.ink_mask_diff_pixels
        << " max_ink_mask_diff_per_cell="
        << repeated_w_stability.max_ink_mask_diff_pixels_per_cell
        << " all_ink=" << repeated_w_stability.all_cells_have_ink
        << " max_ink_delta="
        << repeated_w_stability.max_ink_pixels_delta
        << " max_rel_first_x_delta="
        << repeated_w_stability.max_relative_first_ink_x_delta
        << " max_rel_bbox_x_delta="
        << repeated_w_stability.max_relative_bbox_x_delta
        << '\n';
    std::cout << "LCD atlas translated repeated-W stability"
        << " ready=" << translated_repeated_w_atlas.ready
        << " image=" << translated_repeated_w_atlas.image.width()
        << 'x' << translated_repeated_w_atlas.image.height()
        << " host_translation=" << repeated_w_translated_host.x()
        << ',' << repeated_w_translated_host.y()
        << " msdf_active="
        << translated_repeated_w_render.msdf_text_renderer_active
        << " msdf_instances="
        << translated_repeated_w_render.msdf_text_glyph_instances
        << " msdf_draws="
        << translated_repeated_w_render.msdf_text_draw_calls
        << " glyph_draws=" << translated_repeated_w_render.glyph_draw_calls
        << " glyph_instances=" << translated_repeated_w_render.glyph_buffer_instances
        << " compared_cells="
        << translated_repeated_w_stability.compared_cells
        << " compared_pixels="
        << translated_repeated_w_stability.compared_pixels
        << " diff_pixels=" << translated_repeated_w_stability.diff_pixels
        << " max_delta=" << translated_repeated_w_stability.max_delta
        << " max_diff_per_cell="
        << translated_repeated_w_stability.max_diff_pixels_per_cell
        << " ink_mask_diff_pixels="
        << translated_repeated_w_stability.ink_mask_diff_pixels
        << " max_ink_mask_diff_per_cell="
        << translated_repeated_w_stability.max_ink_mask_diff_pixels_per_cell
        << " all_ink=" << translated_repeated_w_stability.all_cells_have_ink
        << " max_ink_delta="
        << translated_repeated_w_stability.max_ink_pixels_delta
        << " max_rel_first_x_delta="
        << translated_repeated_w_stability.max_relative_first_ink_x_delta
        << " max_rel_bbox_x_delta="
        << translated_repeated_w_stability.max_relative_bbox_x_delta
        << '\n';
    std::cout << "LCD atlas translated repeated-W origin match"
        << " compared_cells="
        << translated_w_origin_match.compared_cells
        << " compared_pixels="
        << translated_w_origin_match.compared_pixels
        << " diff_pixels=" << translated_w_origin_match.diff_pixels
        << " max_delta=" << translated_w_origin_match.max_delta
        << " max_diff_per_cell="
        << translated_w_origin_match.max_diff_pixels_per_cell
        << " ink_mask_diff_pixels="
        << translated_w_origin_match.ink_mask_diff_pixels
        << " max_ink_mask_diff_per_cell="
        << translated_w_origin_match.max_ink_mask_diff_pixels_per_cell
        << " all_ink="
        << translated_w_origin_match.all_cells_have_ink
        << " max_ink_delta="
        << translated_w_origin_match.max_ink_pixels_delta
        << " max_rel_first_x_delta="
        << translated_w_origin_match.max_relative_first_ink_x_delta
        << " max_rel_bbox_x_delta="
        << translated_w_origin_match.max_relative_bbox_x_delta
        << '\n';
    std::cout << "LCD atlas contiguous-X stability"
        << " ready=" << contiguous_x_atlas.ready
        << " image=" << contiguous_x_atlas.image.width()
        << 'x' << contiguous_x_atlas.image.height()
        << " font_size=" << k_lcd_contiguous_x_font_size
        << " msdf_active="
        << contiguous_x_render.msdf_text_renderer_active
        << " msdf_runs=" << contiguous_x_render.msdf_text_runs
        << " msdf_instances="
        << contiguous_x_render.msdf_text_glyph_instances
        << " msdf_draws=" << contiguous_x_render.msdf_text_draw_calls
        << " glyph_draws=" << contiguous_x_render.glyph_draw_calls
        << " glyph_instances="
        << contiguous_x_render.glyph_buffer_instances
        << " column_stride=" << k_lcd_contiguous_x_column_stride
        << " physical_cell=" << contiguous_x_stability.physical_cell_width
        << 'x' << contiguous_x_stability.physical_cell_height
        << " ascent=" << contiguous_x_stability.physical_cell_ascent
        << " integer_deltas="
        << contiguous_x_stability.cell_width_integer_delta
        << ',' << contiguous_x_stability.cell_height_integer_delta
        << ',' << contiguous_x_stability.cell_ascent_integer_delta
        << " compared_cells=" << contiguous_x_stability.compared_cells
        << " compared_pixels=" << contiguous_x_stability.compared_pixels
        << " diff_pixels=" << contiguous_x_stability.diff_pixels
        << " max_delta=" << contiguous_x_stability.max_delta
        << " max_diff_per_cell="
        << contiguous_x_stability.max_diff_pixels_per_cell
        << " ink_mask_diff_pixels="
        << contiguous_x_stability.ink_mask_diff_pixels
        << " max_ink_mask_diff_per_cell="
        << contiguous_x_stability.max_ink_mask_diff_pixels_per_cell
        << " all_ink=" << contiguous_x_stability.all_cells_have_ink
        << " max_ink_delta="
        << contiguous_x_stability.max_ink_pixels_delta
        << " max_rel_first_x_delta="
        << contiguous_x_stability.max_relative_first_ink_x_delta
        << " max_rel_bbox_x_delta="
        << contiguous_x_stability.max_relative_bbox_x_delta
        << '\n';
    std::cout << "LCD atlas intensity-X stability"
        << " ready=" << intensity_x_atlas.ready
        << " image=" << intensity_x_atlas.image.width()
        << 'x' << intensity_x_atlas.image.height()
        << " font_size=" << k_lcd_intensity_x_font_size
        << " msdf_active="
        << intensity_x_render.msdf_text_renderer_active
        << " msdf_runs=" << intensity_x_render.msdf_text_runs
        << " msdf_instances="
        << intensity_x_render.msdf_text_glyph_instances
        << " msdf_draws=" << intensity_x_render.msdf_text_draw_calls
        << " glyph_draws=" << intensity_x_render.glyph_draw_calls
        << " glyph_instances="
        << intensity_x_render.glyph_buffer_instances
        << " rows=" << k_lcd_intensity_x_rows
        << " columns=" << k_lcd_intensity_x_columns
        << '\n';
    for (const Lcd_intensity_x_row_stability& row : intensity_x_stability) {
        const QColor foreground = QColor::fromRgba(row.foreground_rgba);
        const Lcd_repeated_w_stability_stats& stability = row.stability;
        std::cout << "LCD atlas intensity-X row"
            << " row_index=" << row.row_index
            << " row=" << row.row
            << " glyph=" << row.glyph
            << " foreground_rgb=" << foreground.red()
            << ',' << foreground.green()
            << ',' << foreground.blue()
            << " compared_cells=" << stability.compared_cells
            << " compared_pixels=" << stability.compared_pixels
            << " diff_pixels=" << stability.diff_pixels
            << " max_delta=" << stability.max_delta
            << " max_diff_per_cell="
            << stability.max_diff_pixels_per_cell
            << " ink_mask_diff_pixels="
            << stability.ink_mask_diff_pixels
            << " max_ink_mask_diff_per_cell="
            << stability.max_ink_mask_diff_pixels_per_cell
            << " all_ink=" << stability.all_cells_have_ink
            << " max_ink_delta=" << stability.max_ink_pixels_delta
            << " max_rel_first_x_delta="
            << stability.max_relative_first_ink_x_delta
            << " max_rel_bbox_x_delta="
            << stability.max_relative_bbox_x_delta
            << '\n';
    }
    std::cout << "LCD atlas fragmented-X shaped stability"
        << " ready=" << fragmented_x_atlas.ready
        << " image=" << fragmented_x_atlas.image.width()
        << 'x' << fragmented_x_atlas.image.height()
        << " font_size=" << k_lcd_fragmented_x_font_size
        << " msdf_active="
        << fragmented_x_render.msdf_text_renderer_active
        << " msdf_runs=" << fragmented_x_render.msdf_text_runs
        << " msdf_instances="
        << fragmented_x_render.msdf_text_glyph_instances
        << " msdf_draws=" << fragmented_x_render.msdf_text_draw_calls
        << " glyph_draws=" << fragmented_x_render.glyph_draw_calls
        << " glyph_instances="
        << fragmented_x_render.glyph_buffer_instances
        << " shaped_runs="
        << fragmented_x_atlas.atlas_report.producer.shaped_runs_built +
            fragmented_x_atlas.atlas_report.producer.shaped_runs_reused
        << " simple_used="
        << fragmented_x_atlas.atlas_report.producer.simple_path_used
        << " columns=" << k_lcd_fragmented_x_columns
        << " compared_cells=" << fragmented_x_stability.compared_cells
        << " compared_pixels=" << fragmented_x_stability.compared_pixels
        << " diff_pixels=" << fragmented_x_stability.diff_pixels
        << " max_delta=" << fragmented_x_stability.max_delta
        << " max_diff_per_cell="
        << fragmented_x_stability.max_diff_pixels_per_cell
        << " ink_mask_diff_pixels="
        << fragmented_x_stability.ink_mask_diff_pixels
        << " max_ink_mask_diff_per_cell="
        << fragmented_x_stability.max_ink_mask_diff_pixels_per_cell
        << " all_ink=" << fragmented_x_stability.all_cells_have_ink
        << " max_ink_delta="
        << fragmented_x_stability.max_ink_pixels_delta
        << " max_rel_first_x_delta="
        << fragmented_x_stability.max_relative_first_ink_x_delta
        << " max_rel_bbox_x_delta="
        << fragmented_x_stability.max_relative_bbox_x_delta
        << '\n';
    std::cout << "LCD atlas repeated-on fractional-host stability"
        << " ready=" << repeated_on_atlas.ready
        << " image=" << repeated_on_atlas.image.width()
        << 'x' << repeated_on_atlas.image.height()
        << " host_translation=" << repeated_on_fractional_host.x()
        << ',' << repeated_on_fractional_host.y()
        << " msdf_active="
        << repeated_on_render.msdf_text_renderer_active
        << " msdf_instances="
        << repeated_on_render.msdf_text_glyph_instances
        << " msdf_draws=" << repeated_on_render.msdf_text_draw_calls
        << " glyph_draws=" << repeated_on_render.glyph_draw_calls
        << " glyph_instances=" << repeated_on_render.glyph_buffer_instances
        << " physical_cell=" << repeated_on_stability.physical_cell_width
        << 'x' << repeated_on_stability.physical_cell_height
        << " ascent=" << repeated_on_stability.physical_cell_ascent
        << " expected_glyphs=" << repeated_on_stability.expected_glyphs
        << " visible_glyphs=" << repeated_on_stability.visible_glyphs
        << " compared_pairs="
        << repeated_on_stability.compared_same_glyph_pairs
        << " first_gutter=" << repeated_on_stability.first_word_gutter
        << " second_gutter=" << repeated_on_stability.second_word_gutter
        << " gutter_delta=" << repeated_on_stability.gutter_delta
        << " max_visible_left_delta="
        << repeated_on_stability.max_visible_left_delta
        << " max_visible_right_delta="
        << repeated_on_stability.max_visible_right_delta
        << " max_visible_top_delta="
        << repeated_on_stability.max_visible_top_delta
        << " max_visible_baseline_delta="
        << repeated_on_stability.max_visible_baseline_delta
        << " max_center_delta="
        << repeated_on_stability.max_weighted_center_x_delta
        << ',' << repeated_on_stability.max_weighted_center_y_delta
        << '\n';
    std::cout << "LCD atlas repeated-on app-font fractional-host stability"
        << " ready=" << repeated_on_app_font_atlas.ready
        << " image=" << repeated_on_app_font_atlas.image.width()
        << 'x' << repeated_on_app_font_atlas.image.height()
        << " font_size=" << k_repeated_on_app_font_size
        << " host_translation=" << repeated_on_app_font_fractional_host.x()
        << ',' << repeated_on_app_font_fractional_host.y()
        << " msdf_active="
        << repeated_on_app_font_render.msdf_text_renderer_active
        << " msdf_instances="
        << repeated_on_app_font_render.msdf_text_glyph_instances
        << " msdf_draws=" << repeated_on_app_font_render.msdf_text_draw_calls
        << " glyph_draws=" << repeated_on_app_font_render.glyph_draw_calls
        << " glyph_instances="
        << repeated_on_app_font_render.glyph_buffer_instances
        << " physical_cell="
        << repeated_on_app_font_stability.physical_cell_width
        << 'x' << repeated_on_app_font_stability.physical_cell_height
        << " ascent=" << repeated_on_app_font_stability.physical_cell_ascent
        << " expected_glyphs="
        << repeated_on_app_font_stability.expected_glyphs
        << " visible_glyphs="
        << repeated_on_app_font_stability.visible_glyphs
        << " compared_pairs="
        << repeated_on_app_font_stability.compared_same_glyph_pairs
        << " first_gutter="
        << repeated_on_app_font_stability.first_word_gutter
        << " second_gutter="
        << repeated_on_app_font_stability.second_word_gutter
        << " gutter_delta=" << repeated_on_app_font_stability.gutter_delta
        << " max_visible_left_delta="
        << repeated_on_app_font_stability.max_visible_left_delta
        << " max_visible_right_delta="
        << repeated_on_app_font_stability.max_visible_right_delta
        << " max_visible_top_delta="
        << repeated_on_app_font_stability.max_visible_top_delta
        << " max_visible_baseline_delta="
        << repeated_on_app_font_stability.max_visible_baseline_delta
        << " max_center_delta="
        << repeated_on_app_font_stability.max_weighted_center_x_delta
        << ',' << repeated_on_app_font_stability.max_weighted_center_y_delta
        << '\n';
    std::cout << "LCD atlas repeated-on live-app stability"
        << " ready=" << repeated_on_live_app_atlas.ready
        << " image=" << repeated_on_live_app_atlas.image.width()
        << 'x' << repeated_on_live_app_atlas.image.height()
        << " font_size=" << k_lcd_repeated_on_live_app_font_size
        << " host_translation=" << repeated_on_live_app_host.x()
        << ',' << repeated_on_live_app_host.y()
        << " msdf_active="
        << repeated_on_live_app_render.msdf_text_renderer_active
        << " msdf_instances="
        << repeated_on_live_app_render.msdf_text_glyph_instances
        << " msdf_draws=" << repeated_on_live_app_render.msdf_text_draw_calls
        << " glyph_draws=" << repeated_on_live_app_render.glyph_draw_calls
        << " glyph_instances="
        << repeated_on_live_app_render.glyph_buffer_instances
        << " physical_cell="
        << repeated_on_live_app_stability.physical_cell_width
        << 'x' << repeated_on_live_app_stability.physical_cell_height
        << " ascent=" << repeated_on_live_app_stability.physical_cell_ascent
        << " expected_glyphs="
        << repeated_on_live_app_stability.expected_glyphs
        << " visible_glyphs="
        << repeated_on_live_app_stability.visible_glyphs
        << " compared_pairs="
        << repeated_on_live_app_stability.compared_same_glyph_pairs
        << " first_gutter="
        << repeated_on_live_app_stability.first_word_gutter
        << " second_gutter="
        << repeated_on_live_app_stability.second_word_gutter
        << " gutter_delta=" << repeated_on_live_app_stability.gutter_delta
        << " max_visible_left_delta="
        << repeated_on_live_app_stability.max_visible_left_delta
        << " max_visible_right_delta="
        << repeated_on_live_app_stability.max_visible_right_delta
        << " max_visible_top_delta="
        << repeated_on_live_app_stability.max_visible_top_delta
        << " max_visible_baseline_delta="
        << repeated_on_live_app_stability.max_visible_baseline_delta
        << " max_center_delta="
        << repeated_on_live_app_stability.max_weighted_center_x_delta
        << ',' << repeated_on_live_app_stability.max_weighted_center_y_delta
        << '\n';
    std::cout << "LCD atlas ASCII panel probe"
        << " ready=" << ascii_panel_atlas.ready
        << " image=" << ascii_panel_atlas.image.width()
        << 'x' << ascii_panel_atlas.image.height()
        << " msdf_active="
        << ascii_panel_render.msdf_text_renderer_active
        << " msdf_instances="
        << ascii_panel_render.msdf_text_glyph_instances
        << " expected_instances="
        << ascii_panel_stats.expected_drawable_glyphs
        << " msdf_draws=" << ascii_panel_render.msdf_text_draw_calls
        << " glyph_draws=" << ascii_panel_render.glyph_draw_calls
        << " glyph_instances="
        << ascii_panel_render.glyph_buffer_instances
        << " missed_runs="
        << ascii_panel_render.msdf_text_missed_supported_runs
        << " missed_glyphs="
        << ascii_panel_render.msdf_text_missed_supported_glyphs
        << " physical_cell=" << ascii_panel_stats.physical_cell_width
        << 'x' << ascii_panel_stats.physical_cell_height
        << " ascent=" << ascii_panel_stats.physical_cell_ascent
        << " integer_deltas="
        << ascii_panel_stats.cell_width_integer_delta
        << ',' << ascii_panel_stats.cell_height_integer_delta
        << ',' << ascii_panel_stats.cell_ascent_integer_delta
        << " probed_cells=" << ascii_panel_stats.probed_cell_count
        << " all_ink=" << ascii_panel_stats.all_probe_cells_have_ink
        << " min_ink=" << ascii_panel_stats.min_ink_pixels
        << " all_visible="
        << ascii_panel_stats.all_probe_cells_have_visible_ink
        << " min_visible_strong="
        << ascii_panel_stats.min_visible_strong_pixels
        << " key_edge_count="
        << ascii_panel_stats.key_glyph_transition_count
        << " key_edge_max="
        << ascii_panel_stats.max_key_glyph_transition_width
        << " key_edge_avg_max="
        << ascii_panel_stats.max_key_glyph_average_transition_width;
    if (ascii_panel_l_cell != nullptr) {
        std::cout
            << " L_left=" << ascii_panel_l_cell->left_half_ink_pixels
            << " L_right=" << ascii_panel_l_cell->right_half_ink_pixels
            << " L_top=" << ascii_panel_l_cell->top_half_ink_pixels
            << " L_bottom=" << ascii_panel_l_cell->bottom_half_ink_pixels;
    }
    std::cout << '\n';
    std::cout << "LCD atlas ASCII panel raw placement"
        << " raw_ready=" << ascii_panel_raw_atlas_reference.ready
        << " raw_image=" << ascii_panel_raw_atlas_reference.image.width()
        << 'x' << ascii_panel_raw_atlas_reference.image.height()
        << " expected_cells="
        << ascii_panel_raw_atlas_placement.expected_cells
        << " compared_cells="
        << ascii_panel_raw_atlas_placement.compared_cells
        << " all_ink="
        << ascii_panel_raw_atlas_placement.all_cells_have_ink
        << " max_left_delta="
        << ascii_panel_raw_atlas_placement.max_left_delta
        << " max_right_delta="
        << ascii_panel_raw_atlas_placement.max_right_delta
        << " max_top_delta="
        << ascii_panel_raw_atlas_placement.max_top_delta
        << " max_bottom_delta="
        << ascii_panel_raw_atlas_placement.max_bottom_delta
        << " max_first_x_delta="
        << ascii_panel_raw_atlas_placement.max_first_ink_x_delta
        << " max_center_delta="
        << ascii_panel_raw_atlas_placement.max_center_x_delta
        << ',' << ascii_panel_raw_atlas_placement.max_center_y_delta
        << '\n';
    std::cout << "LCD atlas ASCII panel clipped cursor placement"
        << " expected_cells="
        << ascii_panel_cursor_text_raw_atlas_placement.expected_cells
        << " compared_cells="
        << ascii_panel_cursor_text_raw_atlas_placement.compared_cells
        << " all_visible="
        << ascii_panel_cursor_text_raw_atlas_placement.all_cells_have_visible_ink
        << " max_visible_left_delta="
        << ascii_panel_cursor_text_raw_atlas_placement.max_visible_left_delta
        << " max_visible_right_delta="
        << ascii_panel_cursor_text_raw_atlas_placement.max_visible_right_delta
        << " max_visible_top_delta="
        << ascii_panel_cursor_text_raw_atlas_placement.max_visible_top_delta
        << " max_visible_baseline_delta="
        << ascii_panel_cursor_text_raw_atlas_placement.max_visible_baseline_delta
        << " max_visible_center_y_delta="
        << ascii_panel_cursor_text_raw_atlas_placement
            .max_visible_weighted_center_y_delta
        << '\n';
    std::cout << "LCD atlas ASCII panel Qt visible placement"
        << " qt_ready=" << ascii_panel_qt_text_reference.ready
        << " qt_image=" << ascii_panel_qt_text_reference.image.width()
        << 'x' << ascii_panel_qt_text_reference.image.height()
        << " expected_cells="
        << ascii_panel_user_text_qt_text_placement.expected_cells
        << " compared_cells="
        << ascii_panel_user_text_qt_text_placement.compared_cells
        << " all_visible="
        << ascii_panel_user_text_qt_text_placement.all_cells_have_visible_ink
        << " max_visible_left_delta="
        << ascii_panel_user_text_qt_text_placement.max_visible_left_delta
        << " max_visible_right_delta="
        << ascii_panel_user_text_qt_text_placement.max_visible_right_delta
        << " max_visible_top_delta="
        << ascii_panel_user_text_qt_text_placement.max_visible_top_delta
        << " max_visible_baseline_delta="
        << ascii_panel_user_text_qt_text_placement.max_visible_baseline_delta
        << " signed_visible_baseline_range="
        << ascii_panel_user_text_qt_text_placement
            .signed_visible_baseline_delta_range
        << " signed_visible_center_y_range="
        << ascii_panel_user_text_qt_text_placement
            .signed_visible_weighted_center_y_delta_range
        << '\n';
    std::cout << "LCD atlas ASCII panel Qt visible gutters"
        << " expected_pairs="
        << ascii_panel_user_text_qt_text_gutters.expected_pairs
        << " compared_pairs="
        << ascii_panel_user_text_qt_text_gutters.compared_pairs
        << " all_visible="
        << ascii_panel_user_text_qt_text_gutters.all_pairs_visible
        << " max_gutter_delta="
        << ascii_panel_user_text_qt_text_gutters.max_gutter_delta;
    for (const Lcd_ascii_panel_gutter_pair_stats& pair :
        ascii_panel_user_text_qt_text_gutters.pairs)
    {
        std::cout << " [" << pair.left_label.toUtf8().constData()
            << '/' << pair.right_label.toUtf8().constData()
            << " ref=" << pair.reference_gutter
            << " candidate=" << pair.candidate_gutter
            << " delta=" << pair.gutter_delta << ']';
    }
    std::cout << '\n';
    std::cout << "LCD atlas ASCII panel raw visible gutters"
        << " expected_pairs="
        << ascii_panel_user_text_raw_atlas_gutters.expected_pairs
        << " compared_pairs="
        << ascii_panel_user_text_raw_atlas_gutters.compared_pairs
        << " all_visible="
        << ascii_panel_user_text_raw_atlas_gutters.all_pairs_visible
        << " max_gutter_delta="
        << ascii_panel_user_text_raw_atlas_gutters.max_gutter_delta;
    for (const Lcd_ascii_panel_gutter_pair_stats& pair :
        ascii_panel_user_text_raw_atlas_gutters.pairs)
    {
        std::cout << " [" << pair.left_label.toUtf8().constData()
            << '/' << pair.right_label.toUtf8().constData()
            << " ref=" << pair.reference_gutter
            << " candidate=" << pair.candidate_gutter
            << " delta=" << pair.gutter_delta << ']';
    }
    std::cout << '\n';
    std::cout << "LCD atlas ASCII panel user-text jitter"
        << " text=" << lcd_ascii_panel_user_text().toUtf8().constData()
        << " expected_cells="
        << ascii_panel_user_text_raw_atlas_placement.expected_cells
        << " compared_cells="
        << ascii_panel_user_text_raw_atlas_placement.compared_cells
        << " all_ink="
        << ascii_panel_user_text_raw_atlas_placement.all_cells_have_ink
        << " signed_center_y_range="
        << ascii_panel_user_text_raw_atlas_placement.signed_center_y_delta_range
        << " signed_top_range="
        << ascii_panel_user_text_raw_atlas_placement.signed_top_delta_range
        << " signed_bottom_range="
        << ascii_panel_user_text_raw_atlas_placement.signed_bottom_delta_range
        << " max_visible_baseline_delta="
        << ascii_panel_user_text_raw_atlas_placement.max_visible_baseline_delta
        << " signed_visible_baseline_range="
        << ascii_panel_user_text_raw_atlas_placement
            .signed_visible_baseline_delta_range
        << " signed_visible_center_y_range="
        << ascii_panel_user_text_raw_atlas_placement
            .signed_visible_weighted_center_y_delta_range
        << '\n';
    print_lcd_probe_records(probe_records);
    print_missing_lcd_probe_family_coverage_labels(probe_records);

    const term::Glyph_coverage_counts& production_coverage =
        atlas.atlas_report.frame_build.glyph_coverage;
    const bool msdf_text_renderer_enabled =
        atlas.atlas_report.render.msdf_text_renderer_enabled;
    const bool msdf_text_renderer_active =
        atlas.atlas_report.render.msdf_text_renderer_active;

    // Batch 1 targets the pinned Windows D3D11 hardware path; missing
    // dual-source probing or sample-family glyph images is a gate failure here.
    bool ok = true;
    ok &= check(
        lcd_probe_image_has_pixels(atlas.image),
        "LCD atlas probe captures a non-empty atlas image");
    ok &= check(
        lcd_probe_image_has_pixels(reference.image),
        "LCD atlas probe renders a non-empty RGBA atlas reference image");
    ok &= check(
        qt_text_reference.ready &&
            lcd_probe_image_has_pixels(qt_text_reference.image),
        "LCD atlas probe captures a non-empty Qt text-node reference image");
    ok &= check(
        single_w_atlas.ready &&
            atlas_report_render_state_ready(single_w_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(single_w_atlas.image),
        "LCD atlas probe captures a non-empty single-W atlas image");
    ok &= check(
        lcd_probe_image_has_pixels(single_w_raw_atlas_reference.image),
        "LCD atlas probe renders a non-empty single-W raw atlas reference image");
    ok &= check(
        single_w_qt_text_reference.ready &&
            lcd_probe_image_has_pixels(single_w_qt_text_reference.image),
        "LCD atlas probe captures a non-empty single-W Qt text-node reference image");
    ok &= check(
        repeated_w_atlas.ready &&
            atlas_report_render_state_ready(repeated_w_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(repeated_w_atlas.image),
        "LCD atlas probe captures a non-empty repeated-W atlas image");
    ok &= check(
        translated_repeated_w_atlas.ready &&
            atlas_report_render_state_ready(
                translated_repeated_w_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(translated_repeated_w_atlas.image),
        "LCD atlas probe captures a non-empty translated repeated-W atlas image");
    ok &= check(
        contiguous_x_atlas.ready &&
            atlas_report_render_state_ready(contiguous_x_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(contiguous_x_atlas.image),
        "LCD atlas probe captures a non-empty contiguous-X atlas image");
    ok &= check(
        intensity_x_atlas.ready &&
            atlas_report_render_state_ready(intensity_x_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(intensity_x_atlas.image),
        "LCD atlas probe captures a non-empty intensity-X atlas image");
    ok &= check(
        fragmented_x_atlas.ready &&
            atlas_report_render_state_ready(fragmented_x_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(fragmented_x_atlas.image),
        "LCD atlas probe captures a non-empty fragmented-X atlas image");
    ok &= check(
        repeated_on_atlas.ready &&
            atlas_report_render_state_ready(repeated_on_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(repeated_on_atlas.image),
        "LCD atlas probe captures a non-empty repeated-on atlas image");
    ok &= check(
        repeated_on_app_font_atlas.ready &&
            atlas_report_render_state_ready(
                repeated_on_app_font_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(repeated_on_app_font_atlas.image),
        "LCD atlas probe captures a non-empty app-font repeated-on atlas image");
    ok &= check(
        ascii_panel_atlas.ready &&
            atlas_report_render_state_ready(ascii_panel_atlas.atlas_report) &&
            lcd_probe_image_has_pixels(ascii_panel_atlas.image),
        "LCD atlas probe captures a non-empty ASCII panel atlas image");
    ok &= check(
        lcd_probe_image_has_pixels(ascii_panel_raw_atlas_reference.image),
        "LCD atlas probe renders a non-empty ASCII panel raw atlas reference image");
    ok &= check(
        ascii_panel_qt_text_reference.ready &&
            lcd_probe_image_has_pixels(ascii_panel_qt_text_reference.image),
        "LCD atlas probe renders a non-empty ASCII panel Qt text reference image");
    ok &= check(
        lcd_probe_image_has_visible_variation(atlas.image),
        "LCD atlas probe atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(reference.image),
        "LCD atlas probe reference image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(qt_text_reference.image),
        "LCD atlas probe Qt text-node reference contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(single_w_atlas.image),
        "LCD atlas probe single-W atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(single_w_raw_atlas_reference.image),
        "LCD atlas probe single-W raw atlas reference contains visible variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(single_w_qt_text_reference.image),
        "LCD atlas probe single-W Qt text-node image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(repeated_w_atlas.image),
        "LCD atlas probe repeated-W atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(translated_repeated_w_atlas.image),
        "LCD atlas probe translated repeated-W atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(contiguous_x_atlas.image),
        "LCD atlas probe contiguous-X atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(intensity_x_atlas.image),
        "LCD atlas probe intensity-X atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(fragmented_x_atlas.image),
        "LCD atlas probe fragmented-X atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(repeated_on_atlas.image),
        "LCD atlas probe repeated-on atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(repeated_on_app_font_atlas.image),
        "LCD atlas probe app-font repeated-on atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(ascii_panel_atlas.image),
        "LCD atlas probe ASCII panel atlas image contains visible fixture variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(
            ascii_panel_raw_atlas_reference.image),
        "LCD atlas probe ASCII panel raw atlas reference contains visible variation");
    ok &= check(
        lcd_probe_image_has_visible_variation(
            ascii_panel_qt_text_reference.image),
        "LCD atlas probe ASCII panel Qt text reference contains visible variation");
    if (single_w_msdf.compiled) {
        ok &= check(
            single_w_msdf.render.ready &&
                lcd_probe_image_has_pixels(single_w_msdf.render.image),
            "LCD atlas probe captures a non-empty single-W MSDF image");
        ok &= check(
            lcd_probe_image_has_visible_variation(single_w_msdf.render.image),
            "LCD atlas probe single-W MSDF image contains visible variation");
    }
    ok &= check(
        lcd_probe_image_has_pixels(raster_variants.sheet) &&
            lcd_probe_image_has_visible_variation(raster_variants.sheet),
        "LCD atlas probe raster variant sheet contains visible diagnostic content");
    ok &= check(
        raster_variant_probe_records_are_usable(raster_variants),
        "LCD atlas probe records complete raster variant diagnostics");
    ok &= check(
        metrics_placement_probe_is_usable(metrics_placement),
        "LCD atlas probe records complete metrics placement diagnostics");
    ok &= check(
        qt_text_reference.image.size() == atlas.image.size(),
        "LCD atlas probe Qt text-node reference uses the atlas image size");
    ok &= check(
        single_w_qt_text_reference.image.size() == single_w_atlas.image.size(),
        "LCD atlas probe single-W Qt text-node reference uses the atlas image size");
    ok &= check(
        single_w_raw_atlas_reference.image.size() == single_w_atlas.image.size(),
        "LCD atlas probe single-W raw atlas reference uses the atlas image size");
    ok &= check(
        ascii_panel_raw_atlas_reference.image.size() ==
            ascii_panel_atlas.image.size(),
        "LCD atlas probe ASCII panel raw atlas reference uses the atlas image size");
    ok &= check(
        ascii_panel_qt_text_reference.image.size() ==
            ascii_panel_atlas.image.size(),
        "LCD atlas probe ASCII panel Qt text reference uses the atlas image size");
    ok &= check(
        pixel_device_pixel_ratios_match(
            qt_text_reference.device_pixel_ratio,
            atlas.device_pixel_ratio) &&
            pixel_device_pixel_ratios_match(
                qt_text_reference.image_device_pixel_ratio,
                atlas.image_device_pixel_ratio),
        "LCD atlas probe Qt text-node reference uses the atlas DPR");
    ok &= check(
        pixel_device_pixel_ratios_match(
            single_w_qt_text_reference.device_pixel_ratio,
            single_w_atlas.device_pixel_ratio) &&
            pixel_device_pixel_ratios_match(
                single_w_qt_text_reference.image_device_pixel_ratio,
                single_w_atlas.image_device_pixel_ratio),
        "LCD atlas probe single-W Qt text-node reference uses the atlas DPR");
    ok &= check(
        pixel_device_pixel_ratios_match(
            single_w_raw_atlas_reference.device_pixel_ratio,
            single_w_atlas.device_pixel_ratio) &&
            pixel_device_pixel_ratios_match(
                single_w_raw_atlas_reference.image_device_pixel_ratio,
                single_w_atlas.image_device_pixel_ratio),
        "LCD atlas probe single-W raw atlas reference uses the atlas DPR");
    ok &= check(
        pixel_device_pixel_ratios_match(
            ascii_panel_raw_atlas_reference.device_pixel_ratio,
            ascii_panel_atlas.device_pixel_ratio) &&
            pixel_device_pixel_ratios_match(
                ascii_panel_raw_atlas_reference.image_device_pixel_ratio,
                ascii_panel_atlas.image_device_pixel_ratio),
        "LCD atlas probe ASCII panel raw atlas reference uses the atlas DPR");
    ok &= check(
        pixel_device_pixel_ratios_match(
            ascii_panel_qt_text_reference.device_pixel_ratio,
            ascii_panel_atlas.device_pixel_ratio) &&
            pixel_device_pixel_ratios_match(
                ascii_panel_qt_text_reference.image_device_pixel_ratio,
                ascii_panel_atlas.image_device_pixel_ratio),
        "LCD atlas probe ASCII panel Qt text reference uses the atlas DPR");
    ok &= check(
        qt_text_glyphs.compared_pixels > 0,
        "LCD atlas probe Qt text-node comparison covers glyph mask pixels");
    ok &= check(
        qt_text_glyphs.reference_ink_pixels > 0 &&
            qt_text_glyphs.atlas_ink_pixels > 0,
        "LCD atlas probe Qt text-node comparison sees glyph ink in both renderers");
    ok &= check(
        single_w_qt_text_glyphs.compared_pixels > 0,
        "LCD atlas probe single-W comparison covers glyph mask pixels");
    ok &= check(
        single_w_qt_text_glyphs.reference_ink_pixels > 0 &&
            single_w_qt_text_glyphs.atlas_ink_pixels > 0,
        "LCD atlas probe single-W comparison sees glyph ink in both renderers");
    if (single_w_msdf.compiled) {
        ok &= check(
            single_w_msdf_raw_glyphs.compared_pixels > 0 &&
                single_w_msdf_qt_text_glyphs.compared_pixels > 0,
            "LCD atlas probe single-W MSDF comparison covers glyph mask pixels");
        ok &= check(
            single_w_msdf_raw_glyphs.reference_ink_pixels > 0 &&
                single_w_msdf_raw_glyphs.atlas_ink_pixels > 0 &&
                single_w_msdf_qt_text_glyphs.reference_ink_pixels > 0 &&
                single_w_msdf_qt_text_glyphs.atlas_ink_pixels > 0,
            "LCD atlas probe single-W MSDF comparison sees glyph ink");
    }
    ok &= compare_pixel_fixture(
        fixture,
        reference,
        atlas,
        backend,
        rgba_reference_budget_for_coverage(production_coverage),
        "LCD atlas RGBA reference",
        !msdf_text_renderer_active);
    ok &= check(
        !probe_records.empty(),
        "LCD atlas probe records glyph image capability details");
    ok &= check(
        lcd_probe_records_cover_required_families(probe_records),
        "LCD atlas probe records concrete glyph image coverage for every sample family");
    ok &= check(
        lcd_probe_records_have_supported_classifier_results(probe_records),
        "LCD atlas probe concrete glyph images classify as supported coverage kinds");
    ok &= check(
        lcd_probe_color_capable_records_keep_color_kind(probe_records),
        "LCD atlas probe color-capable glyph images keep color coverage kind");
    ok &= check(
        lcd_probe_has_valid_production_tile_for_family(
            probe_records,
            QStringLiteral("emoji_color")),
        "LCD atlas probe records production-valid tiles for emoji presentation samples");
    ok &= check(
        atlas.atlas_report.frame_build.glyph_coverage.grayscale_masks > 0,
        "LCD atlas probe reports production grayscale coverage entries");
    ok &= check(
        glyph_coverage_has_lcd(production_coverage),
        "LCD atlas probe reports production LCD coverage entries");
    if (atlas.atlas_report.render.atlas_page_count > 1) {
        ok &= check(
            atlas.atlas_report.frame_build.max_glyph_instance_page > 0,
            "LCD atlas probe renders glyph instances from texture-array pages beyond zero");
    }
    ok &= check(
        atlas.atlas_report.render.glyph_shader_package_available,
        "LCD atlas probe reports loaded glyph shader packages");
    ok &= check(
        atlas.atlas_report.render.glyph_sampler_mode ==
            term::Qsg_atlas_sampler_mode::NEAREST,
        "LCD atlas probe reports nearest glyph coverage sampling");
    if (msdf_text_renderer_enabled) {
        ok &= check(
            msdf_text_renderer_active,
            "LCD atlas probe activates the MSDF text renderer");
        ok &= check(
            atlas.atlas_report.render.msdf_text_shader_package_available &&
                atlas.atlas_report.render.msdf_text_atlas_built &&
                atlas.atlas_report.render.msdf_text_atlas_ready &&
                atlas.atlas_report.render.msdf_text_texture_ready &&
                atlas.atlas_report.render.msdf_text_resources_ready,
            "LCD atlas probe reports ready MSDF renderer resources");
        ok &= check(
            atlas.atlas_report.render.msdf_text_sampler_mode ==
                term::Qsg_atlas_sampler_mode::LINEAR,
            "LCD atlas probe reports linear MSDF atlas sampling");
        ok &= check(
            atlas.atlas_report.render.msdf_text_supported_runs > 0 &&
                atlas.atlas_report.render.msdf_text_runs > 0 &&
                atlas.atlas_report.render.msdf_text_glyph_instances > 0 &&
                atlas.atlas_report.render.msdf_text_draw_calls > 0,
            "LCD atlas probe routes supported text through MSDF draw calls");
        ok &= check(
            atlas.atlas_report.render.msdf_text_missed_supported_runs == 0 &&
                atlas.atlas_report.render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe reports no silent MSDF text misses");
        ok &= check(
            single_w_atlas.atlas_report.render.msdf_text_renderer_active &&
                single_w_atlas.atlas_report.render.msdf_text_glyph_instances > 0 &&
                single_w_atlas.atlas_report.render.msdf_text_draw_calls > 0 &&
                single_w_atlas.atlas_report.render.glyph_draw_calls == 0 &&
                single_w_atlas.atlas_report.render.glyph_buffer_instances == 0 &&
                single_w_atlas.atlas_report.render.msdf_text_missed_supported_runs == 0 &&
                single_w_atlas.atlas_report.render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe single-W fixture uses the real MSDF renderer path");
        constexpr int k_min_single_w_first_column_rows = 5;
        ok &= check(
            single_w_atlas_first_column.has_ink(),
            "LCD atlas probe single-W MSDF first column contains ink");
        ok &= check(
            single_w_atlas_first_column.first_column_rows >=
                k_min_single_w_first_column_rows,
            "LCD atlas probe single-W MSDF first column has enough samples");
        ok &= check(
            single_w_raw_atlas_first_column.has_ink() &&
                single_w_raw_atlas_first_column.first_column_rows >=
                    k_min_single_w_first_column_rows,
            "LCD atlas probe single-W raw atlas first column is comparable");
        constexpr int k_single_w_msdf_bbox_tolerance_pixels = 1;
        ok &= check(
            single_w_msdf_first_column.has_ink(),
            "LCD atlas probe CPU MSDF single-W first column contains ink");
        constexpr int k_single_w_msdf_column_metric_tolerance = 1;
        ok &= check(
            std::abs(
                single_w_atlas_first_column.distinct_first_column_rgb_colors -
                single_w_msdf_first_column.distinct_first_column_rgb_colors) <=
                k_single_w_msdf_column_metric_tolerance &&
                std::abs(
                    single_w_atlas_first_column.adjacent_equal_first_column_pairs -
                    single_w_msdf_first_column.adjacent_equal_first_column_pairs) <=
                    k_single_w_msdf_column_metric_tolerance &&
                std::abs(
                    single_w_atlas_first_column.longest_equal_first_column_run -
                    single_w_msdf_first_column.longest_equal_first_column_run) <=
                    k_single_w_msdf_column_metric_tolerance,
            "LCD atlas probe single-W MSDF first-column metrics match the CPU diagnostic");
        ok &= check(
            lcd_rect_components_within(
                single_w_atlas_first_column.bbox,
                single_w_msdf_first_column.bbox,
                k_single_w_msdf_bbox_tolerance_pixels),
            "LCD atlas probe single-W MSDF bbox matches the CPU MSDF diagnostic");
        ok &= check(
            single_w_atlas_msdf_ink_ratio >= 0.80 &&
                single_w_atlas_msdf_ink_ratio <= 1.25,
            "LCD atlas probe single-W MSDF ink count matches the CPU MSDF diagnostic");
        ok &= check(
            single_w_atlas_edges.transition_count > 0 &&
                single_w_msdf_edges.transition_count > 0,
            "LCD atlas probe single-W MSDF edge metric captures transitions");
        ok &= check(
            single_w_atlas_edges.max_transition_width <=
                single_w_msdf_edges.max_transition_width + 1,
            "LCD atlas probe single-W MSDF edge width is not blurrier than CPU MSDF");
        ok &= check(
            repeated_w_render.msdf_text_renderer_active &&
                repeated_w_render.msdf_text_glyph_instances ==
                    k_lcd_repeated_w_cell_count &&
                repeated_w_render.msdf_text_draw_calls > 0 &&
                repeated_w_render.glyph_draw_calls == 0 &&
                repeated_w_render.glyph_buffer_instances == 0 &&
                repeated_w_render.msdf_text_missed_supported_runs == 0 &&
                repeated_w_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe repeated-W fixture uses only the real MSDF renderer path");
        ok &= check(
            repeated_w_stability.cell_width_integer &&
                repeated_w_stability.cell_height_integer &&
                repeated_w_stability.cell_ascent_integer,
            "LCD atlas probe repeated-W fixture uses integer physical cell metrics");
        ok &= check(
            repeated_w_stability.compared_cells ==
                k_lcd_repeated_w_cell_count - 1 &&
                repeated_w_stability.compared_pixels > 0,
            "LCD atlas probe repeated-W stability comparison covers every repeated cell");
        ok &= check(
            repeated_w_stability.diff_pixels == 0 &&
                repeated_w_stability.max_diff_pixels_per_cell == 0 &&
                repeated_w_stability.max_delta == 0 &&
                repeated_w_stability.ink_mask_diff_pixels == 0 &&
                repeated_w_stability.max_ink_mask_diff_pixels_per_cell == 0,
            "LCD atlas probe repeated-W RGB crops are identical across cells");
        ok &= check(
            repeated_w_stability.all_cells_have_ink &&
                repeated_w_stability.max_ink_pixels_delta == 0 &&
                repeated_w_stability.max_relative_first_ink_x_delta == 0 &&
                repeated_w_stability.max_relative_bbox_x_delta == 0 &&
                repeated_w_stability.max_relative_bbox_y_delta == 0 &&
                repeated_w_stability.max_relative_bbox_width_delta == 0 &&
                repeated_w_stability.max_relative_bbox_height_delta == 0,
            "LCD atlas probe repeated-W glyph ink geometry is identical across cells");
        ok &= check(
            translated_repeated_w_render.msdf_text_renderer_active &&
                translated_repeated_w_render.msdf_text_glyph_instances ==
                    k_lcd_repeated_w_cell_count &&
                translated_repeated_w_render.msdf_text_draw_calls > 0 &&
                translated_repeated_w_render.glyph_draw_calls == 0 &&
                translated_repeated_w_render.glyph_buffer_instances == 0 &&
                translated_repeated_w_render.msdf_text_missed_supported_runs == 0 &&
                translated_repeated_w_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe translated repeated-W fixture uses only the real MSDF renderer path");
        ok &= check(
            translated_repeated_w_stability.cell_width_integer &&
                translated_repeated_w_stability.cell_height_integer &&
                translated_repeated_w_stability.cell_ascent_integer,
            "LCD atlas probe translated repeated-W fixture uses integer physical cell metrics");
        ok &= check(
            translated_repeated_w_stability.compared_cells ==
                k_lcd_repeated_w_cell_count - 1 &&
                translated_repeated_w_stability.compared_pixels > 0,
            "LCD atlas probe translated repeated-W stability comparison covers every repeated cell");
        constexpr int k_translated_repeated_w_rgb_delta_tolerance = 1;
        ok &= check(
            translated_repeated_w_stability.max_delta <=
                    k_translated_repeated_w_rgb_delta_tolerance &&
                translated_repeated_w_stability.ink_mask_diff_pixels == 0 &&
                translated_repeated_w_stability.max_ink_mask_diff_pixels_per_cell == 0,
            "LCD atlas probe translated repeated-W RGB crops remain visually stable across cells");
        ok &= check(
            translated_repeated_w_stability.all_cells_have_ink &&
                translated_repeated_w_stability.max_ink_pixels_delta == 0 &&
                translated_repeated_w_stability.max_relative_first_ink_x_delta == 0 &&
                translated_repeated_w_stability.max_relative_bbox_x_delta == 0 &&
                translated_repeated_w_stability.max_relative_bbox_y_delta == 0 &&
                translated_repeated_w_stability.max_relative_bbox_width_delta == 0 &&
                translated_repeated_w_stability.max_relative_bbox_height_delta == 0,
            "LCD atlas probe translated repeated-W glyph ink geometry is identical across cells");
        ok &= check(
            translated_w_origin_match.compared_cells ==
                k_lcd_repeated_w_cell_count &&
                translated_w_origin_match.compared_pixels > 0,
            "LCD atlas probe translated repeated-W comparison covers every origin cell");
        const bool translated_w_origin_rgb_stable =
            translated_w_origin_match.max_delta <=
                k_translated_repeated_w_rgb_delta_tolerance &&
            translated_w_origin_match.ink_mask_diff_pixels == 0 &&
            translated_w_origin_match.max_ink_mask_diff_pixels_per_cell == 0;
        ok &= check(
            translated_w_origin_rgb_stable,
            "LCD atlas probe translated repeated-W RGB crops remain visually stable against the origin cells");
        const bool translated_w_origin_geometry_exact =
            translated_w_origin_match.all_cells_have_ink &&
            translated_w_origin_match.max_ink_pixels_delta == 0 &&
            translated_w_origin_match.max_relative_first_ink_x_delta == 0 &&
            translated_w_origin_match.max_relative_bbox_x_delta == 0 &&
            translated_w_origin_match.max_relative_bbox_y_delta == 0 &&
            translated_w_origin_match.max_relative_bbox_width_delta == 0 &&
            translated_w_origin_match.max_relative_bbox_height_delta == 0;
        ok &= check(
            translated_w_origin_geometry_exact,
            "LCD atlas probe translated repeated-W glyph ink geometry matches the origin cells");
        ok &= check(
            contiguous_x_render.msdf_text_renderer_active &&
                contiguous_x_render.msdf_text_runs == 1 &&
                contiguous_x_render.msdf_text_glyph_instances ==
                    k_lcd_contiguous_x_cell_count &&
                contiguous_x_render.msdf_text_draw_calls > 0 &&
                contiguous_x_render.glyph_draw_calls == 0 &&
                contiguous_x_render.glyph_buffer_instances == 0 &&
                contiguous_x_render.msdf_text_missed_supported_runs == 0 &&
                contiguous_x_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe contiguous-X fixture uses one real MSDF text run");
        ok &= check(
            contiguous_x_stability.cell_width_integer &&
                contiguous_x_stability.cell_height_integer &&
                contiguous_x_stability.cell_ascent_integer,
            "LCD atlas probe contiguous-X fixture uses integer physical cell metrics");
        ok &= check(
            contiguous_x_stability.compared_cells ==
                k_lcd_contiguous_x_cell_count - 1 &&
                contiguous_x_stability.compared_pixels > 0,
            "LCD atlas probe contiguous-X stability comparison covers every repeated cell");
        ok &= check(
            contiguous_x_stability.diff_pixels == 0 &&
                contiguous_x_stability.max_diff_pixels_per_cell == 0 &&
                contiguous_x_stability.max_delta == 0 &&
                contiguous_x_stability.ink_mask_diff_pixels == 0 &&
                contiguous_x_stability.max_ink_mask_diff_pixels_per_cell == 0,
            "LCD atlas probe contiguous-X RGB crops are identical across cells");
        ok &= check(
            contiguous_x_stability.all_cells_have_ink &&
                contiguous_x_stability.max_ink_pixels_delta == 0 &&
                contiguous_x_stability.max_relative_first_ink_x_delta == 0 &&
                contiguous_x_stability.max_relative_bbox_x_delta == 0 &&
                contiguous_x_stability.max_relative_bbox_y_delta == 0 &&
                contiguous_x_stability.max_relative_bbox_width_delta == 0 &&
                contiguous_x_stability.max_relative_bbox_height_delta == 0,
            "LCD atlas probe contiguous-X glyph ink geometry is identical across cells");
        ok &= check(
            intensity_x_render.msdf_text_renderer_active &&
                intensity_x_render.msdf_text_runs == k_lcd_intensity_x_rows &&
                intensity_x_render.msdf_text_glyph_instances ==
                    k_lcd_intensity_x_rows * k_lcd_intensity_x_columns &&
                intensity_x_render.msdf_text_draw_calls > 0 &&
                intensity_x_render.glyph_draw_calls == 0 &&
                intensity_x_render.glyph_buffer_instances == 0 &&
                intensity_x_render.msdf_text_missed_supported_runs == 0 &&
                intensity_x_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe intensity-X fixture uses real MSDF text runs");
        ok &= check(
            static_cast<int>(intensity_x_stability.size()) ==
                k_lcd_intensity_x_rows,
            "LCD atlas probe intensity-X stability covers every intensity row");
        for (const Lcd_intensity_x_row_stability& row : intensity_x_stability) {
            const std::string row_label =
                "LCD atlas probe intensity-X row " +
                std::to_string(row.row_index);
            const Lcd_repeated_w_stability_stats& stability = row.stability;
            const auto check_row = [&](bool condition, const char* suffix) {
                const std::string message = row_label + suffix;
                return check(
                    condition,
                    std::string_view(message.data(), message.size()));
            };
            ok &= check_row(
                stability.cell_width_integer &&
                    stability.cell_height_integer &&
                    stability.cell_ascent_integer,
                " uses integer physical cell metrics");
            ok &= check_row(
                stability.compared_cells ==
                    k_lcd_intensity_x_columns - 1 &&
                    stability.compared_pixels > 0,
                " stability comparison covers every repeated cell");
            ok &= check_row(
                stability.diff_pixels == 0 &&
                    stability.max_diff_pixels_per_cell == 0 &&
                    stability.max_delta == 0 &&
                    stability.ink_mask_diff_pixels == 0 &&
                    stability.max_ink_mask_diff_pixels_per_cell == 0,
                " RGB crops are identical across cells");
            ok &= check_row(
                stability.all_cells_have_ink &&
                    stability.max_ink_pixels_delta == 0 &&
                    stability.max_relative_first_ink_x_delta == 0 &&
                    stability.max_relative_bbox_x_delta == 0 &&
                    stability.max_relative_bbox_y_delta == 0 &&
                    stability.max_relative_bbox_width_delta == 0 &&
                    stability.max_relative_bbox_height_delta == 0,
                " glyph ink geometry is identical across cells");
        }
        const int fragmented_x_shaped_runs =
            fragmented_x_atlas.atlas_report.producer.shaped_runs_built +
            fragmented_x_atlas.atlas_report.producer.shaped_runs_reused;
        const int fragmented_x_shaped_glyph_records =
            fragmented_x_atlas.atlas_report.producer.shaped_glyph_records_built +
            fragmented_x_atlas.atlas_report.producer.shaped_glyph_records_reused;
        ok &= check(
            fragmented_x_render.msdf_text_runs == 0 &&
                fragmented_x_render.msdf_text_glyph_instances == 0 &&
                fragmented_x_render.msdf_text_draw_calls == 0 &&
                fragmented_x_render.glyph_draw_calls > 0 &&
                fragmented_x_atlas.atlas_report.frame_build.glyph_instances ==
                    k_lcd_fragmented_x_columns &&
                fragmented_x_atlas.atlas_report.producer.simple_path_used == 0 &&
                fragmented_x_shaped_runs >= k_lcd_fragmented_x_columns &&
                fragmented_x_shaped_glyph_records >= k_lcd_fragmented_x_columns,
            "LCD atlas probe fragmented-X fixture uses the shaped glyph atlas path");
        ok &= check(
            fragmented_x_atlas.atlas_report.frame_build.snapped_origin_failures == 0,
            "LCD atlas probe fragmented-X fixture submits snapped glyph origins");
        ok &= check(
            fragmented_x_stability.cell_width_integer &&
                fragmented_x_stability.cell_height_integer &&
                fragmented_x_stability.cell_ascent_integer,
            "LCD atlas probe fragmented-X fixture uses integer physical cell metrics");
        ok &= check(
            fragmented_x_stability.compared_cells ==
                k_lcd_fragmented_x_columns - 1 &&
                fragmented_x_stability.compared_pixels > 0,
            "LCD atlas probe fragmented-X stability comparison covers every repeated cell");
        ok &= check(
            fragmented_x_stability.all_cells_have_ink &&
                fragmented_x_stability.max_relative_first_ink_x_delta == 0 &&
                fragmented_x_stability.max_relative_bbox_x_delta == 0 &&
                fragmented_x_stability.max_relative_bbox_y_delta == 0 &&
                fragmented_x_stability.max_relative_bbox_width_delta == 0 &&
                fragmented_x_stability.max_relative_bbox_height_delta == 0,
            "LCD atlas probe fragmented-X glyph ink geometry is identical across cells");
        ok &= check(
            repeated_on_render.msdf_text_renderer_active &&
                repeated_on_render.msdf_text_glyph_instances == 4 &&
                repeated_on_render.msdf_text_draw_calls > 0 &&
                repeated_on_render.glyph_draw_calls == 0 &&
                repeated_on_render.glyph_buffer_instances == 0 &&
                repeated_on_render.msdf_text_missed_supported_runs == 0 &&
                repeated_on_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe repeated-on fixture uses only the real MSDF renderer path");
        ok &= check(
            repeated_on_stability.all_glyphs_visible &&
                repeated_on_stability.compared_same_glyph_pairs == 2,
            "LCD atlas probe repeated-on fixture covers both repeated words");
        constexpr double k_repeated_on_center_tolerance_pixels = 0.001;
        ok &= check(
            repeated_on_stability.gutter_delta == 0 &&
                repeated_on_stability.max_visible_left_delta == 0 &&
                repeated_on_stability.max_visible_right_delta == 0 &&
                repeated_on_stability.max_visible_top_delta == 0 &&
                repeated_on_stability.max_visible_baseline_delta == 0 &&
                repeated_on_stability.max_weighted_center_x_delta <=
                    k_repeated_on_center_tolerance_pixels &&
                repeated_on_stability.max_weighted_center_y_delta <=
                    k_repeated_on_center_tolerance_pixels,
            "LCD atlas probe repeated-on glyphs and gutters are stable across columns");
        ok &= check(
            repeated_on_app_font_render.msdf_text_renderer_active &&
                repeated_on_app_font_render.msdf_text_glyph_instances == 4 &&
                repeated_on_app_font_render.msdf_text_draw_calls > 0 &&
                repeated_on_app_font_render.glyph_draw_calls == 0 &&
                repeated_on_app_font_render.glyph_buffer_instances == 0 &&
                repeated_on_app_font_render.msdf_text_missed_supported_runs == 0 &&
                repeated_on_app_font_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe app-font repeated-on fixture uses only the real MSDF renderer path");
        ok &= check(
            repeated_on_app_font_stability.all_glyphs_visible &&
                repeated_on_app_font_stability.compared_same_glyph_pairs == 2,
            "LCD atlas probe app-font repeated-on fixture covers both repeated words");
        ok &= check(
            repeated_on_app_font_stability.gutter_delta == 0 &&
                repeated_on_app_font_stability.max_visible_left_delta == 0 &&
                repeated_on_app_font_stability.max_visible_right_delta == 0 &&
                repeated_on_app_font_stability.max_visible_top_delta == 0 &&
                repeated_on_app_font_stability.max_visible_baseline_delta == 0 &&
                repeated_on_app_font_stability.max_weighted_center_x_delta <=
                    k_repeated_on_center_tolerance_pixels &&
                repeated_on_app_font_stability.max_weighted_center_y_delta <=
                    k_repeated_on_center_tolerance_pixels,
            "LCD atlas probe app-font repeated-on glyphs and gutters are stable across columns");
        ok &= check(
            repeated_on_live_app_render.msdf_text_renderer_active &&
                repeated_on_live_app_render.msdf_text_glyph_instances == 4 &&
                repeated_on_live_app_render.msdf_text_draw_calls > 0 &&
                repeated_on_live_app_render.glyph_draw_calls == 0 &&
                repeated_on_live_app_render.glyph_buffer_instances == 0 &&
                repeated_on_live_app_render.msdf_text_missed_supported_runs == 0 &&
                repeated_on_live_app_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe live-app repeated-on fixture uses only the real MSDF renderer path");
        ok &= check(
            repeated_on_live_app_stability.all_glyphs_visible &&
                repeated_on_live_app_stability.compared_same_glyph_pairs == 2,
            "LCD atlas probe live-app repeated-on fixture covers both repeated words");
        ok &= check(
            repeated_on_live_app_stability.gutter_delta == 0 &&
                repeated_on_live_app_stability.max_visible_left_delta == 0 &&
                repeated_on_live_app_stability.max_visible_right_delta == 0 &&
                repeated_on_live_app_stability.max_visible_top_delta == 0 &&
                repeated_on_live_app_stability.max_visible_baseline_delta == 0 &&
                repeated_on_live_app_stability.max_weighted_center_x_delta <=
                    k_repeated_on_center_tolerance_pixels &&
                repeated_on_live_app_stability.max_weighted_center_y_delta <=
                    k_repeated_on_center_tolerance_pixels,
            "LCD atlas probe live-app repeated-on glyphs and gutters are stable across columns");
        ok &= check(
            ascii_panel_render.msdf_text_renderer_active &&
                ascii_panel_render.msdf_text_supported_runs > 0 &&
                ascii_panel_render.msdf_text_runs > 0 &&
                ascii_panel_render.msdf_text_draw_calls > 0 &&
                ascii_panel_render.glyph_draw_calls == 0 &&
                ascii_panel_render.glyph_buffer_instances == 0 &&
                ascii_panel_render.msdf_text_missed_supported_runs == 0 &&
                ascii_panel_render.msdf_text_missed_supported_glyphs == 0,
            "LCD atlas probe ASCII panel fixture uses only the real MSDF renderer path");
        ok &= check(
            ascii_panel_stats.expected_drawable_glyphs > 0 &&
                ascii_panel_render.msdf_text_glyph_instances ==
                    ascii_panel_stats.expected_drawable_glyphs,
            "LCD atlas probe ASCII panel MSDF glyph count matches printable cells and cursor text");
        ok &= check(
            ascii_panel_stats.cell_width_integer &&
                ascii_panel_stats.cell_height_integer &&
                ascii_panel_stats.cell_ascent_integer,
            "LCD atlas probe ASCII panel fixture uses integer physical cell metrics");
        ok &= check(
            ascii_panel_stats.probed_cell_count ==
                static_cast<int>(lcd_ascii_panel_probe_cell_specs().size()) &&
                ascii_panel_stats.all_probe_cells_have_ink &&
                ascii_panel_stats.min_ink_pixels > 0,
            "LCD atlas probe ASCII panel representative cells and cursor text contain ink");
        ok &= check(
            ascii_panel_stats.all_probe_cells_have_visible_ink &&
                ascii_panel_stats.min_visible_strong_pixels > 0 &&
                ascii_panel_qt_text_reference_stats.all_probe_cells_have_visible_ink &&
                ascii_panel_qt_text_reference_stats.min_visible_strong_pixels > 0,
            "LCD atlas probe ASCII panel MSDF and Qt references contain visible strong ink");
        const int ascii_panel_key_edge_width_limit = std::max(
            4,
            static_cast<int>(std::lround(
                ascii_panel_stats.physical_cell_width)));
        const double ascii_panel_key_edge_average_limit = std::max(
            single_w_atlas_edges.average_transition_width,
            single_w_msdf_edges.average_transition_width) + 1.0;
        ok &= check(
            ascii_panel_m_cell != nullptr &&
                ascii_panel_m_cell->has_ink() &&
                ascii_panel_m_cell->edge_transitions.transition_count > 0 &&
                ascii_panel_m_cell->edge_transitions.max_transition_width <=
                    ascii_panel_key_edge_width_limit &&
                ascii_panel_m_cell->edge_transitions.average_transition_width <=
                    ascii_panel_key_edge_average_limit,
            "LCD atlas probe ASCII panel M glyph has bounded edge transitions");
        ok &= check(
            ascii_panel_lower_m_cell != nullptr &&
                ascii_panel_lower_m_cell->has_ink() &&
                ascii_panel_lower_m_cell->left_third_ink_pixels > 0 &&
                ascii_panel_lower_m_cell->center_third_ink_pixels > 0 &&
                ascii_panel_lower_m_cell->right_third_ink_pixels > 0,
            "LCD atlas probe ASCII panel m glyph keeps left, center, and right ink");
        ok &= check(
            ascii_panel_e_cell != nullptr &&
                ascii_panel_e_cell->has_ink() &&
                ascii_panel_e_cell->edge_transitions.transition_count > 0 &&
                ascii_panel_e_cell->edge_transitions.max_transition_width <=
                    ascii_panel_key_edge_width_limit &&
                ascii_panel_e_cell->edge_transitions.average_transition_width <=
                    ascii_panel_key_edge_average_limit,
            "LCD atlas probe ASCII panel e glyph has bounded edge transitions");
        ok &= check(
            ascii_panel_l_cell != nullptr &&
                ascii_panel_l_cell->has_ink() &&
                ascii_panel_l_cell->left_half_ink_pixels >
                    ascii_panel_l_cell->right_half_ink_pixels &&
                ascii_panel_l_cell->bottom_half_ink_pixels >
                    ascii_panel_l_cell->top_half_ink_pixels,
            "LCD atlas probe ASCII panel L glyph is upright and not mirrored");
        constexpr int k_ascii_panel_placement_edge_tolerance_pixels = 1;
        constexpr double k_ascii_panel_placement_center_x_tolerance_pixels = 1.0;
        constexpr double k_ascii_panel_placement_center_y_tolerance_pixels = 0.5;
        ok &= check(
            ascii_panel_raw_atlas_placement.expected_cells > 0 &&
                ascii_panel_raw_atlas_placement.compared_cells ==
                    ascii_panel_raw_atlas_placement.expected_cells &&
                ascii_panel_raw_atlas_placement.all_cells_have_ink &&
                ascii_panel_raw_atlas_placement.max_left_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_right_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_top_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_bottom_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_first_ink_x_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_center_x_delta <=
                    k_ascii_panel_placement_center_x_tolerance_pixels &&
                ascii_panel_raw_atlas_placement.max_center_y_delta <=
                    k_ascii_panel_placement_center_y_tolerance_pixels,
            "LCD atlas probe ASCII panel MSDF cell placement matches the raw atlas reference");
        ok &= check(
            ascii_panel_cursor_text_raw_atlas_placement.expected_cells == 1 &&
                ascii_panel_cursor_text_raw_atlas_placement.compared_cells ==
                    ascii_panel_cursor_text_raw_atlas_placement.expected_cells &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .all_cells_have_visible_ink &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .max_visible_left_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .max_visible_right_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .max_visible_top_delta <=
                    k_ascii_panel_placement_edge_tolerance_pixels &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .max_visible_baseline_delta == 0 &&
                ascii_panel_cursor_text_raw_atlas_placement
                    .max_visible_weighted_center_y_delta <=
                    k_ascii_panel_placement_center_y_tolerance_pixels,
            "LCD atlas probe clipped cursor MSDF text matches the raw reference");
        constexpr double k_ascii_panel_user_text_jitter_tolerance_pixels = 0.5;
        constexpr double
            k_ascii_panel_user_text_visible_center_y_tolerance_pixels = 0.75;
        ok &= check(
            ascii_panel_user_text_raw_atlas_placement.expected_cells ==
                lcd_ascii_panel_user_text().size() &&
                ascii_panel_user_text_raw_atlas_placement.compared_cells ==
                    ascii_panel_user_text_raw_atlas_placement.expected_cells &&
                ascii_panel_user_text_raw_atlas_placement.all_cells_have_ink &&
                ascii_panel_user_text_raw_atlas_placement
                    .all_cells_have_visible_ink &&
                ascii_panel_user_text_raw_atlas_placement
                    .max_visible_baseline_delta == 0 &&
                ascii_panel_user_text_raw_atlas_placement
                    .signed_visible_baseline_delta_range == 0 &&
                ascii_panel_user_text_raw_atlas_placement
                    .signed_center_y_delta_range <=
                    k_ascii_panel_user_text_jitter_tolerance_pixels &&
                ascii_panel_user_text_raw_atlas_placement
                    .signed_visible_weighted_center_y_delta_range <=
                    k_ascii_panel_user_text_visible_center_y_tolerance_pixels,
            "LCD atlas probe build_msdf MSDF vertical jitter matches the raw atlas reference");
        ok &= check(
            ascii_panel_user_text_qt_text_placement.expected_cells ==
                static_cast<int>(
                    lcd_ascii_panel_user_text_visible_baseline_probe_labels()
                        .size()) &&
                ascii_panel_user_text_qt_text_placement.compared_cells ==
                    ascii_panel_user_text_qt_text_placement.expected_cells &&
                ascii_panel_user_text_qt_text_placement.all_cells_have_visible_ink,
            "LCD atlas probe build_msdf visible baseline comparison covers Qt reference cells");
        ok &= check(
                ascii_panel_user_text_qt_text_gutters.expected_pairs ==
                static_cast<int>(
                    lcd_ascii_panel_user_text_targeted_visible_gutter_pairs()
                        .size()) &&
                ascii_panel_user_text_qt_text_gutters.compared_pairs ==
                    ascii_panel_user_text_qt_text_gutters.expected_pairs &&
                ascii_panel_user_text_qt_text_gutters.all_pairs_visible,
            "LCD atlas probe build_msdf visible gutter comparison covers Qt reference pairs");
        constexpr int k_ascii_panel_user_text_gutter_tolerance_pixels = 2;
        ok &= check(
            ascii_panel_user_text_raw_atlas_gutters.expected_pairs ==
                static_cast<int>(
                    lcd_ascii_panel_user_text_targeted_visible_gutter_pairs()
                        .size()) &&
                ascii_panel_user_text_raw_atlas_gutters.compared_pairs ==
                    ascii_panel_user_text_raw_atlas_gutters.expected_pairs &&
                ascii_panel_user_text_raw_atlas_gutters.all_pairs_visible &&
                ascii_panel_user_text_raw_atlas_gutters.max_gutter_delta <=
                    k_ascii_panel_user_text_gutter_tolerance_pixels,
            "LCD atlas probe build_msdf visible gutters match the raw reference");
    }
    ok &= check(
        atlas.atlas_report.render.shaped_missing_string_indexes == 0 &&
            atlas.atlas_report.render.shaped_invalid_string_indexes == 0,
        "LCD atlas probe reports complete shaped glyph string-index ownership");
    ok &= check(
        atlas.atlas_report.render.dual_source_probe_shader_package_available,
        "LCD atlas probe reports loaded dual-source probe shader package");
    ok &= check(
        atlas.atlas_report.render.dual_source_blend_factors_available,
        "LCD atlas probe reports available dual-source blend factors");
    ok &= check(
        atlas.atlas_report.render.dual_source_blend_factors_runtime_probe,
        "LCD atlas probe runs the dual-source blend pipeline capability probe");
    ok &= check(
        write_lcd_probe_artifacts(
            fixture,
            single_w_fixture,
            repeated_w_fixture,
            ascii_panel_fixture,
            atlas,
            reference,
            qt_text_reference,
            single_w_atlas,
            single_w_raw_atlas_reference,
            single_w_qt_text_reference,
            repeated_w_atlas,
            translated_repeated_w_atlas,
            ascii_panel_atlas,
            ascii_panel_raw_atlas_reference,
            ascii_panel_qt_text_reference,
            repeated_w_translated_host,
            translated_repeated_w_stability,
            translated_w_origin_match,
            ascii_panel_stats,
            ascii_panel_raw_atlas_reference_stats,
            ascii_panel_qt_text_reference_stats,
            ascii_panel_raw_atlas_placement,
            ascii_panel_user_text_raw_atlas_placement,
            ascii_panel_user_text_qt_text_placement,
            ascii_panel_user_text_raw_atlas_gutters,
            ascii_panel_user_text_qt_text_gutters,
            single_w_msdf,
            raster_variants,
            metrics_placement,
            probe_records,
            backend),
        "LCD atlas probe writes requested artifacts");
    return ok ? 0 : 1;
}

int test_atlas_report(QGuiApplication& app, const char* backend)
{
    const int backend_status =
        verify_requested_backend(app, backend, "atlas report");
    if (backend_status != 0) {
        return backend_status;
    }

    const qreal device_pixel_ratio =
        pixel_probe_render_window_device_pixel_ratio(app);
    if (!atlas_report_backend_usable(app, device_pixel_ratio)) {
        return k_unsupported_backend_skip_return_code;
    }

    bool ok = true;
    ok &= run_atlas_report_case(
        app,
        "selection",
        [](VNM_TerminalSurface&, term::Terminal_render_snapshot& snapshot) {
            snapshot.selection_spans.push_back({
                {{0, 1}, {0, 3}, term::Terminal_selection_mode::NORMAL},
                0,
                1,
                2,
            });
        },
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.non_dirty_selection_invalidation;
        });
    ok &= run_atlas_report_case(
        app,
        "cursor",
        [](VNM_TerminalSurface&, term::Terminal_render_snapshot& snapshot) {
            snapshot.cursor.visible       = true;
            snapshot.cursor.blink_enabled = false;
            snapshot.cursor.position      = {1, 2};
        },
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.non_dirty_cursor_invalidation;
        });
    ok &= run_atlas_report_case(
        app,
        "preedit",
        [](VNM_TerminalSurface& surface, term::Terminal_render_snapshot&) {
            term::Ime_preedit_state state;
            state.text            = QStringLiteral("x");
            state.cursor_position = 1;
            state.active          = true;
            term::VNM_TerminalSurface_render_bridge::set_ime_preedit_state(
                surface,
                std::move(state));
        },
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.non_dirty_preedit_invalidation;
        },
        true);
    ok &= run_atlas_report_case(
        app,
        "options",
        [](VNM_TerminalSurface& surface, term::Terminal_render_snapshot&) {
            surface.set_cursor_style(VNM_TerminalSurface::Cursor_style::UNDERLINE);
        },
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.non_dirty_options_invalidation;
        },
        true);
    ok &= run_atlas_report_case(
        app,
        "visual_bell",
        [](VNM_TerminalSurface&, term::Terminal_render_snapshot& snapshot) {
            snapshot.metadata.visual_bell_active = true;
        },
        [](const term::Qsg_atlas_render_summary& render_summary) {
            return render_summary.non_dirty_visual_bell_invalidation;
        });
    ok &= test_atlas_glyph_row_stable_dirty_update(app);
    ok &= test_atlas_rect_row_stable_dense_graphic_update(app);
    ok &= test_atlas_rect_row_stable_graphic_arc_update(app);
    ok &= test_atlas_rect_row_stable_styled_blank_background_update(app);
    ok &= test_atlas_glyph_row_stable_wide_update(app);
    ok &= test_atlas_glyph_row_stable_combining_update(app);
    ok &= test_atlas_glyph_row_stable_cursor_dirty_update(app);
    ok &= test_atlas_glyph_row_stable_cursor_clean_row_promoted(app);
    ok &= test_atlas_prepared_text_reuse(app);
    ok &= test_atlas_failed_prepare_forces_next_sparse_full_upload(app);
    ok &= test_atlas_failed_prepare_preserves_font_epoch_basis(app);
    ok &= test_atlas_failed_prepare_preserves_cursor_layer_basis(app);
    ok &= test_atlas_msdf_resource_stability(app);
    ok &= test_atlas_msdf_zoom_reuses_baked_atlas(app);
    ok &= test_atlas_msdf_zoom_crosses_bake_bucket(app);

    const qreal dpr = pixel_normalized_device_pixel_ratio(device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = pixel_metrics(dpr);
    Pixel_parity_fixture public_scroll = make_layout_parity_base_fixture(
        "atlas_public_projection_scroll",
        {3, 8},
        920U,
        metrics,
        dpr);
    public_scroll.snapshot.basis =
        term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    public_scroll.snapshot.purpose =
        term::Terminal_render_snapshot_purpose::SCROLL;
    public_scroll.snapshot.styles.push_back(rgb_style(0xfff8f8f2U, 0xff204050U));
    public_scroll.snapshot.cells.push_back(
        make_pixel_cell(0, 0, QStringLiteral("A"), 1, term::k_default_terminal_style_id));
    public_scroll.snapshot.cells.push_back(
        make_pixel_cell(1, 1, QStringLiteral(" "), 1, 1U));
    public_scroll.snapshot.cells.push_back(
        make_pixel_cell(1, 2, QStringLiteral(" "), 1, 1U));
    const Pixel_render_result atlas =
        render_pixel_atlas_fixture(app, public_scroll);
    if (!atlas.ready) {
        std::cerr << "FAIL: atlas public projection report lost usable QRhi "
            << "render state after backend probe\n";
        return 1;
    }

    const term::Qsg_atlas_render_summary& render_summary = atlas.atlas_report.render;
    ok &= check(render_summary.full_dirty_range_reupload &&
            render_summary.public_projection_full_reupload &&
            render_summary.scroll_full_reupload,
        "atlas report marks PUBLIC_PROJECTION/SCROLL as full reupload");
    const bool public_scroll_msdf_active =
        render_summary.msdf_text_renderer_enabled &&
        render_summary.msdf_text_renderer_active;
    ok &= check(
        render_summary.text_renderer_policy ==
            term::Terminal_text_renderer_policy::AUTO,
        "atlas report records default text renderer policy");
    ok &= check(
        render_summary.effective_text_renderer ==
            (public_scroll_msdf_active
                ? term::Terminal_text_renderer_kind::MSDF
                : term::Terminal_text_renderer_kind::GLYPH),
        "atlas report records effective text renderer path");
    ok &= check(render_summary.text_renderer_fallback_allowed,
        "atlas report records auto text renderer fallback permission");
    ok &= check(
        render_summary.text_renderer_fallback_used == !public_scroll_msdf_active,
        "atlas report records default text renderer fallback use");
    ok &= check(
        render_summary.rect_buffer.full_repaint_upload &&
            render_summary.rect_buffer.full_upload,
        "atlas report routes PUBLIC_PROJECTION/SCROLL geometry through full rect upload");
    ok &= check(
        public_scroll_msdf_active
            ? render_summary.msdf_text_buffer.full_repaint_upload &&
                render_summary.msdf_text_buffer.full_upload
            : render_summary.glyph_buffer.full_repaint_upload &&
                render_summary.glyph_buffer.full_upload,
        "atlas report routes PUBLIC_PROJECTION/SCROLL text through full text upload");
    ok &= check(render_summary.background_rects_before_coalescing >
            render_summary.background_rects_after_coalescing,
        "atlas report records background coalescing");
    ok &= check(render_summary.draw_calls > 0 &&
            render_summary.rect_draw_calls > 0 &&
            (public_scroll_msdf_active
                ? render_summary.msdf_text_draw_calls > 0 &&
                    render_summary.glyph_draw_calls == 0
                : render_summary.glyph_draw_calls > 0),
        "atlas report records minimized non-empty draw calls");
    ok &= check(render_summary.atlas_page_budget >= 1 &&
            render_summary.atlas_page_bytes > 0U &&
            render_summary.atlas_budget_bytes >= render_summary.atlas_allocated_bytes,
        "atlas report records atlas memory and page budget counters");
    ok &= check(public_scroll_msdf_active ||
            atlas.atlas_report.frame_build.max_glyph_instance_page >= 0,
        "atlas report records rendered glyph page usage");
    ok &= check(atlas.atlas_report.frame_build.glyph_missed_instances == 0 &&
            atlas.atlas_report.frame_build.glyph_coverage_failures == 0 &&
            atlas.atlas_report.frame_build.glyph_atlas_insert_failures == 0,
        "atlas report records no silent glyph misses");
    ok &= test_atlas_auto_msdf_prepare_resource_failure_falls_back(app);
    ok &= test_atlas_auto_msdf_buffer_failure_falls_back(app);
    ok &= test_atlas_forced_msdf_prepare_resource_failure_does_not_commit(app);
    ok &= test_atlas_forced_msdf_buffer_failure_does_not_commit(app);
    return ok ? 0 : 1;
}

bool test_font_file_bytes_for_font()
{
    bool ok = true;

    // The bundled monospace family resolves to the embedded resource bytes.
    const QString bundled_family =
        term::vnm_terminal_default_monospace_font_family();
    const QFont bundled_font(bundled_family);
    const std::optional<QByteArray> bundled_bytes =
        term::font_file_bytes_for_font(bundled_font);
    ok &= check(bundled_bytes.has_value() && !bundled_bytes->isEmpty(),
        "font_file_bytes_for_font returns bytes for the bundled family");

#if VNM_TERMINAL_QSG_ATLAS_MSDF_DIAGNOSTIC_AVAILABLE
    QString font_error;
    const QByteArray resource_bytes = read_msdf_terminal_font_data(font_error);
    ok &= check(font_error.isEmpty(),
        "embedded terminal monospace font resource reads cleanly");
    if (bundled_bytes.has_value()) {
        ok &= check(*bundled_bytes == resource_bytes,
            "bundled font bytes match the embedded resource");
    }

    // An unknown family must resolve to nothing. The GDI resolver that
    // DirectWrite replaced let the font mapper substitute Arial for any family
    // it could not match, so MSDF baked a proportional font into monospace
    // cells (user-visible with "OCR A", whose DirectWrite family name differs
    // from its GDI face name "OCR A Extended").
    const QFont unknown_font(QStringLiteral("VNM No Such Family"));
    ok &= check(!term::font_file_bytes_for_font(unknown_font).has_value(),
        "an unknown family resolves to no bytes instead of a substitute font");

    // An arbitrary installed family must resolve to a full sfnt blob that
    // msdfgen accepts. Non-bundled resolution exists only on Windows
    // (DirectWrite); the other platforms return nullopt and MSDF stays
    // bundled-only there, so only assert the build path where we can resolve.
    const QFont system_font(QStringLiteral("Consolas"));
    const std::optional<QByteArray> system_bytes =
        term::font_file_bytes_for_font(system_font);
#if defined(Q_OS_WIN)
    // The producer must resolve bytes exactly when the platform font database
    // renders the requested family. When the platform resolves the family to
    // something else (the offscreen platform's database does not see installed
    // system fonts), the producer must refuse: MSDF would otherwise bake a
    // font the glyph path is not drawing.
    const QRawFont system_rendered = QRawFont::fromFont(system_font);
    const bool system_family_rendered = system_rendered.isValid() &&
        system_rendered.familyName() == system_font.family();
    if (system_family_rendered) {
        ok &= check(system_bytes.has_value() && !system_bytes->isEmpty(),
            "font_file_bytes_for_font resolves an installed system family");
    }
    else {
        ok &= check(!system_bytes.has_value(),
            "the producer refuses a family the platform is not rendering");
    }
    if (system_bytes.has_value() && !system_bytes->isEmpty() &&
        bundled_bytes.has_value())
    {
        // Distinct families must produce distinct bytes, hence distinct atlas
        // fingerprints, so their baked atlases never collide in the cache.
        ok &= check(*system_bytes != *bundled_bytes,
            "a non-bundled family resolves to bytes distinct from the bundled font");
    }
    if (system_bytes.has_value() && !system_bytes->isEmpty()) {
        // The resolved blob must be the same physical font Qt's glyph path
        // renders; a substituted file would bake one font while the glyph path
        // draws another.
        QRawFont resolved_raw;
        resolved_raw.loadFromData(*system_bytes, 16.0, QFont::PreferDefaultHinting);
        const QRawFont rendered_raw = QRawFont::fromFont(system_font);
        ok &= check(resolved_raw.isValid() && rendered_raw.isValid() &&
                resolved_raw.fontTable("head") == rendered_raw.fontTable("head"),
            "resolved system-font bytes are the same physical font Qt renders");
    }

    // Families whose DirectWrite name differs from their GDI face name were
    // the substitution victims; when such a family is installed and rendered
    // (OCR A is the reported case), its bytes must resolve and be the rendered
    // font's file.
    const QString decomposed_family = QStringLiteral("OCR A");
    if (QFontDatabase::families().contains(decomposed_family) &&
        QRawFont::fromFont(QFont(decomposed_family)).familyName() ==
            decomposed_family)
    {
        const QFont decomposed_font(decomposed_family);
        const std::optional<QByteArray> decomposed_bytes =
            term::font_file_bytes_for_font(decomposed_font);
        ok &= check(decomposed_bytes.has_value() && !decomposed_bytes->isEmpty(),
            "a DirectWrite-decomposed family name resolves to font bytes");
        if (decomposed_bytes.has_value() && !decomposed_bytes->isEmpty()) {
            QRawFont decomposed_raw;
            decomposed_raw.loadFromData(
                *decomposed_bytes, 16.0, QFont::PreferDefaultHinting);
            const QRawFont decomposed_rendered = QRawFont::fromFont(decomposed_font);
            ok &= check(decomposed_raw.isValid() && decomposed_rendered.isValid() &&
                    decomposed_raw.fontTable("head") ==
                        decomposed_rendered.fontTable("head"),
                "decomposed-family bytes are the same physical font Qt renders");
        }
    }
    if (system_bytes.has_value() && !system_bytes->isEmpty()) {
        // Build with an ASCII-core sample plus a powerline Private-Use glyph.
        // A standard font covers the core but not the powerline glyph; that is
        // precisely why the old full-inventory ready check disabled MSDF for
        // every font except the bundled powerline-patched one. The relaxed check
        // needs only the ASCII core, so MSDF now activates and powerline-bearing
        // runs fall back to the glyph renderer per run.
        constexpr char32_t k_powerline_codepoint = 0xe0a0U;
        const std::vector<char32_t> codepoints = {
            k_msdf_single_w_codepoint, k_powerline_codepoint};
        const msdf::build_result_t build = msdf::build_font_atlas(
            reinterpret_cast<const std::uint8_t*>(system_bytes->constData()),
            static_cast<std::size_t>(system_bytes->size()),
            48,
            std::span<const char32_t>(codepoints.data(), codepoints.size()),
            msdf_single_w_options());
        ok &= check(build.status != msdf::Build_status::FAILURE,
            "msdfgen builds an atlas from resolved system-font bytes");
        ok &= check(
            build.atlas.glyphs.find(k_msdf_single_w_codepoint) !=
                build.atlas.glyphs.end(),
            "system-font MSDF atlas contains the ASCII core (W) glyph");
        ok &= check(
            build.atlas.glyphs.find(k_powerline_codepoint) ==
                build.atlas.glyphs.end(),
            "a standard font lacks the powerline glyph the strict ready check required");
    }
#else
    (void)system_bytes;
#endif
#endif

    return ok;
}

bool run_unit_tests()
{
    bool ok = true;
    ok &= test_source_posture();
    ok &= test_font_file_bytes_for_font();
    ok &= test_cache_key_includes_physical_size_and_face();
    ok &= test_software_renderer_driver_classifier();
    ok &= test_packing_and_stride_copy();
    ok &= test_indexed8_and_grayscale_conversion();
    ok &= test_rgba_tile_model_preparation();
    ok &= test_glyph_coverage_candidate_classifier();
    ok &= test_glyph_image_diagnostics();
    ok &= test_shaped_glyph_physical_pixel_placement();
    ok &= test_cell_stable_ascii_layout_font();
    ok &= test_shaped_glyph_records();
    ok &= test_epoch_invalidation();
    ok &= test_atlas_rotating_buffer_planner();
    ok &= test_atlas_row_stable_glyph_planner();
    ok &= test_atlas_non_dirty_and_full_reupload_planner();
    ok &= test_msdf_text_buffer_fallback_retry_decision();
    ok &= test_msdf_text_prepare_fallback_retry_decision();
    ok &= test_msdf_text_failure_diagnostics_merge();
    ok &= test_atlas_msdf_text_path_adapter_skip_predicate();
    ok &= test_text_renderer_report_names();
    ok &= test_atlas_recorder_accumulates_elapsed_counters();
    ok &= test_atlas_budget_stats();
    ok &= test_atlas_warm_set_table_and_shaping();
    ok &= test_atlas_warm_set_representative_family_coverage();
    return ok;
}

}

int main(int argc, char** argv)
{
    const bool render_smoke = has_argument(argc, argv, "--render-smoke");
    const bool dense_grid_smoke = has_argument(argc, argv, "--dense-grid-smoke");
    const bool pixel_parity = has_argument(argc, argv, "--pixel-parity");
    const bool layout_parity = has_argument(argc, argv, "--layout-parity");
    const bool atlas_report = has_argument(argc, argv, "--atlas-report");
    const bool warm_lazy_smoke = has_argument(argc, argv, "--warm-lazy-smoke");
    const bool lcd_capability_probe =
        has_argument(argc, argv, "--lcd-capability-probe");
    const bool host_state_smoke = has_argument(argc, argv, "--host-state-smoke");
    const bool cursor_descender_smoke =
        has_argument(argc, argv, "--cursor-descender-smoke");
    const char* backend = argument_value(argc, argv, "--backend", "d3d11");
    const bool graphics_mode =
        render_smoke || dense_grid_smoke || pixel_parity || layout_parity ||
        atlas_report || warm_lazy_smoke || lcd_capability_probe ||
        host_state_smoke || cursor_descender_smoke;
    if (graphics_mode) {
        configure_graphics_api(backend);
    }
    else
    {
        configure_unit_test_qt_environment();
    }

    QGuiApplication app(argc, argv);
    if (dense_grid_smoke) {
        return test_dense_grid_smoke(app, backend);
    }
    if (cursor_descender_smoke) {
        return test_cursor_descender_clip(app, backend);
    }
    if (atlas_report) {
        return test_atlas_report(app, backend);
    }
    if (warm_lazy_smoke) {
        return test_atlas_warm_lazy_smoke(app, backend);
    }
    if (lcd_capability_probe) {
        return test_lcd_capability_probe(app, backend);
    }
    if (host_state_smoke) {
        return test_atlas_host_state_smoke(app, backend);
    }
    if (layout_parity) {
        return test_layout_parity(app, backend);
    }
    if (pixel_parity) {
        return test_pixel_parity(app, backend);
    }
    if (render_smoke) {
        return test_render_smoke(app, backend);
    }

    return run_unit_tests() ? 0 : 1;
}
