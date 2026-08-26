#include "vnm_terminal/terminal_canvas_frame.h"

bool terminal_canvas_public_header_contract()
{
    return
        vnm_terminal::k_terminal_canvas_frame_api_version == 1U &&
        vnm_terminal::k_terminal_canvas_max_rows          == 200 &&
        vnm_terminal::k_terminal_canvas_max_columns       == 300 &&
        vnm_terminal::k_terminal_canvas_max_cells         == 32'768U &&
        vnm_terminal::k_terminal_canvas_max_styles        == 256U &&
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units == 4'096 &&
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes == 96 * 1'024;
}
