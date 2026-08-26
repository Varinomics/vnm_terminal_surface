#include "vnm_terminal/terminal_canvas_export.h"

#include <type_traits>

static_assert(std::is_same_v<
    decltype(vnm_terminal::Terminal_canvas_export_result::frame),
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>>);

bool terminal_canvas_public_export_header_contract()
{
    return true;
}
