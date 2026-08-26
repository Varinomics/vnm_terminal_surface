#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <memory>

int main()
{
    bool (VNM_TerminalCanvas::*set_canvas_frame)(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>) =
            &VNM_TerminalCanvas::set_canvas_frame;
    return set_canvas_frame != nullptr ? 0 : 1;
}
