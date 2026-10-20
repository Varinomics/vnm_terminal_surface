#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace vnm_terminal::internal {

// Availability only consumes the latest font selection. Keep one running check
// and one replaceable pending check so obsolete selections cannot queue bakes.
class Msdf_availability_work
{
public:
    bool submit(std::function<void()> check)
    {
        std::lock_guard lock(m_mutex);
        m_pending = std::move(check);
        return !std::exchange(m_worker_active, true);
    }

    // Called by the worker admitted by submit(). Weak ownership lets surface
    // destruction discard pending checks without waiting for an active bake.
    static void run(const std::weak_ptr<Msdf_availability_work>& weak_work)
    {
        for (;;) {
            auto work = weak_work.lock();
            if (work == nullptr) {
                return;
            }

            auto check = work->take_next();
            work.reset();
            if (!check) {
                return;
            }
            check();
        }
    }

private:
    std::function<void()> take_next()
    {
        std::lock_guard lock(m_mutex);
        if (!m_pending) {
            m_worker_active = false;
        }
        return std::exchange(m_pending, {});
    }

    std::mutex            m_mutex;
    std::function<void()> m_pending;
    bool                  m_worker_active = false;
};

}
