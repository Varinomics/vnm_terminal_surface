#pragma once

namespace vnm_terminal::internal {

enum class Posix_pty_io_point_for_testing
{
    READ,
    WRITE,
    SHUTDOWN_JOIN,
};

using Posix_pty_io_hook_for_testing = void (*)(Posix_pty_io_point_for_testing, int);

// Defined by test-enabled builds only. The I/O hook runs after the worker has
// acquired its descriptor, outside the backend mutex, to expose its lifetime.
void set_posix_pty_io_hook_for_testing(Posix_pty_io_hook_for_testing hook);

} // namespace vnm_terminal::internal
