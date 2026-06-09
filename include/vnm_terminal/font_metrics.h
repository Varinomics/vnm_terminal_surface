#pragma once

#include <QString>
#include <QtGlobal>

namespace vnm_terminal {

// Default monospace font pixel size the surface uses when a consumer does not
// override it. Mirrors the surface's internal default so consumers can seed
// their own font-size option from the same value the surface computes against.
inline constexpr int k_default_font_pixel_size = 13;

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
    qreal          device_pixel_ratio);

// Report whether `metrics` describe a usable cell: finite, strictly positive
// width and height. A consumer should treat invalid metrics as a signal not to
// resize against them.
bool cell_metrics_valid(const Cell_metrics& metrics);

}
