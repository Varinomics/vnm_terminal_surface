#pragma once

#include <cstdint>
#include <limits>

namespace vnm_terminal::internal {

using Terminal_hyperlink_id = std::uint32_t;

constexpr Terminal_hyperlink_id k_no_terminal_hyperlink_id = 0U;
constexpr Terminal_hyperlink_id k_max_terminal_hyperlink_id =
    std::numeric_limits<Terminal_hyperlink_id>::max();

}
