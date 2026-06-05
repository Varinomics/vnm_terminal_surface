#pragma once

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace vnm_terminal::internal {

constexpr qreal k_terminal_graphic_antialias_feather = 0.5;

inline qreal terminal_graphic_light_stroke(terminal_cell_metrics_t metrics)
{
    return std::max<qreal>(
        1.0,
        std::floor(std::min(metrics.width, metrics.height) * 0.12));
}

inline qreal terminal_graphic_heavy_stroke(terminal_cell_metrics_t metrics)
{
    return std::max<qreal>(
        terminal_graphic_light_stroke(metrics) + 1.0,
        std::floor(std::min(metrics.width, metrics.height) * 0.20));
}

inline void append_terminal_graphic_rect(
    std::vector<Terminal_render_rect>& rects,
    QRectF                             rect,
    const QColor&                      color,
    bool                               antialias = false)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || color.alpha() <= 0) {
        return;
    }

    rects.push_back({rect, color, antialias});
}

inline void append_terminal_graphic_arc(
    std::vector<Terminal_render_arc>&  arcs,
    Terminal_render_arc_kind           kind,
    const QRectF&                      cell,
    const QColor&                      color,
    qreal                              stroke)
{
    if (cell.width() <= 0.0 ||
        cell.height() <= 0.0 ||
        stroke <= 0.0       ||
        color.alpha() <= 0)
    {
        return;
    }

    arcs.push_back({kind, cell, color, stroke});
}

inline QRectF terminal_graphic_centered_horizontal_rect(
    const QRectF& cell,
    qreal         left,
    qreal         right,
    qreal         stroke)
{
    return QRectF(
        left,
        cell.center().y() - stroke * 0.5,
        right - left,
        stroke);
}

inline QRectF terminal_graphic_centered_vertical_rect(
    const QRectF& cell,
    qreal         top,
    qreal         bottom,
    qreal         stroke)
{
    return QRectF(
        cell.center().x() - stroke * 0.5,
        top,
        stroke,
        bottom - top);
}

inline void append_terminal_box_drawing_rects(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    const QColor&                      color,
    bool                               connects_left,
    bool                               connects_right,
    bool                               connects_up,
    bool                               connects_down,
    qreal                              stroke)
{
    const qreal center_x = cell.center().x();
    const qreal center_y = cell.center().y();

    if (connects_left || connects_right) {
        const qreal left  = connects_left ? cell.left() : center_x;
        const qreal right = connects_right ? cell.right() : center_x;
        append_terminal_graphic_rect(
            rects,
            terminal_graphic_centered_horizontal_rect(cell, left, right, stroke),
            color,
            true);
    }
    if (connects_up || connects_down) {
        const qreal top    = connects_up ? cell.top() : center_y;
        const qreal bottom = connects_down ? cell.bottom() : center_y;
        append_terminal_graphic_rect(
            rects,
            terminal_graphic_centered_vertical_rect(cell, top, bottom, stroke),
            color,
            true);
    }
}

constexpr unsigned int k_terminal_box_left  = 0x01U;
constexpr unsigned int k_terminal_box_right = 0x02U;
constexpr unsigned int k_terminal_box_up    = 0x04U;
constexpr unsigned int k_terminal_box_down  = 0x08U;

struct terminal_box_stroke_spec_t
{
    ushort       codepoint   = 0U;
    unsigned int connections = 0U;
    bool         heavy       = false;
};

struct terminal_box_arc_spec_t
{
    ushort                   codepoint = 0U;
    Terminal_render_arc_kind kind      = Terminal_render_arc_kind::DOWN_RIGHT;
};

