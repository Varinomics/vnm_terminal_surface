#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vnm_terminal {

struct Process_snapshot_entry
{
    std::int64_t                process_id = 0;
    std::uint64_t               birth_token = 0;
    std::string                 name;
    std::optional<std::uint64_t> cpu_time_usec;
    std::optional<std::uint64_t> resident_memory_bytes;
};

// Observations confer no process-control authority. A zero birth token cannot
// identify the same process occurrence across samples for a CPU-rate interval.
struct Process_snapshot
{
    std::uint64_t                      sampled_at_usec = 0;
    std::uint32_t                      logical_processor_count = 0;
    std::vector<Process_snapshot_entry> processes;
};

} // namespace vnm_terminal
