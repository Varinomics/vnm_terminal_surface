#pragma once

namespace vnm_terminal {

// The terminal renderer's glyph atlas samples a texture array and draws
// instanced quads. An OpenGL QRhi offers both only on desktop OpenGL 3.3 or
// OpenGL ES 3.0, and it decides from the version the context reports. Qt's
// offscreen GLX platform reports the requested version rather than the one the
// driver provides, so a default request leaves the atlas without either.
//
// Raises QSurfaceFormat::defaultFormat() to that minimum, keeping a higher
// requested version and every other format option. Call it in each process
// that renders a terminal, after its QGuiApplication exists and before it
// creates its first window or QRhi.
void apply_terminal_renderer_minimum_surface_format();

} // namespace vnm_terminal
