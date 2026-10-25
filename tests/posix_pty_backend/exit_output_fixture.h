#pragma once

#include "native_backend_factory.h"

#include <functional>
#include <sys/types.h>

namespace vnm_terminal::test_helpers {

struct Exit_output_hooks
{
    Native_backend_factory make_backend;
    std::function<int(pid_t)> observe_detached;
    std::function<bool()> verify_cleanup;
};

int run_exit_output_tests(int argc, char** argv, const Exit_output_hooks& hooks);

} // namespace vnm_terminal::test_helpers
