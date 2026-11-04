#pragma once

#include <poll.h>

namespace vnm_terminal::internal {

enum class Posix_pty_io_point_for_testing
{
    READ,
    WRITE,
    SHUTDOWN_JOIN,
    ROOT_EXIT,
    FINISH_BEGIN,
    FINISH_END,
    MASTER_CLOSED,
    DRAIN_PEER_OPEN,
    DRAIN_PEER_READY,
    OUTPUT_BOUNDARY,
};

using Posix_pty_io_hook_for_testing = void (*)(Posix_pty_io_point_for_testing, int);

// Defined by test-enabled builds only. The I/O hook runs after the worker has
// acquired its descriptor, outside the backend mutex, to expose its lifetime.
void set_posix_pty_io_hook_for_testing(Posix_pty_io_hook_for_testing hook);

enum class Posix_pty_io_worker_for_testing
{
    READER,
    WRITER,
};

enum class Posix_pty_poll_phase_for_testing
{
    BEFORE_WAIT,
    AFTER_WAIT,
};

struct posix_pty_poll_observation_t
{
    Posix_pty_io_worker_for_testing  worker;
    Posix_pty_poll_phase_for_testing phase;
    const pollfd*                   descriptors;
    nfds_t                          descriptor_count;
    int                             timeout_ms;
    int                             result;
    int                             error_code;
};

using Posix_pty_poll_hook_for_testing = void (*)(const posix_pty_poll_observation_t&);

// Observes the actual native wait without replacing readiness. Descriptors are
// borrowed for the callback; result/error_code are meaningful after the wait.
// The observer runs outside the backend mutex and preserves the native errno.
// Keep fixture state alive until the observed workers have joined.
void set_posix_pty_poll_hook_for_testing(Posix_pty_poll_hook_for_testing hook);

using Posix_pty_wake_write_error_hook_for_testing = int (*)(
    Posix_pty_io_worker_for_testing, int);

// Zero delegates to the native write; a nonzero errno injects a failed attempt.
// The descriptor is the real nonblocking write endpoint, allowing full-pipe
// tests without fabricated readiness. The hook must not re-enter the backend.
void set_posix_pty_wake_write_error_hook_for_testing(
    Posix_pty_wake_write_error_hook_for_testing hook);

} // namespace vnm_terminal::internal
