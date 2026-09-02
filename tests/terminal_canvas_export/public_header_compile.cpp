#ifdef _WIN32
#include <windows.h>
#endif

#include "vnm_terminal/terminal_canvas_frame.h"

#include <optional>
#include <type_traits>

bool terminal_canvas_public_header_contract()
{
    static_assert(std::is_same_v<
        decltype(vnm_terminal::Terminal_canvas_frame{}.content_extent),
        std::optional<vnm_terminal::terminal_canvas_content_extent_t>>);

    const vnm_terminal::terminal_canvas_content_extent_t extent;
    return
        vnm_terminal::k_terminal_canvas_frame_api_version == 4U &&
        vnm_terminal::k_terminal_canvas_content_extent_version == 1U &&
        extent.record_version ==
            vnm_terminal::k_terminal_canvas_content_extent_version &&
        extent.active_buffer ==
            vnm_terminal::Terminal_canvas_buffer::PRIMARY_BUFFER &&
        extent.content_bottom_row_exclusive == 1 &&
        extent.scrollback_rows == 0 &&
        extent.offset_from_tail == 0 &&
        vnm_terminal::k_terminal_canvas_max_rows == 32'768 &&
        vnm_terminal::k_terminal_canvas_max_columns == 32'768 &&
        vnm_terminal::k_terminal_canvas_max_cells         == 32'768U &&
        vnm_terminal::k_terminal_canvas_max_styles        == 256U &&
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units == 4'096 &&
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes == 96 * 1'024;
}
