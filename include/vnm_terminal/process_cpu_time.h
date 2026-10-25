#pragma once

#include <cstdint>

namespace vnm_terminal {

enum class Process_cpu_scope
{
    // Best-effort observations of the command and discoverable descendants.
    // Short-lived or detached children can be missed, as can work before a
    // process is first observed and after its last successful observation.
    OBSERVED_FAMILY,
    // Native workload accounting, including CPU from exited descendants.
    WORKLOAD_FAMILY,
};

// Cumulative user and kernel CPU time within scope. Previously credited CPU
// remains in the counter after a process exits. Terminal and custody helper
// processes are outside either accounting scope.
struct process_cpu_time_t
{
    std::uint64_t cpu_time_usec           = 0;
    std::uint64_t sampled_at_usec         = 0;
    std::uint32_t logical_processor_count = 0;
    Process_cpu_scope scope              = Process_cpu_scope::OBSERVED_FAMILY;
};

// sampled_at_usec uses std::chrono::steady_clock's epoch. The processor count
// is the whole machine's online logical CPU count, not the command's affinity
// or quota. Counters from different terminal launches are not comparable.

} // namespace vnm_terminal
