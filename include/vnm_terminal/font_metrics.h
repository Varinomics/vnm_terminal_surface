#pragma once

#include <QString>
#include <QtGlobal>

namespace vnm_terminal {

// The requested font size remains the user's value. This policy controls how
// the monospace advance used by the terminal grid is produced from it.
enum class Font_advance_policy
{
    ADJUST_FONT_SIZE,
    SNAP_ADVANCE_UP,
    SNAP_ADVANCE_NEAREST,
};

constexpr bool font_advance_policy_is_valid(Font_advance_policy policy)
{
    switch (policy) {
        case Font_advance_policy::ADJUST_FONT_SIZE:
        case Font_advance_policy::SNAP_ADVANCE_UP:
        case Font_advance_policy::SNAP_ADVANCE_NEAREST:
            return true;
    }

    return false;
}

// Default monospace font pixel size the surface uses when a consumer does not
// override it. Mirrors the surface's internal default so consumers can seed
// their own font-size option from the same value the surface computes against.
inline constexpr int k_default_font_pixel_size = 13;
inline constexpr qreal k_default_logical_dpi = 96.0;

// Family name of the surface's bundled default monospace font. Resolves the
// embedded framework font when it loads and otherwise falls back to the
// platform "monospace" family.
QString default_monospace_font_family();

// Cell geometry the surface derives for a given font and device pixel ratio.
// `width` and `height` are the per-cell advance and line height in logical
// pixels, already snapped to the device pixel grid.
struct Cell_metrics {
    qreal width  = 0.0;
    qreal height = 0.0;
};

// Compute the per-cell geometry the surface would use to lay out a grid in the
// named font at `pixel_size`, snapped for `device_pixel_ratio`. This matches
// the geometry the surface uses internally, so a consumer can size windows in
// whole-cell multiples without reaching into surface internals.
Cell_metrics cell_metrics_for_font(
    const QString& family,
    qreal          pixel_size,
    qreal          device_pixel_ratio,
    Font_advance_policy policy = Font_advance_policy::ADJUST_FONT_SIZE,
    qreal               logical_dpi = k_default_logical_dpi);

// Return the actual logical pixel size used to render the font under
// `policy`. For SNAP_ADVANCE_* this is the requested size; for
// ADJUST_FONT_SIZE it is the smallest renderable size whose measured advance
// reaches the next device pixel, without shrinking below the requested size.
qreal effective_font_size_for_font(
    const QString& family,
    qreal          pixel_size,
    qreal          device_pixel_ratio,
    Font_advance_policy policy = Font_advance_policy::ADJUST_FONT_SIZE,
    qreal               logical_dpi = k_default_logical_dpi);

// Report whether `metrics` describe a usable cell: finite, strictly positive
// width and height. A consumer should treat invalid metrics as a signal not to
// resize against them.
bool cell_metrics_valid(const Cell_metrics& metrics);

}
