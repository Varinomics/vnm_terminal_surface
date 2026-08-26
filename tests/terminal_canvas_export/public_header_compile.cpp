#include "vnm_terminal/terminal_canvas_frame.h"

#include <type_traits>

static_assert(std::is_same_v<
    decltype(vnm_terminal::Terminal_canvas_export_result::frame),
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>>);

bool terminal_canvas_public_header_contract()
{
    return
        vnm_terminal::k_terminal_canvas_frame_api_version == 1U &&
        vnm_terminal::k_terminal_canvas_max_rows          == 200 &&
        vnm_terminal::k_terminal_canvas_max_columns       == 300 &&
        vnm_terminal::k_terminal_canvas_max_cells         == 32'768U &&
        vnm_terminal::k_terminal_canvas_max_styles        == 256U;
}
