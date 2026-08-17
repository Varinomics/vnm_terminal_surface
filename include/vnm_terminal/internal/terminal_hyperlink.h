#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace vnm_terminal::internal {

using Terminal_hyperlink_id = std::uint32_t;

constexpr Terminal_hyperlink_id k_no_terminal_hyperlink_id = 0U;
constexpr Terminal_hyperlink_id k_max_terminal_hyperlink_id =
    std::numeric_limits<Terminal_hyperlink_id>::max();

// How many identities the active hyperlink registry may gain before it is
// pruned back to the ones the screen still references. The id space is 32 bits
// wide, so waiting for it to run out is not a memory bound; this is.
constexpr std::size_t k_terminal_hyperlink_prune_threshold = 4096U;

}
