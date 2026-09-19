#pragma once

#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "../../src/native_backend_io_core.h"
#include "vnm_terminal/internal/backend_contract.h"

#include <QByteArray>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace vnm_terminal::test_helpers {

struct Callback_lifetime_audit
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    admitted           = false;
    bool                    release            = false;
    bool                    returned           = false;
    bool                    destruction_started = false;
    bool                    destruction_returned = false;
    bool                    invalid_receiver   = false;
    bool                    snapshot_barrier   = false;
    std::atomic_bool        receiver_alive{true};
};

// Use the shared_ptr atomic functions for libc++ implementations that lack
// the C++20 atomic<shared_ptr> specialization. Every probe access stays atomic.
inline std::shared_ptr<Callback_lifetime_audit> callback_lifetime_probe;

inline void stop_after_callback_snapshot(internal::Native_backend_callback_kind_for_testing kind)
{
    if (kind != internal::Native_backend_callback_kind_for_testing::OUTPUT) {
        return;
    }
    auto audit = std::atomic_load(&callback_lifetime_probe);
    if (!audit) {
        return;
    }
    std::unique_lock lock(audit->mutex);
    if (audit->admitted) {
        return;
    }
    audit->admitted = true;
    audit->changed.notify_all();
    audit->changed.wait(lock, [&] { return audit->release; });
}

class Callback_receiver_lifetime
{
public:
    explicit Callback_receiver_lifetime(std::shared_ptr<Callback_lifetime_audit> audit)
    :
        m_audit(std::move(audit))
    {}

    ~Callback_receiver_lifetime() { m_audit->receiver_alive.store(false); }

private:
    std::shared_ptr<Callback_lifetime_audit> m_audit;
};

// The independent lifetime token replaces the unsafe raw-this dereference. The
// receiver really dies after facade destruction; observing its token is safe.
inline bool check_callback_lifetime(
    std::unique_ptr<internal::Terminal_backend> backend,
    const internal::Terminal_launch_config& config,
    bool snapshot_barrier)
{
    constexpr auto timeout = std::chrono::seconds(20);
    auto audit = std::make_shared<Callback_lifetime_audit>();
    audit->snapshot_barrier = snapshot_barrier;
    if (snapshot_barrier) {
        std::atomic_store(&callback_lifetime_probe, audit);
        internal::set_native_backend_callback_snapshot_hook_for_testing(stop_after_callback_snapshot);
    }
    auto receiver = std::make_unique<Callback_receiver_lifetime>(audit);
    internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [audit](QByteArray) {
        std::unique_lock lock(audit->mutex);
        if (audit->admitted && !audit->snapshot_barrier) {
            audit->invalid_receiver |= !audit->receiver_alive.load();
            return;
        }
        if (!audit->snapshot_barrier) {
            audit->admitted = true;
            audit->changed.notify_all();
            audit->changed.wait(lock, [&] { return audit->release; });
        }
        audit->invalid_receiver = !audit->receiver_alive.load();
        audit->returned = true;
        audit->changed.notify_all();
    };
    callbacks.process_exited = [](internal::Terminal_backend_exit) {};
    callbacks.error_reported = [](internal::Terminal_backend_error) {};

    const auto started = backend->start(config, std::move(callbacks));
    bool ok = check(started.code == internal::Terminal_backend_result_code::ACCEPTED,
        "callback lifetime fixture starts");
    bool admitted = false;
    {
        std::unique_lock lock(audit->mutex);
        admitted = audit->changed.wait_for(lock, timeout, [&] { return audit->admitted; });
        ok &= check(admitted, "callback reaches the controlled receiver barrier");
    }

    std::thread destroyer([
            audit,
            backend = std::move(backend),
            receiver = std::move(receiver)
        ]() mutable
        {
            {
                const std::lock_guard lock(audit->mutex);
                audit->destruction_started = true;
                audit->changed.notify_all();
            }
            backend.reset();
            receiver.reset();
            {
                const std::lock_guard lock(audit->mutex);
                audit->destruction_returned = true;
                audit->changed.notify_all();
            }
        });

    {
        std::unique_lock lock(audit->mutex);
        ok &= check(audit->changed.wait_for(lock, timeout, [&] {
            return audit->destruction_started;
        }), "facade destruction thread enters");
        // A draining implementation may wait here; the timeout releases the
        // artificial callback barrier so that its destructor can finish.
        const bool returned_before_release = audit->changed.wait_for(lock, timeout, [&] {
            return audit->destruction_returned;
        });
        std::cerr << "CALLBACK_LIFETIME snapshot_barrier=" << snapshot_barrier
                  << " barrier_reached=" << admitted
                  << " facade_returned_before_release=" << returned_before_release
                  << " receiver_alive=" << audit->receiver_alive.load() << '\n';
        audit->release = true;
        audit->changed.notify_all();
        if (admitted && !snapshot_barrier) {
            ok &= check(audit->changed.wait_for(lock, timeout, [&] { return audit->returned; }),
                "admitted callback leaves the controlled barrier");
            std::cerr << "CALLBACK_LIFETIME invalid_receiver=" << audit->invalid_receiver << '\n';
            ok &= check(!audit->invalid_receiver,
                "facade destruction must preserve an already admitted callback receiver");
        }
    }
    destroyer.join();
    const bool idle = internal::Native_backend_cleanup_reservation::wait_until_idle(
        std::chrono::steady_clock::now() + timeout);
    ok &= check(idle, "fixture cleanup registry drains after callback release");
    {
        const std::lock_guard lock(audit->mutex);
        std::cerr << "CALLBACK_LIFETIME snapshot_barrier=" << snapshot_barrier
                  << " callback_invoked=" << audit->returned
                  << " invalid_receiver=" << audit->invalid_receiver
                  << " cleanup_idle=" << idle << '\n';
        ok &= check(!audit->invalid_receiver,
            "copied callback must not invoke a receiver after facade destruction");
    }
    if (snapshot_barrier) {
        internal::set_native_backend_callback_snapshot_hook_for_testing(nullptr);
        std::atomic_store(&callback_lifetime_probe, std::shared_ptr<Callback_lifetime_audit>{});
    }
    return ok;
}

} // namespace vnm_terminal::test_helpers
