#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

// Reserve a joinable cleanup worker before native birth. The public facade may
// request cleanup while it still owns the Impl (failed start), and later hand
// the Impl over without allocating, starting a thread, joining, or pretending
// that cleanup succeeded. The original native wait/signal owners stay inside
// that Impl. A negative native observation can keep this task pending without
// holding a product call or another backend's cleanup worker.
//
// The module's registry owns every join handle. It reaps finished tasks on new
// admission/observation and joins remaining tasks at module/process teardown.
// Unloading the code which owns a still-running task is deliberately NOT a
// bounded operation. The containing native owner must retain that module; this
// mechanism is not permission to unload it or claim native settlement.
class Native_backend_cleanup_reservation
{
    using Action = void (*)(void*) noexcept;
    using Settlement = bool (*)(void*) noexcept;

    struct Job
    {
        std::mutex mutex;
        std::condition_variable changed;
        void* context = nullptr;
        Action cleanup = nullptr;
        Action dispose = nullptr;
        Settlement settlement = nullptr;
        bool requested = false;
        bool released = false;
        bool cancelled = false;
        std::atomic_bool native_complete{false};
        std::atomic_bool finished{false};
        std::thread worker;
    };

    class Registry
    {
    public:
        ~Registry()
        {
            // Final code-unload barrier, not the public backend destructor.
            // Never detach a native owner or erase an unconfirmed task.
            for (const auto& job : m_jobs) {
                if (job->worker.joinable()) {
                    job->worker.join();
                }
            }
        }

        std::shared_ptr<Job> reserve(void* context, Action cleanup, Action dispose, Settlement settlement)
        {
            if (!context || !cleanup || !dispose) {
                throw std::invalid_argument("A native cleanup reservation requires its exact owner");
            }
            auto job = std::make_shared<Job>();
            job->context = context;
            job->cleanup = cleanup;
            job->dispose = dispose;
            job->settlement = settlement;
            const std::lock_guard lock(m_mutex);
            reap_finished_locked();
            m_jobs.push_back(job); // All admission allocations precede birth.
            try {
                job->worker = std::thread([job] {
                    {
                        std::unique_lock gate(job->mutex);
                        job->changed.wait(gate, [&] { return job->requested || job->cancelled; });
                        if (job->cancelled) {
                            job->finished.store(true, std::memory_order_release);
                            return;
                        }
                    }
                    job->cleanup(job->context);
                    job->native_complete.store(
                        !job->settlement || job->settlement(job->context), std::memory_order_release);
                    {
                        std::unique_lock gate(job->mutex);
                        job->changed.wait(gate, [&] { return job->released; });
                    }
                    job->dispose(job->context);
                    job->context = nullptr;
                    job->finished.store(true, std::memory_order_release);
                });
            }
            catch (...) {
                m_jobs.pop_back();
                throw; // No child has been admitted by this reservation.
            }
            return job;
        }

        std::size_t retained()
        {
            const std::lock_guard lock(m_mutex);
            reap_finished_locked();
            return m_jobs.size();
        }

        std::size_t unconfirmed()
        {
            const std::lock_guard lock(m_mutex);
            reap_finished_locked();
            return std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& job) {
                return job->finished.load(std::memory_order_acquire) &&
                    !job->native_complete.load(std::memory_order_acquire);
            });
        }

    private:
        void reap_finished_locked()
        {
            std::erase_if(m_jobs, [](const auto& job) {
                if (!job->finished.load(std::memory_order_acquire)) {
                    return false;
                }
                if (job->worker.joinable()) {
                    job->worker.join();
                }
                // Owner loss permits local disposal, not a fabricated receipt.
                // Retain the preallocated outcome record without a live Impl.
                return job->cancelled || job->native_complete.load(std::memory_order_acquire);
            });
        }
        std::mutex m_mutex;
        std::vector<std::shared_ptr<Job>> m_jobs;
    };

    static Registry& registry()
    {
        static Registry owner;
        return owner;
    }

public:
    Native_backend_cleanup_reservation(
        void* context, Action cleanup, Action dispose, Settlement settlement = nullptr)
        : m_job(registry().reserve(context, cleanup, dispose, settlement))
    {}

    ~Native_backend_cleanup_reservation()
    {
        // Construction failure before a start can cancel its unused worker.
        // After handoff the registry, not this ticket, owns the running task.
        const std::lock_guard lock(m_job->mutex);
        if (!m_job->requested) {
            m_job->cancelled = true;
            m_job->changed.notify_all();
        }
    }

    Native_backend_cleanup_reservation(const Native_backend_cleanup_reservation&) = delete;
    Native_backend_cleanup_reservation& operator=(const Native_backend_cleanup_reservation&) = delete;

    void request() noexcept
    {
        const std::lock_guard lock(m_job->mutex);
        m_job->requested = true;
        m_job->changed.notify_all();
    }

    void release() noexcept
    {
        const std::lock_guard lock(m_job->mutex);
        m_job->released = true;
        m_job->requested = true;
        m_job->changed.notify_all();
    }

    bool native_complete() const noexcept
    {
        return m_job->native_complete.load(std::memory_order_acquire);
    }

    static std::size_t retained_tasks()
    {
        return registry().retained();
    }

    static std::size_t unconfirmed_tasks()
    {
        return registry().unconfirmed();
    }

    static bool wait_until_idle(std::chrono::steady_clock::time_point deadline)
    {
        while (retained_tasks() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

private:
    std::shared_ptr<Job> m_job;
};

} // namespace vnm_terminal::internal
