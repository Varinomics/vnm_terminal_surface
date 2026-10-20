#pragma once

#include "vnm_process_custody/process_spawn.h"

#include <string>
#include <vector>

namespace vnm::process_custody::detail {

// argv borrows resolved_arguments; envp borrows the request's environment.
// Populate a fresh value in place and keep both owners stationary until spawn returns.
struct Prepared_spawn
{
    std::vector<std::string>   resolved_arguments;
    std::vector<char*>         argv;
    std::vector<char*>         envp;
    int                       sources[64];
    int                       targets[64];
    std::vector<Native_handle> child_handles;
    int                       highest_target = 2; // stdio remains part of the tidied range
};

bool prepare_spawn(const Spawn_request& request, Prepared_spawn& prepared, std::string& error);

} // namespace vnm::process_custody::detail