inline constexpr terminal_box_stroke_spec_t k_terminal_box_stroke_specs[] = {
    {0x2500, k_terminal_box_left | k_terminal_box_right, false},
    {0x2501, k_terminal_box_left | k_terminal_box_right, true},
    {0x2502, k_terminal_box_up | k_terminal_box_down, false},
    {0x2503, k_terminal_box_up | k_terminal_box_down, true},
    {0x250c, k_terminal_box_right | k_terminal_box_down, false},
    {0x250f, k_terminal_box_right | k_terminal_box_down, true},
    {0x2510, k_terminal_box_left | k_terminal_box_down, false},
    {0x2513, k_terminal_box_left | k_terminal_box_down, true},
    {0x2514, k_terminal_box_right | k_terminal_box_up, false},
    {0x2517, k_terminal_box_right | k_terminal_box_up, true},
    {0x2518, k_terminal_box_left | k_terminal_box_up, false},
    {0x251b, k_terminal_box_left | k_terminal_box_up, true},
    {0x251c, k_terminal_box_right | k_terminal_box_up | k_terminal_box_down, false},
    {0x2523, k_terminal_box_right | k_terminal_box_up | k_terminal_box_down, true},
    {0x2524, k_terminal_box_left | k_terminal_box_up | k_terminal_box_down, false},
    {0x252b, k_terminal_box_left | k_terminal_box_up | k_terminal_box_down, true},
    {0x252c, k_terminal_box_left | k_terminal_box_right | k_terminal_box_down, false},
    {0x2533, k_terminal_box_left | k_terminal_box_right | k_terminal_box_down, true},
    {0x2534, k_terminal_box_left | k_terminal_box_right | k_terminal_box_up, false},
    {0x253b, k_terminal_box_left | k_terminal_box_right | k_terminal_box_up, true},
    {0x253c, k_terminal_box_left | k_terminal_box_right | k_terminal_box_up |
         k_terminal_box_down, false},
    {0x254b, k_terminal_box_left | k_terminal_box_right | k_terminal_box_up |
         k_terminal_box_down, true},
    {0x2574, k_terminal_box_left, false},
    {0x2575, k_terminal_box_up, false},
    {0x2576, k_terminal_box_right, false},
    {0x2577, k_terminal_box_down, false},
};

inline constexpr terminal_box_arc_spec_t k_terminal_box_arc_specs[] = {
    {0x256d, Terminal_render_arc_kind::DOWN_RIGHT},
    {0x256e, Terminal_render_arc_kind::DOWN_LEFT},
    {0x256f, Terminal_render_arc_kind::UP_LEFT},
    {0x2570, Terminal_render_arc_kind::UP_RIGHT},
};

inline constexpr bool terminal_box_connects(
    unsigned int connections,
    unsigned int connection)
{
    return (connections & connection) != 0U;
}

inline const terminal_box_stroke_spec_t* find_terminal_box_stroke_spec(
    ushort codepoint)
{
    for (const terminal_box_stroke_spec_t& spec : k_terminal_box_stroke_specs) {
        if (spec.codepoint == codepoint) {
            return &spec;
        }
    }

    return nullptr;
}

inline const terminal_box_arc_spec_t* find_terminal_box_arc_spec(ushort codepoint)
{
    for (const terminal_box_arc_spec_t& spec : k_terminal_box_arc_specs) {
        if (spec.codepoint == codepoint) {
            return &spec;
        }
    }

    return nullptr;
}

inline bool append_terminal_box_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    if (const terminal_box_stroke_spec_t* spec =
            find_terminal_box_stroke_spec(codepoint);
        spec != nullptr)
    {
        const qreal stroke = spec->heavy
            ? terminal_graphic_heavy_stroke(metrics)
            : terminal_graphic_light_stroke(metrics);

        append_terminal_box_drawing_rects(
            rects,
            cell,
            color,
            terminal_box_connects(spec->connections, k_terminal_box_left),
            terminal_box_connects(spec->connections, k_terminal_box_right),
            terminal_box_connects(spec->connections, k_terminal_box_up),
            terminal_box_connects(spec->connections, k_terminal_box_down),
            stroke);
        return true;
    }

    if (const terminal_box_arc_spec_t* spec = find_terminal_box_arc_spec(codepoint);
        spec != nullptr)
    {
        append_terminal_graphic_arc(
            arcs,
            spec->kind,
            cell,
            color,
            terminal_graphic_light_stroke(metrics));
        return true;
    }

    return false;
}

