#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <memory>

int main()
{
    bool (VNM_TerminalCanvas::*set_canvas_frame)(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>) =
            &VNM_TerminalCanvas::set_canvas_frame;
    bool (VNM_TerminalCanvas::*content_extent_available)() const =
        &VNM_TerminalCanvas::content_extent_available;
    const vnm_terminal::terminal_canvas_content_extent_t content_extent;
    return
        set_canvas_frame != nullptr &&
            content_extent_available != nullptr &&
            content_extent.record_version ==
                vnm_terminal::k_terminal_canvas_content_extent_version
        ? 0
        : 1;
}
