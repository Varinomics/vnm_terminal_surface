// Oracles for these fixtures:
//   - Qt documents one physical wheel notch as 120 units of angle delta, which fixes the
//     angle-to-notch conversion and the sub-notch remainder carry.
//   - The zoom guard is fixed by the fontSize property contract, not by taste:
//     normalized_font_pixel_size passes non-finite and non-positive sizes through
//     unchanged, so fontSize can hold NaN, zero, a negative, or an infinity, and the zoom
//     has to produce a size the terminal can render from any of them. The surface has
//     applied that guard since before this seam existed; the fixtures pin it so the
//     scrollbar, which is the other caller, cannot drift away from it again.
//   - A zoom notch exists to change what the terminal shows. Shaped glyphs draw from the
//     raw font Qt resolves for the render font, at a whole pixel size; MSDF glyphs draw
//     from the snapped cell ascent; both lay out in the cell. The zoom fixtures require
//     every notch to change what each path the text renderer can use consumes, as the
//     renderer accounts for it. They measure the shipped face and the platform's own
//     monospace face with this platform's rasterizer, at the display scales and under
//     the advance policies and text renderers the terminal offers, rather than naming
//     sizes that happen to collide on one platform.

#include "vnm_terminal/font_metrics.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/wheel_gesture.h"
#include "helpers/test_check.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QPoint>
#include <QPointF>
#include <QStandardPaths>
#include <QStringList>
#include <QWheelEvent>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

