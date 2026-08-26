#pragma once

#include "vnm_terminal/terminal_canvas_frame.h"

#include <memory>

class VNM_TerminalSurface;

namespace vnm_terminal {

enum class Terminal_canvas_export_status
{
    OK,
    NO_FRAME,
    WRONG_THREAD,
    INVALID_SOURCE_FRAME,
    ROW_LIMIT_EXCEEDED,
    COLUMN_LIMIT_EXCEEDED,
    CELL_LIMIT_EXCEEDED,
    STYLE_LIMIT_EXCEEDED,
    CELL_TEXT_UTF16_CODE_UNIT_LIMIT_EXCEEDED,
    FRAME_TEXT_UTF8_BYTE_LIMIT_EXCEEDED,
};

struct Terminal_canvas_export_result
{
    Terminal_canvas_export_status               status = Terminal_canvas_export_status::NO_FRAME;
    std::shared_ptr<const Terminal_canvas_frame> frame;
};

// Call on the surface's owning thread. The returned frame owns all of its data;
// later surface publications cannot mutate a previously returned frame. Text
// that exceeds either published limit fails explicitly and is never truncated.
Terminal_canvas_export_result export_terminal_canvas_frame(
    const VNM_TerminalSurface& surface);

} // namespace vnm_terminal
