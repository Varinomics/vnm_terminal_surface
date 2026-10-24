#pragma once

#include <cstdint>

namespace vnm_terminal {

// Cumulative user and kernel CPU time for one terminal command and its
// descendants, including descendants that have already exited. Terminal and
// custody helper processes are outside this accounting scope.
struct process_cpu_time_t
{
    std::uint64_t cpu_time_usec           = 0;
    std::uint64_t sampled_at_usec         = 0;
    std::uint32_t logical_processor_count = 0;
};

// sampled_at_usec uses std::chrono::steady_clock's epoch. The processor count
// is the whole machine's online logical CPU count, not the command's affinity
// or quota. Counters from different terminal launches are not comparable.

} // namespace vnm_terminal
