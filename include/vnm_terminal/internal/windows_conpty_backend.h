#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <cstddef>
#include <memory>

namespace vnm_terminal::internal {

#if defined(_WIN32)

struct Windows_conpty_backend_write_state_for_testing
{
    std::size_t queued_write_bytes       = 0U;
    std::size_t queued_write_count       = 0U;
    std::size_t in_flight_write_bytes    = 0U;
    std::size_t successful_write_count   = 0U;
    std::size_t failed_write_count       = 0U;
    bool        running                  = false;
    bool        stopping                 = false;
    bool        writer_failed            = false;
    // Whether a queued interrupt has left the write queue. The writer sets an
    // INTERRUPTED exit override the moment it dequeues the Ctrl+C entry and
    // trades that for the delivery flag once the write completes, so either
    // state means a code-130 exit is now classified as an interrupt. A test
    // whose scenario depends on the interrupt staying queued can ask whether
    // that premise held instead of inferring it from bytes in flight, which
    // are counted per write and so cannot distinguish the two writes.
    bool        interrupt_left_write_queue = false;
};

// Which of the two post-creation start failures to force on the next start.
// Windows offers no way to make AssignProcessToJobObject or ResumeThread fail
// on demand, and the created-child custody the backend owes after either one is
// only observable when they do.
enum class Windows_conpty_start_failure_injection
{
    NONE,
    JOB_ASSIGNMENT,
    RESUME_THREAD,
};

// Arms one injected post-creation failure; the next start consumes it.
void windows_conpty_inject_start_failure_after_creation_for_testing(
    Windows_conpty_start_failure_injection injection);

// Withholds the termination request a rejected created child would receive, so
// a test can hold that child alive across the rejection and see whether the
// backend reports a settled exit it never observed. The retained custody still
// acts on destruction, which is what settles the child.
void windows_conpty_suppress_failed_start_termination_for_testing(
    bool                                   suppress);

// The process id of the most recent child this backend created, so a test can
// name the exact process whose custody it is checking.
unsigned long windows_conpty_last_created_process_id_for_testing();

class Windows_conpty_backend final : public Terminal_backend
{
public:
    Windows_conpty_backend();
    ~Windows_conpty_backend() override;

    Windows_conpty_backend(const Windows_conpty_backend&)            = delete;
    Windows_conpty_backend& operator=(const Windows_conpty_backend&) = delete;

    Terminal_backend_result start(
        const Terminal_launch_config&   config,
        Terminal_backend_callbacks      callbacks) override;

    Terminal_backend_result write(
        QByteArray                      bytes) override;

    Terminal_backend_result resize(
        Terminal_backend_resize_request request) override;

    Terminal_backend_result set_output_paused(
        bool                            paused) override;

    Terminal_backend_result interrupt() override;
    Terminal_backend_result terminate() override;

    Windows_conpty_backend_write_state_for_testing write_state_for_testing();

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<Terminal_backend> make_windows_conpty_backend();

#endif

}
