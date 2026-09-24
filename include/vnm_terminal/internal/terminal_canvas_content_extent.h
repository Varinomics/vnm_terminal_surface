#pragma once

#include <algorithm>

namespace vnm_terminal::internal {

inline int terminal_canvas_content_bottom_row_exclusive(
    int rows, int occupied_bottom, bool cursor_visible, int cursor_row)
{
    const int cursor_bottom = cursor_visible && cursor_row >= 0 && cursor_row < rows
        ? cursor_row + 1
        : 0;
    return std::clamp(
        std::max({1, occupied_bottom, cursor_bottom}),
        1,
        rows);
}

}