QWheelEvent make_wheel_event(QPoint angle_delta, QPoint pixel_delta)
{
    return QWheelEvent(
        QPointF(0.0, 0.0),
        QPointF(0.0, 0.0),
        pixel_delta,
        angle_delta,
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
}

bool test_angle_delta_steps()
{
    bool  ok              = true;
    qreal angle_remainder = 0.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent one_notch = make_wheel_event(QPoint(0, 120), QPoint());
    ok &= check(
        term::vertical_wheel_steps(
            one_notch, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder) == 1,
        "one notch of angle delta is one step");
    ok &= check(angle_remainder == 0.0, "a whole notch leaves no remainder");

    const QWheelEvent three_notches = make_wheel_event(QPoint(0, -360), QPoint());
    ok &= check(
        term::vertical_wheel_steps(
            three_notches, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder)
            == -3,
        "three notches backwards are three negative steps");
    return ok;
}

bool test_sub_notch_deltas_accumulate()
{
    bool  ok              = true;
    qreal angle_remainder = 0.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent partial = make_wheel_event(QPoint(0, 40), QPoint());
    for (int i = 0; i < 2; ++i) {
        ok &= check(
            term::vertical_wheel_steps(
                partial, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder)
                == 0,
            "a sub-notch delta yields no step yet");
    }

    ok &= check(
        term::vertical_wheel_steps(
            partial, term::k_angle_delta_per_wheel_step, angle_remainder, pixel_remainder) == 1,
        "accumulated sub-notch deltas reach a whole step");
    return ok;
}

bool test_pixel_delta_uses_the_caller_step_size()
{
    bool  ok              = true;
    qreal angle_remainder = 7.0;
    qreal pixel_remainder = 0.0;

    const QWheelEvent pixels = make_wheel_event(QPoint(), QPoint(0, 45));
    ok &= check(
        term::vertical_wheel_steps(pixels, 15.0, angle_remainder, pixel_remainder) == 3,
        "pixel delta is divided by the caller's step size");
    ok &= check(
        angle_remainder == 0.0,
        "entering the pixel route clears the angle remainder");

    const QWheelEvent none = make_wheel_event(QPoint(), QPoint());
    ok &= check(
        term::vertical_wheel_steps(none, 15.0, angle_remainder, pixel_remainder) == 0,
        "an event with no vertical delta yields no step");
    ok &= check(
        !term::has_vertical_wheel_delta(none),
        "an event with no vertical delta reports none");
    ok &= check(
        term::has_vertical_wheel_delta(pixels),
        "an event with a pixel delta reports a vertical delta");
    return ok;
}

// Under SNAP_ADVANCE_UP and the glyph renderer every whole size draws from its own
// raster font, so a notch is one pixel. That isolates the clamping and the guard from
// any platform's metrics.
qreal zoom_rendering_every_size(qreal current_font_size, int steps)
{
    return term::font_size_after_wheel_zoom(
        current_font_size,
        steps,
        vnm_terminal::default_monospace_font_family(),
        1.0,
        vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP,
        vnm_terminal::k_default_logical_dpi,
        term::Terminal_text_renderer_policy::GLYPH);
}

bool test_zoom_steps_from_a_renderable_font_size()
{
    const qreal default_size = static_cast<qreal>(term::k_vnm_terminal_default_font_pixel_size);

    bool ok = true;
    ok &= check(
        zoom_rendering_every_size(13.0, 1) == 14.0,
        "a positive size steps by one pixel per notch");
    ok &= check(
        zoom_rendering_every_size(13.0, -1) == 12.0,
        "a negative step count zooms out");
    ok &= check(
        zoom_rendering_every_size(6.0, -1) == 6.0,
        "zooming out is clamped at the minimum");
    ok &= check(
        zoom_rendering_every_size(72.0, 1) == 72.0,
        "zooming in is clamped at the maximum");

    // fontSize passes these through, so the zoom must recover from every one of them.
    ok &= check(
        zoom_rendering_every_size(0.0, 1) == default_size + 1.0,
        "a zero size zooms from the default size");
    ok &= check(
        zoom_rendering_every_size(-4.0, 1) == default_size + 1.0,
        "a negative size zooms from the default size");
    ok &= check(
        zoom_rendering_every_size(std::numeric_limits<qreal>::quiet_NaN(), 1)
            == default_size + 1.0,
        "a NaN size zooms from the default size instead of staying NaN");
    ok &= check(
        zoom_rendering_every_size(std::numeric_limits<qreal>::infinity(), -1)
            == default_size - 1.0,
        "an infinite size zooms from the default size instead of the maximum");
    return ok;
}

struct zoom_display_t
{
    const char* name;
    qreal       device_pixel_ratio;
    qreal       logical_dpi;
};

// The display scales the terminal runs on: 100% at the Windows and macOS logical DPI,
// the fractional Windows scales, and a Retina display.
const zoom_display_t k_zoom_displays[] = {
    {"100% at 96 dpi",  1.0,  96.0},
    {"100% at 72 dpi",  1.0,  72.0},
    {"125% at 120 dpi", 1.25, 120.0},
    {"150% at 144 dpi", 1.5,  144.0},
    {"175% at 168 dpi", 1.75, 168.0},
    {"200% at 72 dpi",  2.0,  72.0},
};

const vnm_terminal::Font_advance_policy k_zoom_advance_policies[] = {
    vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE,
    vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP,
    vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST,
};

const term::Terminal_text_renderer_policy k_zoom_renderer_policies[] = {
    term::Terminal_text_renderer_policy::GLYPH,
    term::Terminal_text_renderer_policy::MSDF,
    term::Terminal_text_renderer_policy::AUTO,
};

// The shipped face, and the platform's own monospace face where it is installed:
// Consolas on Windows, whose file the offscreen font database has to be handed,
// Menlo on macOS and DejaVu Sans Mono on Linux. Their hinting differs from the shipped
// face's, so they reach sizes that draw alike on one path and not on another.
QStringList installed_zoom_test_families()
{
    const QString consolas_file =
        QStandardPaths::locate(QStandardPaths::FontsLocation, QStringLiteral("consola.ttf"));
    if (!consolas_file.isEmpty()) {
        QFontDatabase::addApplicationFont(consolas_file);
    }

    QStringList families{vnm_terminal::default_monospace_font_family()};
    for (const QString& family : {
             QStringLiteral("Consolas"),
             QStringLiteral("Menlo"),
             QStringLiteral("DejaVu Sans Mono")})
    {
        if (QFontInfo(QFont(family)).family() == family) {
            families.push_back(family);
        }
    }
    return families;
}

const QStringList& zoom_test_families()
{
    static const QStringList families = installed_zoom_test_families();
    return families;
}

// How the terminal is set up to draw text.
struct Zoom_text_setup
{
    QString                             family;
    vnm_terminal::Font_advance_policy   advance_policy;
    term::Terminal_text_renderer_policy renderer_policy;
    zoom_display_t                      display;
};

std::string describe(const Zoom_text_setup& setup)
{
    const auto advance_name = [](vnm_terminal::Font_advance_policy policy) {
        switch (policy) {
            case vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE:     return "adjust";
            case vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP:      return "snap up";
            case vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST: return "snap nearest";
        }
        return "?";
    };
    const auto renderer_name = [](term::Terminal_text_renderer_policy policy) {
        switch (policy) {
            case term::Terminal_text_renderer_policy::GLYPH: return "glyph";
            case term::Terminal_text_renderer_policy::MSDF:  return "MSDF";
            case term::Terminal_text_renderer_policy::AUTO:  return "auto";
        }
        return "?";
    };

    std::ostringstream text;
    text
        << setup.family.toStdString() << ", " << advance_name(setup.advance_policy)
        << ", " << renderer_name(setup.renderer_policy) << ", " << setup.display.name;
    return text.str();
}

// What a requested size draws at: its effective size and, from the renderer's account
// of them, the sizes each text path consumes.
struct zoom_drawing_t
{
    qreal                             effective_size;
    term::qsg_atlas_text_draw_sizes_t sizes;
};

zoom_drawing_t drawing_of(const Zoom_text_setup& setup, qreal font_size)
{
    const qreal effective_size = vnm_terminal::effective_font_size_for_font(
        setup.family,
        font_size,
        setup.display.device_pixel_ratio,
        setup.advance_policy,
        setup.display.logical_dpi);
    return {
        effective_size,
        term::qsg_atlas_text_draw_sizes(
            term::vnm_terminal_font(setup.family, effective_size, setup.display.logical_dpi),
            setup.display.device_pixel_ratio,
            setup.advance_policy,
            setup.display.logical_dpi),
    };
}

std::string describe(const zoom_drawing_t& drawing)
{
    const term::terminal_cell_metrics_t& cell = drawing.sizes.cell_metrics;
    std::ostringstream text;
    text
        << "raster " << drawing.sizes.raster_pixel_size
        << " px, MSDF " << drawing.sizes.msdf_draw_pixel_height
        << " px, cell " << cell.width << 'x' << cell.height << " ascent " << cell.ascent
        << " (effective " << drawing.effective_size << ')';
    return text.str();
}

// Every path the renderer policy can draw a run with must draw it differently: shaped
// glyphs follow the raster font, MSDF glyphs the draw height, and both the cell.
bool draws_differently(
    const Zoom_text_setup& setup,
    const zoom_drawing_t&  lhs,
    const zoom_drawing_t&  rhs)
{
    const term::qsg_atlas_text_draw_sizes_t& a = lhs.sizes;
    const term::qsg_atlas_text_draw_sizes_t& b = rhs.sizes;
    const bool cell_differs =
        a.cell_metrics.width   != b.cell_metrics.width   ||
        a.cell_metrics.height  != b.cell_metrics.height  ||
        a.cell_metrics.ascent  != b.cell_metrics.ascent  ||
        a.cell_metrics.descent != b.cell_metrics.descent;
    const bool glyphs_differ = cell_differs || a.raster_pixel_size      != b.raster_pixel_size;
    const bool msdf_differs  = cell_differs || a.msdf_draw_pixel_height != b.msdf_draw_pixel_height;
    switch (setup.renderer_policy) {
        case term::Terminal_text_renderer_policy::GLYPH: return glyphs_differ;
        case term::Terminal_text_renderer_policy::MSDF:  return msdf_differs;
        case term::Terminal_text_renderer_policy::AUTO:  break;
    }
    return glyphs_differ && msdf_differs;
}

qreal zoom(const Zoom_text_setup& setup, qreal font_size, int steps)
{
    return term::font_size_after_wheel_zoom(
        font_size,
        steps,
        setup.family,
        setup.display.device_pixel_ratio,
        setup.advance_policy,
        setup.display.logical_dpi,
        setup.renderer_policy);
}

struct Zoom_walk
{
    std::vector<qreal>          font_sizes;
    std::vector<zoom_drawing_t> drawings;
};

// Zooms one notch at a time from `font_size` until a notch leaves the size unchanged,
// recording every size reached and how it draws.
Zoom_walk walk_zoom(const Zoom_text_setup& setup, qreal font_size, int direction)
{
    // One notch per whole size of the range is the longest walk that can still be
    // converging.
    const int notch_limit = static_cast<int>(
        term::k_font_zoom_max_pixel_size - term::k_font_zoom_min_pixel_size) + 1;

    Zoom_walk walk;
    for (int notch = 0; notch <= notch_limit; ++notch) {
        walk.font_sizes.push_back(font_size);
        walk.drawings.push_back(drawing_of(setup, font_size));
        const qreal next_font_size = zoom(setup, font_size, direction);
        if (next_font_size == font_size) {
            break;
        }
        font_size = next_font_size;
    }
    return walk;
}

bool check_every_notch_changes_the_drawing(
    const Zoom_text_setup& setup,
    const Zoom_walk&       walk,
    const char*            direction)
{
    bool ok = true;
    for (std::size_t notch = 1; notch < walk.font_sizes.size(); ++notch) {
        const zoom_drawing_t& before = walk.drawings[notch - 1];
        const zoom_drawing_t& after  = walk.drawings[notch];
        if (draws_differently(setup, before, after)) {
            continue;
        }

        std::cerr
            << "FAIL: zoom " << direction << " (" << describe(setup) << "): the notch "
            << walk.font_sizes[notch - 1] << " -> " << walk.font_sizes[notch]
            << " draws at " << describe(before) << " -> " << describe(after) << '\n';
        ok = false;
    }
    return ok;
}

bool test_every_zoom_notch_changes_the_drawing()
{
    bool ok = true;
    for (const QString& family : zoom_test_families()) {
        for (const vnm_terminal::Font_advance_policy advance_policy : k_zoom_advance_policies) {
            for (const term::Terminal_text_renderer_policy renderer_policy :
                     k_zoom_renderer_policies)
            {
                for (const zoom_display_t& display : k_zoom_displays) {
                    const Zoom_text_setup setup{family, advance_policy, renderer_policy, display};
                    const auto check_setup = [&setup](bool condition, const char* message) {
                        return check(condition, std::string(message) + " (" + describe(setup) + ')');
                    };

                    const Zoom_walk zoom_in  =
                        walk_zoom(setup, term::k_font_zoom_min_pixel_size,  1);
                    const Zoom_walk zoom_out =
                        walk_zoom(setup, zoom_in.font_sizes.back(),       -1);
                    ok &= check_every_notch_changes_the_drawing(setup, zoom_in,  "in");
                    ok &= check_every_notch_changes_the_drawing(setup, zoom_out, "out");

                    const zoom_drawing_t maximum =
                        drawing_of(setup, term::k_font_zoom_max_pixel_size);
                    const zoom_drawing_t minimum =
                        drawing_of(setup, term::k_font_zoom_min_pixel_size);
                    ok &= check_setup(
                        !draws_differently(setup, zoom_in.drawings.back(),  maximum),
                        "zooming in stops only where the maximum size draws");
                    ok &= check_setup(
                        !draws_differently(setup, zoom_out.drawings.back(), minimum),
                        "zooming out stops only where the minimum size draws");

                    // Zooming in settles on the largest size of each drawing, so zooming
                    // out retraces those sizes exactly; only the size the walk started
                    // from need not be one of them.
                    const std::size_t count = zoom_in.font_sizes.size();
                    bool retraces_drawings = zoom_out.font_sizes.size() == count;
                    bool retraces_sizes    = retraces_drawings;
                    for (std::size_t i = 0; retraces_drawings && i < count; ++i) {
                        const std::size_t mirrored = count - 1 - i;
                        retraces_drawings = !draws_differently(
                            setup, zoom_out.drawings[i], zoom_in.drawings[mirrored]);
                        if (mirrored != 0) {
                            retraces_sizes = retraces_sizes &&
                                zoom_out.font_sizes[i] == zoom_in.font_sizes[mirrored];
                        }
                    }
                    ok &= check_setup(retraces_drawings,
                        "zooming out passes the drawings zooming in passed, in reverse");
                    ok &= check_setup(retraces_drawings && retraces_sizes,
                        "zooming out returns to the sizes zooming in settled on");
                }
            }
        }
    }
    return ok;
}

// Looks for neighbouring whole sizes that draw alike in one respect and differently in
// another, and requires a notch from either of them to change the drawing, unless no
// differing drawing is left in that direction within the zoom range. Prints how many
// pairs each setup has, so a run shows where the case was exercised.
template <typename Pair_predicate>
bool check_zoom_across_pairs(
    const Zoom_text_setup& setup,
    Pair_predicate         is_pair,
    const char*            pair_name)
{
    const zoom_drawing_t minimum = drawing_of(setup, term::k_font_zoom_min_pixel_size);
    const zoom_drawing_t maximum = drawing_of(setup, term::k_font_zoom_max_pixel_size);

    bool        ok    = true;
    int         pairs = 0;
    std::string first_pair;
    for (qreal smaller_size = term::k_font_zoom_min_pixel_size;
         smaller_size < term::k_font_zoom_max_pixel_size;
         smaller_size += 1.0)
    {
        const qreal          larger_size = smaller_size + 1.0;
        const zoom_drawing_t smaller     = drawing_of(setup, smaller_size);
        const zoom_drawing_t larger      = drawing_of(setup, larger_size);
        if (!is_pair(smaller, larger, smaller_size, larger_size)) {
            continue;
        }

        ++pairs;
        if (first_pair.empty()) {
            std::ostringstream text;
            text
                << smaller_size << " and " << larger_size << " draw at "
                << describe(smaller) << " and " << describe(larger);
            first_pair = text.str();
        }

        const auto check_pair = [&](bool condition, const char* message) {
            if (!condition) {
                std::cerr
                    << "FAIL: " << message << " (" << describe(setup) << ") for "
                    << smaller_size << " and " << larger_size << '\n';
            }
            return condition;
        };
        if (draws_differently(setup, larger, maximum)) {
            ok &= check_pair(
                draws_differently(setup, smaller, drawing_of(setup, zoom(setup, smaller_size, 1))),
                "zooming in across the pair changes the drawing");
        }
        if (draws_differently(setup, smaller, minimum)) {
            ok &= check_pair(
                draws_differently(setup, larger, drawing_of(setup, zoom(setup, larger_size, -1))),
                "zooming out across the pair changes the drawing");
        }
    }

    if (pairs > 0) {
        std::cout
            << pair_name << " pairs (" << describe(setup) << "): " << pairs
            << ", first " << first_pair << '\n';
    }
    return ok;
}

// The adjustment keeps a requested size whose advance already lands on a device pixel,
// and otherwise searches for the size whose advance reaches the next pixel. That search
// can end just below a whole pixel size Qt rounds up to, so a request kept at its own
// size and its smaller neighbour, adjusted up, can select one raster font through
// effective sizes that differ.
bool test_zoom_crosses_the_integral_advance_boundary()
{
    bool ok = true;
    for (const zoom_display_t& display : k_zoom_displays) {
        const Zoom_text_setup setup{
            vnm_terminal::default_monospace_font_family(),
            vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE,
            term::Terminal_text_renderer_policy::GLYPH,
            display};
        ok &= check_zoom_across_pairs(
            setup,
            [](const zoom_drawing_t& smaller,
               const zoom_drawing_t& larger,
               qreal                 smaller_size,
               qreal                 larger_size)
            {
                return
                    smaller.effective_size != smaller_size &&
                    larger.effective_size  == larger_size  &&
                    smaller.sizes.raster_pixel_size == larger.sizes.raster_pixel_size;
            },
            "integral-advance");
    }
    return ok;
}

// The MSDF path scales glyphs to the snapped cell ascent and lays them out in the cell,
// so two raster fonts whose rounded metrics agree draw identically under MSDF. Wherever
// this platform has such a pair, the MSDF and AUTO renderers must still step past it.
bool test_zoom_crosses_raster_only_changes_under_msdf()
{
    bool ok = true;
    for (const QString& family : zoom_test_families()) {
        for (const vnm_terminal::Font_advance_policy advance_policy : k_zoom_advance_policies) {
            for (const term::Terminal_text_renderer_policy renderer_policy : {
                     term::Terminal_text_renderer_policy::MSDF,
                     term::Terminal_text_renderer_policy::AUTO})
            {
                for (const zoom_display_t& display : k_zoom_displays) {
                    const Zoom_text_setup setup{family, advance_policy, renderer_policy, display};
                    ok &= check_zoom_across_pairs(
                        setup,
                        [](const zoom_drawing_t& smaller,
                           const zoom_drawing_t& larger,
                           qreal,
                           qreal)
                        {
                            const term::qsg_atlas_text_draw_sizes_t& a = smaller.sizes;
                            const term::qsg_atlas_text_draw_sizes_t& b = larger.sizes;
                            return
                                a.raster_pixel_size      != b.raster_pixel_size      &&
                                a.msdf_draw_pixel_height == b.msdf_draw_pixel_height &&
                                a.cell_metrics.width     == b.cell_metrics.width     &&
                                a.cell_metrics.height    == b.cell_metrics.height    &&
                                a.cell_metrics.ascent    == b.cell_metrics.ascent    &&
                                a.cell_metrics.descent   == b.cell_metrics.descent;
                        },
                        "raster-only");
                }
            }
        }
    }
    return ok;
}

// A fractional size keeps its fraction while stepping and loses it where the end of the
// zoom range clamps it. Every notch still changes the drawing, and zooming back out
// returns to the drawing it left, though not to the fractional size.
bool test_fractional_zoom_returns_to_the_drawing()
{
    const Zoom_text_setup setup{
        vnm_terminal::default_monospace_font_family(),
        vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP,
        term::Terminal_text_renderer_policy::GLYPH,
        k_zoom_displays[0]};
    const qreal start     = term::k_font_zoom_max_pixel_size - 1.8;
    const qreal zoomed_in = zoom(setup, start,      1);
    const qreal clamped   = zoom(setup, zoomed_in,  1);
    const qreal back_out  = zoom(setup, clamped,   -1);

    bool ok = true;
    ok &= check(
        draws_differently(setup, drawing_of(setup, start), drawing_of(setup, zoomed_in)),
        "a notch from a fractional size changes the drawing");
    ok &= check(
        clamped == term::k_font_zoom_max_pixel_size &&
            draws_differently(setup, drawing_of(setup, zoomed_in), drawing_of(setup, clamped)),
        "the end of the zoom range clamps a fractional size and still changes the drawing");
    ok &= check(
        !draws_differently(setup, drawing_of(setup, back_out), drawing_of(setup, zoomed_in)),
        "zooming back out returns to the drawing a fractional size left");
    return ok;
}

}

int main(int argc, char** argv)
{
    // Font metrics need a GUI application; the zoom is measured with real fonts.
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_angle_delta_steps();
    ok &= test_sub_notch_deltas_accumulate();
    ok &= test_pixel_delta_uses_the_caller_step_size();
    ok &= test_zoom_steps_from_a_renderable_font_size();
    ok &= test_every_zoom_notch_changes_the_drawing();
    ok &= test_zoom_crosses_the_integral_advance_boundary();
    ok &= test_zoom_crosses_raster_only_changes_under_msdf();
    ok &= test_fractional_zoom_returns_to_the_drawing();

    if (!ok) {
        std::cerr << "wheel_gesture_tests: FAILED\n";
        return 1;
    }

    std::cout << "wheel_gesture_tests: OK\n";
    return 0;
}