inline void append_terminal_quadrant_rect(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    const QColor&                      color,
    bool                               upper,
    bool                               left)
{
    const qreal half_width  = cell.width() * 0.5;
    const qreal half_height = cell.height() * 0.5;
    append_terminal_graphic_rect(
        rects,
        QRectF(
            left ? cell.left() : cell.left() + half_width,
            upper ? cell.top() : cell.top() + half_height,
            half_width,
            half_height),
        color);
}

inline QColor terminal_shade_graphic_color(QColor color, qreal coverage)
{
    color.setAlpha(std::clamp(
        static_cast<int>(std::round(static_cast<qreal>(color.alpha()) * coverage)),
        0,
        255));
    return color;
}

inline bool append_terminal_block_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color)
{
    switch (codepoint) {
        case 0x2580:
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.top(), cell.width(), cell.height() * 0.5),
                color);
            return true;
        case 0x2581:
        case 0x2582:
        case 0x2583:
        case 0x2584:
        case 0x2585:
        case 0x2586:
        case 0x2587: {
            const qreal eighth_count = static_cast<qreal>(codepoint - 0x2580);
            const qreal height       = cell.height() * eighth_count / 8.0;
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.bottom() - height, cell.width(), height),
                color);
            return true;
        }
        case 0x2588:
            append_terminal_graphic_rect(rects, cell, color);
            return true;
        case 0x2589:
        case 0x258a:
        case 0x258b:
        case 0x258c:
        case 0x258d:
        case 0x258e:
        case 0x258f: {
            const qreal eighth_count = static_cast<qreal>(0x2590 - codepoint);
            append_terminal_graphic_rect(
                rects,
                QRectF(
                    cell.left(),
                    cell.top(),
                    cell.width() * eighth_count / 8.0,
                    cell.height()),
                color);
            return true;
        }
        case 0x2590:
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.center().x(), cell.top(), cell.width() * 0.5, cell.height()),
                color);
            return true;
        case 0x2591:
            append_terminal_graphic_rect(
                rects,
                cell,
                terminal_shade_graphic_color(color, 0.25));
            return true;
        case 0x2592:
            append_terminal_graphic_rect(
                rects,
                cell,
                terminal_shade_graphic_color(color, 0.50));
            return true;
        case 0x2593:
            append_terminal_graphic_rect(
                rects,
                cell,
                terminal_shade_graphic_color(color, 0.75));
            return true;
        case 0x2594:
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.top(), cell.width(), cell.height() / 8.0),
                color);
            return true;
        case 0x2595:
            append_terminal_graphic_rect(
                rects,
                QRectF(
                    cell.right() - cell.width() / 8.0,
                    cell.top(),
                    cell.width() / 8.0,
                    cell.height()),
                color);
            return true;
        case 0x2596:
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x2597:
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x2598:
            append_terminal_quadrant_rect(rects, cell, color, true, true);
            return true;
        case 0x2599:
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259a:
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259b:
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x259c:
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259d:
            append_terminal_quadrant_rect(rects, cell, color, true, false);
            return true;
        case 0x259e:
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x259f:
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        default:
            return false;
    }
}

inline bool append_terminal_graphic_codepoint_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    if (codepoint < 0x2500U || codepoint > 0x259fU) {
        return false;
    }

    if (codepoint >= 0x2580U) {
        return append_terminal_block_graphic_rects(rects, cell, codepoint, color);
    }

    return append_terminal_box_graphic_rects(
        rects,
        arcs,
        cell,
        codepoint,
        color,
        metrics);
}

inline bool append_terminal_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    const Terminal_render_cell_text&   text,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    const std::optional<ushort> codepoint = text.single_code_unit();
    if (!codepoint.has_value()) {
        return false;
    }

    return append_terminal_graphic_codepoint_rects(
        rects,
        arcs,
        cell,
        *codepoint,
        color,
        metrics);
}

