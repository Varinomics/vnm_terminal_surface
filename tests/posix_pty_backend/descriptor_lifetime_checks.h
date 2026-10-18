#pragma once

#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "../../src/posix_pty_io_test_hook.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QTemporaryFile>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace vnm_terminal::test_helpers {

struct Pty_descriptor_lifetime_probe
{
    std::mutex              mutex;
    std::condition_variable changed;
    internal::Posix_pty_io_point_for_testing point = internal::Posix_pty_io_point_for_testing::WRITE;
    struct stat             identity{};
    int                     descriptor = -1;
    bool                    identity_valid = false;
    bool                    worker_paused = false;
    bool                    release_worker = false;
    bool                    join_reached = false;
    bool                    barrier_timed_out = false;
};

inline std::mutex pty_descriptor_probe_mutex;
inline std::shared_ptr<Pty_descriptor_lifetime_probe> pty_descriptor_probe;

inline void hold_pty_descriptor(internal::Posix_pty_io_point_for_testing point, int descriptor)
{
    std::shared_ptr<Pty_descriptor_lifetime_probe> probe;
    {
        const std::lock_guard lock(pty_descriptor_probe_mutex);
        probe = pty_descriptor_probe;
    }
    if (!probe) {
        return;
    }

    std::unique_lock lock(probe->mutex);
    if (point == internal::Posix_pty_io_point_for_testing::SHUTDOWN_JOIN) {
        probe->join_reached = true;
        probe->changed.notify_all();
        return;
    }
    if (point != probe->point || probe->worker_paused) {
        return;
    }
    probe->descriptor     = descriptor;
    probe->identity_valid = ::fstat(descriptor, &probe->identity) == 0;
    probe->worker_paused  = true;
    probe->changed.notify_all();
    probe->barrier_timed_out = !probe->changed.wait_for(
        lock, std::chrono::seconds(20), [&] { return probe->release_worker; });
}

inline bool check_pty_descriptor_lifetime(
    const internal::Terminal_launch_config& config,
    internal::Posix_pty_io_point_for_testing point)
{
    using Cleanup = internal::Native_backend_cleanup_reservation;
    constexpr auto timeout = std::chrono::seconds(10);
    if (!check(Cleanup::wait_until_idle(std::chrono::steady_clock::now() + timeout),
            "descriptor lifetime fixture begins after prior cleanup settles"))
    {
        return false;
    }

    auto probe = std::make_shared<Pty_descriptor_lifetime_probe>();
    probe->point = point;
    {
        const std::lock_guard lock(pty_descriptor_probe_mutex);
        pty_descriptor_probe = probe;
    }
    internal::set_posix_pty_io_hook_for_testing(hold_pty_descriptor);
    auto backend = internal::make_posix_pty_backend();
    internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [](QByteArray) {};
    callbacks.process_exited = [](internal::Terminal_backend_exit) {};
    callbacks.error_reported = [](internal::Terminal_backend_error) {};
    const auto started = backend->start(config, callbacks);
    bool ok = check(started.code == internal::Terminal_backend_result_code::ACCEPTED,
        "descriptor lifetime fixture starts");
    if (ok && point == internal::Posix_pty_io_point_for_testing::WRITE) {
        ok &= check(backend->write(QByteArrayLiteral("old-session-queued-input\n")).code ==
                internal::Terminal_backend_result_code::ACCEPTED,
            "descriptor lifetime fixture queues input");
    }

    bool worker_paused = false;
    if (ok) {
        std::unique_lock lock(probe->mutex);
        worker_paused = probe->changed.wait_for(lock, timeout, [&] { return probe->worker_paused; });
        ok &= check(worker_paused, "I/O worker reaches the descriptor-to-syscall barrier");
    }
    backend.reset();

    bool join_reached = false;
    if (worker_paused) {
        std::unique_lock lock(probe->mutex);
        join_reached = probe->changed.wait_for(lock, timeout, [&] { return probe->join_reached; });
        ok &= check(join_reached, "shutdown reaches worker joining while I/O is paused");
    }

    std::vector<std::unique_ptr<QTemporaryFile>> lower_descriptors;
    auto unrelated = std::make_unique<QTemporaryFile>();
    const QByteArray original_content = QByteArrayLiteral("unrelated-file-content\n");
    if (join_reached) {
        struct stat identity{};
        const bool retained = ::fstat(probe->descriptor, &identity) == 0;
        ok &= check(probe->identity_valid && retained &&
                identity.st_dev  == probe->identity.st_dev &&
                identity.st_ino  == probe->identity.st_ino &&
                identity.st_rdev == probe->identity.st_rdev,
            "shutdown retains the original PTY description until its I/O worker exits");
        ok &= check(unrelated->open(), "unrelated file opens during paused cleanup");
        // Native cleanup can retire lower descriptor numbers first. Keep those
        // ordinary opens alive so the next file exercises the captured number
        // if the backend released it before its I/O worker finished.
        while (unrelated->isOpen() && unrelated->handle() < probe->descriptor) {
            lower_descriptors.push_back(std::move(unrelated));
            unrelated = std::make_unique<QTemporaryFile>();
            ok &= check(unrelated->open(), "unrelated files consume lower free descriptors");
        }
        if (unrelated->isOpen()) {
            ok &= check(unrelated->write(original_content) == original_content.size() &&
                    unrelated->flush() && unrelated->seek(0),
                "unrelated file starts with known bytes and native offset");
        }
    }

    {
        const std::lock_guard lock(probe->mutex);
        probe->release_worker = true;
        probe->changed.notify_all();
    }
    ok &= check(Cleanup::wait_until_idle(std::chrono::steady_clock::now() + timeout),
        "descriptor lifetime cleanup settles after releasing the I/O worker");
    if (unrelated->isOpen()) {
        ok &= check(::lseek(unrelated->handle(), 0, SEEK_CUR) == 0,
            "retired I/O does not consume or overwrite an unrelated descriptor");
        ok &= check(unrelated->seek(0) && unrelated->readAll() == original_content,
            "unrelated file content survives terminal cleanup");
    }
    {
        const std::lock_guard lock(probe->mutex);
        ok &= check(!probe->barrier_timed_out, "descriptor barrier was explicitly released");
    }
    internal::set_posix_pty_io_hook_for_testing(nullptr);
    {
        const std::lock_guard lock(pty_descriptor_probe_mutex);
        pty_descriptor_probe.reset();
    }
    return ok;
}

} // namespace vnm_terminal::test_helpers
