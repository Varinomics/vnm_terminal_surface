#pragma once

#include "vnm_terminal/process_cpu_time.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vnm_terminal {

struct terminal_inherited_descriptor_t
{
    int source = -1;
    int target = -1;
};

struct terminal_process_launch_t
{
    std::string executable;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string working_directory;
    std::vector<terminal_inherited_descriptor_t> inherited;
    int rows         = 0;
    int columns      = 0;
    int pixel_width  = 0;
    int pixel_height = 0;
};

struct terminal_process_launch_result_t
{
    bool accepted                 = false;
    bool native_dispatch_occurred = false;
    bool start_outcome_determinate = true;
    int master_descriptor         = -1;
    std::int64_t root_process_id   = -1;
    std::string error;
};

struct terminal_process_exit_t
{
    int exit_code = -1;
    int signal    = 0;
};

/**
 * Optional POSIX PTY process ownership, selected before a session's native birth.
 * The built-in backend supplies its ordinary direct-child owner when absent.
 * Supplying an owner replaces that lifetime policy; failures never select another.
 *
 * Launch transfers the master descriptor only on success. Every born process
 * remains this owner's cleanup obligation on failure or exception. Prepare any
 * cleanup resources before birth. The backend retains this object through its
 * deferred cleanup, independently of revocable presentation callbacks.
 * Retain endpoints needed to preserve accepted terminal output until finish.
 *
 * One worker calls wait_root for exact exit observation; it need not consume the
 * final wait. stop may run concurrently and must not block for its grace period.
 * A failed stop remains committed and must not be replayed as a new operation.
 * finish runs after the sole waiter, except that the waiter itself may call it
 * after observing exit. It must be idempotent and retire every owned wait.
 * confirmed describes this owner's cleanup obligations, never a guessed exit.
 */
class Terminal_process_owner
{
public:
    virtual ~Terminal_process_owner() = default;

    virtual terminal_process_launch_result_t launch(
        const terminal_process_launch_t& request) = 0;

    virtual bool stop(
        std::chrono::milliseconds graceful_interval,
        std::chrono::milliseconds kill_interval,
        std::string*              error) noexcept = 0;

    virtual std::optional<terminal_process_exit_t> wait_root() = 0;
    virtual void finish() = 0;
    virtual bool confirmed() const noexcept = 0;

    virtual std::optional<process_cpu_time_t> process_cpu_time() const
    {
        return std::nullopt;
    }
};

} // namespace vnm_terminal