inline bool is_terminal_graphic_text(const Terminal_render_cell_text& text)
{
    const std::optional<ushort> codepoint = text.single_code_unit();
    if (!codepoint.has_value()) {
        return false;
    }

    if (*codepoint >= 0x2580U && *codepoint <= 0x259fU) {
        return true;
    }

    return
        find_terminal_box_stroke_spec(*codepoint) != nullptr ||
        find_terminal_box_arc_spec(*codepoint)    != nullptr;
}

struct terminal_render_arc_geometry_t
{
    QPointF center;
    qreal   start_angle = 0.0;
    qreal   end_angle   = 0.0;
};

inline constexpr qreal terminal_graphic_pi()
{
    return 3.14159265358979323846;
}

inline terminal_render_arc_geometry_t terminal_render_arc_geometry(
    const Terminal_render_arc& arc)
{
    const QRectF& cell = arc.rect;
    switch (arc.kind) {
        case Terminal_render_arc_kind::DOWN_RIGHT:
            return {{cell.right(), cell.bottom()}, terminal_graphic_pi(), terminal_graphic_pi() * 1.5};
        case Terminal_render_arc_kind::DOWN_LEFT:
            return {{cell.left(), cell.bottom()}, terminal_graphic_pi() * 1.5, terminal_graphic_pi() * 2.0};
        case Terminal_render_arc_kind::UP_LEFT:
            return {{cell.left(), cell.top()}, 0.0, terminal_graphic_pi() * 0.5};
        case Terminal_render_arc_kind::UP_RIGHT:
            return {{cell.right(), cell.top()}, terminal_graphic_pi() * 0.5, terminal_graphic_pi()};
    }

    return {{cell.right(), cell.bottom()}, terminal_graphic_pi(), terminal_graphic_pi() * 1.5};
}

inline QPointF terminal_render_arc_point_at(
    const Terminal_render_arc&            arc,
    const terminal_render_arc_geometry_t& arc_spec,
    qreal                                 angle)
{
    const qreal radius_x = arc.rect.width() * 0.5;
    const qreal radius_y = arc.rect.height() * 0.5;
    return QPointF(
        arc_spec.center.x() + std::cos(angle) * radius_x,
        arc_spec.center.y() + std::sin(angle) * radius_y);
}

inline bool terminal_render_angle_is_inside_arc(
    qreal                                 angle,
    const terminal_render_arc_geometry_t& arc_spec)
{
    return angle >= arc_spec.start_angle && angle <= arc_spec.end_angle;
}

inline qreal terminal_render_arc_pixel_coverage(
    const Terminal_render_arc&            arc,
    const terminal_render_arc_geometry_t& arc_spec,
    QPointF                               point)
{
    const qreal radius_x = arc.rect.width() * 0.5;
    const qreal radius_y = arc.rect.height() * 0.5;
    if (radius_x <= 0.0 || radius_y <= 0.0) {
        return 0.0;
    }

    qreal angle = std::atan2(
        (point.y() - arc_spec.center.y()) / radius_y,
        (point.x() - arc_spec.center.x()) / radius_x);
    if (angle < 0.0) {
        angle += terminal_graphic_pi() * 2.0;
    }
    if (!terminal_render_angle_is_inside_arc(angle, arc_spec)) {
        return 0.0;
    }

    const QPointF curve_point = terminal_render_arc_point_at(arc, arc_spec, angle);
    const qreal distance = std::hypot(
        point.x() - curve_point.x(),
        point.y() - curve_point.y());
    return std::clamp(
        arc.stroke * 0.5 + k_terminal_graphic_antialias_feather - distance,
        0.0,
        1.0);
}

inline QColor terminal_render_coverage_color(QColor color, qreal coverage)
{
    color.setAlpha(std::clamp(
        static_cast<int>(std::round(static_cast<qreal>(color.alpha()) * coverage)),
        0,
        255));
    return color;
}

}
