#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>

namespace vnm_terminal::internal {

#if defined(_WIN32)

enum class Windows_conpty_start_fault_for_testing
{
    NONE,
    JOB_ASSIGNMENT,
    THREAD_RESUME_FAILURE,
    RESIZE_WORKER_START,
};

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
    bool        process_handle_retained = false;
    bool        process_assigned_to_job = false;
    bool        native_cleanup_settled = false;
    std::size_t adoption_job_count = 0U;
};

// Installed before start; lets native integration tests retain each cleanup
// phase after the facade has gone without accessing a deleted backend.
struct Windows_conpty_close_control_for_testing
{
    std::binary_semaphore close_entered{0};
    std::binary_semaphore allow_close{0};
    std::binary_semaphore observer_entered{0};
    std::binary_semaphore allow_observer{0};
    std::atomic_uint      close_count{0};
    std::atomic_uint      observer_retirement_count{0};
};

// Holds an admitted native resize before it enters ConPTY. Releasing the gate
// lets the same call reach the packaged ResizePseudoConsole implementation.
struct Windows_conpty_resize_control_for_testing
{
    std::binary_semaphore resize_entered{0};
    std::binary_semaphore allow_resize{0};
};

struct windows_conpty_cpu_query_for_testing_t
{
    std::int64_t  before_qpc       = 0;
    std::int64_t  after_qpc        = 0;
    std::uint64_t kernel_ticks     = 0;
    std::uint64_t user_ticks       = 0;
    std::uint32_t active_processes = 0;
    std::uint32_t total_processes  = 0;
    std::uint32_t error            = 0;
};

enum class Windows_conpty_descendant_scan_fault_for_testing
{
    NONE,
    MEMBER_QUERY,
    ACTIVE_FAMILY_ALLOCATION,
};

// Installed before start. Faults occur at the scanner's native failure
// boundaries without invalidating the Job used by public CPU accounting.
struct Windows_conpty_descendant_scan_control_for_testing
{
    std::atomic<Windows_conpty_descendant_scan_fault_for_testing> fault{
        Windows_conpty_descendant_scan_fault_for_testing::NONE};
    std::atomic_uint injected_failures{0};
    std::atomic_uint successful_member_queries{0};
};

// Installed before start. Handles are borrowed only for the callback, which
// runs under native ownership and must not re-enter the backend or retain them.
using Windows_conpty_cpu_observer_for_testing = std::function<void(
    const char* event, void* job, void* root, const windows_conpty_cpu_query_for_testing_t& query)>;

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

    Terminal_backend_resize_dispatch dispatch_resize(
        Terminal_backend_resize_request request) override;

    Terminal_backend_result set_output_paused(
        bool                            paused) override;

    Terminal_backend_result interrupt() override;
    Terminal_backend_result terminate() override;

    std::optional<process_cpu_time_t> process_cpu_time() const override;
    std::optional<Process_snapshot> process_snapshot() const override;
    std::optional<terminal_cell_pixel_size_t> fixed_cell_pixel_size() const override;
    bool owns_screen_buffers() const override;

    Windows_conpty_backend_write_state_for_testing write_state_for_testing();
    bool set_start_fault_for_testing(Windows_conpty_start_fault_for_testing fault);
    void set_cleanup_observation_blocked_for_testing(bool blocked);
    // The test can release this observation gate after destroying the facade;
    // it never calls a member through a deleted backend.
    bool set_cleanup_observation_gate_for_testing(std::shared_ptr<std::atomic_bool> gate);
    bool set_close_control_for_testing(std::shared_ptr<Windows_conpty_close_control_for_testing> control);
    bool set_resize_control_for_testing(std::shared_ptr<Windows_conpty_resize_control_for_testing> control);
    bool set_cpu_observer_for_testing(Windows_conpty_cpu_observer_for_testing observer);
    bool set_descendant_scan_control_for_testing(
        std::shared_ptr<Windows_conpty_descendant_scan_control_for_testing> control);

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<Terminal_backend> make_windows_conpty_backend();

#endif

}
