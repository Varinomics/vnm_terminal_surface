#pragma once

#include "vnm_terminal/terminal_canvas_frame.h"

#include <QStringList>
#include <QStringView>

namespace vnm_terminal {

QStringList terminal_canvas_color_scheme_names();
QString terminal_canvas_default_color_scheme_name();
bool terminal_canvas_color_scheme_available(const Terminal_canvas_frame& frame);

// Resolves source color references through a local scheme without changing
// explicit RGB colors or applying presentation attributes a second time.
// Failure leaves the frame unchanged.
bool apply_terminal_canvas_color_scheme(
    Terminal_canvas_frame& frame,
    QStringView scheme_name);

} // namespace vnm_terminal
