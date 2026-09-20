#pragma once

#if defined(__linux__) || defined(__APPLE__)

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <vector>
#endif

namespace vnm_terminal::internal {

// One retained process-group identity. An owned root has exactly one native
// waiter. Linux also retains the kernel's struct pid, so a queued group signal
// cannot resolve a reused numeric PGID even after the root has been collected.
// Platforms without group pidfds may signal an owned root only before this
// same object consumes its wait. An arbitrary foreground group has no such
// fallback: a PID check followed by kill(-pgid) is not an identity fence.
class Posix_process_group_custody final
{
public:
    explicit Posix_process_group_custody(pid_t owned_child)
    :
        m_pid(owned_child),
        m_wait_owned(owned_child > 1)
    {
        open_group_reference();
    }

    ~Posix_process_group_custody()
    {
        if (m_group_fd >= 0) {
            ::close(m_group_fd);
        }
    }

    Posix_process_group_custody(const Posix_process_group_custody&) = delete;
    Posix_process_group_custody& operator=(const Posix_process_group_custody&) = delete;

    pid_t identity() const noexcept { return m_pid; }

    // The backend allocates this owner before forkpty; adoption adds no heap
    // allocation to the post-birth error paths. Call once before workers start.
    void adopt_owned_child(pid_t owned_child) noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (m_pid > 0 || owned_child <= 1) {
            return;
        }
        m_pid = owned_child;
        m_wait_owned = true;
        open_group_reference();
    }

    static std::shared_ptr<Posix_process_group_custody> capture_foreground(
        pid_t process_group, pid_t retained_session)
    {
#if defined(__linux__)
        try {
            auto result = std::shared_ptr<Posix_process_group_custody>(
                new Posix_process_group_custody(process_group, false));
            if (!result->m_group_signals_supported ||
                !result->belongs_to_session(retained_session))
            {
                return {};
            }
            return result;
        }
        catch (...) {
            return {}; // No numeric fallback on allocation/identity failure.
        }
#else
        (void)process_group;
        (void)retained_session;
        return {};
#endif
    }

    // Return errno, not a boolean which could conflate absent and unconfirmed.
    int signal_group(int signal_number) const noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (m_pid <= 1 || m_pid == ::getpgrp()) {
            return EPERM;
        }
        if (m_group_signals_supported) {
            return signal_descriptor(signal_number);
        }
        if (!m_wait_owned) {
            return ESTALE;
        }
        const int error = ::kill(-m_pid, signal_number) == 0 ? 0 : errno;
#if defined(__APPLE__)
        // XNU killpg1 filters zombies, then reports EPERM when no member was
        // signalled. The retained wait pins this PGID throughout the snapshot;
        // only a complete snapshot with no live members can disambiguate EPERM.
        if (error == EPERM && group_has_only_zombies()) {
            return ESRCH;
        }
#endif
        return error;
    }

    int signal_root(int signal_number) const noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (m_pid <= 1 || !m_wait_owned) {
            return ESTALE;
        }
        return ::kill(m_pid, signal_number) == 0 ? 0 : errno;
    }

    int observe_exit() noexcept
    {
        siginfo_t info{};
        int result = 0;
        do {
            result = ::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result == 0 && info.si_pid == m_pid) {
            return 0;
        }
        const int error = result < 0 ? errno : EIO;
        if (error == ECHILD) {
            // A competing/automatic reaper invalidates the numeric fallback.
            const std::lock_guard lock(m_mutex);
            m_wait_owned = false;
        }
        return error;
    }

    // Sole consuming wait, called after non-consuming exit observation or by
    // shutdown after all workers joined. Retirement and signalling serialize
    // on the same mutex, not on a copied "not reaped yet" boolean.
    pid_t reap(int* status) noexcept
    {
        const std::lock_guard lock(m_mutex);
        if (!m_wait_owned) {
            errno = ECHILD;
            return -1;
        }
        pid_t result = -1;
        do {
            result = ::waitpid(m_pid, status, 0);
        } while (result < 0 && errno == EINTR);
        if (result == m_pid || (result < 0 && errno == ECHILD)) {
            m_wait_owned = false;
        }
        return result;
    }

private:
#if defined(__APPLE__)
    bool group_has_only_zombies() const noexcept
    {
        int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, m_pid};
        std::size_t bytes = 0;
        if (::sysctl(query, 4, nullptr, &bytes, nullptr, 0) != 0) {
            return false;
        }
        try {
            // A growing group makes the second query fail with ENOMEM. Keep
            // the original permission error rather than trust a partial list.
            std::vector<kinfo_proc> members(bytes / sizeof(kinfo_proc) + 1);
            bytes = members.size() * sizeof(kinfo_proc);
            if (::sysctl(query, 4, members.data(), &bytes, nullptr, 0) != 0 ||
                bytes % sizeof(kinfo_proc) != 0)
            {
                return false;
            }
            for (std::size_t i = 0; i < bytes / sizeof(kinfo_proc); ++i) {
                if (members[i].kp_proc.p_stat != SZOMB) {
                    return false;
                }
            }
            return true;
        }
        catch (...) {
            return false;
        }
    }
#endif

    Posix_process_group_custody(pid_t process_group, bool wait_owned)
    :
        m_pid(process_group),
        m_wait_owned(wait_owned)
    {
        open_group_reference();
    }

    int signal_descriptor(int signal_number) const noexcept
    {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
        // Linux UAPI PIDFD_SIGNAL_PROCESS_GROUP (since 6.9). Older headers
        // need not define it; unsupported kernels return EINVAL/ENOSYS and
        // never fall back to an unowned foreground numeric process group.
        constexpr unsigned int k_signal_process_group = 1U << 2;
        return ::syscall(SYS_pidfd_send_signal, m_group_fd, signal_number,
            nullptr, k_signal_process_group) == 0 ? 0 : errno;
#else
        (void)signal_number;
        return ENOTSUP;
#endif
    }

    void open_group_reference() noexcept
    {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
        if (m_pid <= 1) {
            return;
        }
        char path[64]{};
        const int length = std::snprintf(path, sizeof(path), "/proc/%ld", static_cast<long>(m_pid));
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
            return;
        }
        m_group_fd = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (m_group_fd >= 0) {
            const int error = signal_descriptor(0);
            m_group_signals_supported = error == 0 || error == ESRCH;
        }
#endif
    }

    bool belongs_to_session(pid_t retained_session) const noexcept
    {
#if defined(__linux__)
        if (m_group_fd < 0 || retained_session <= 1) {
            return false;
        }
        const int fd = ::openat(m_group_fd, "stat", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        char buffer[4096]{};
        ssize_t count = -1;
        do {
            count = ::read(fd, buffer, sizeof(buffer));
        } while (count < 0 && errno == EINTR);
        ::close(fd);
        if (count <= 0) {
            return false;
        }
        std::string_view stat(buffer, static_cast<std::size_t>(count));
        const auto comm_end = stat.rfind(')');
        if (comm_end == std::string_view::npos || comm_end + 4 >= stat.size() ||
            stat[comm_end + 1] != ' ' || stat[comm_end + 3] != ' ')
        {
            return false;
        }
        stat.remove_prefix(comm_end + 4); // skip comm, state and their spaces
        pid_t fields[3]{}; // ppid, pgrp, session from the retained proc inode
        for (auto& field : fields) {
            const auto parsed = std::from_chars(stat.data(), stat.data() + stat.size(), field);
            if (parsed.ec != std::errc{} || parsed.ptr == stat.data() + stat.size() || *parsed.ptr != ' ') {
                return false;
            }
            stat.remove_prefix(static_cast<std::size_t>(parsed.ptr - stat.data()) + 1);
        }
        return fields[1] == m_pid && fields[2] == retained_session;
#else
        (void)retained_session;
        return false;
#endif
    }

    pid_t m_pid = -1;
    int m_group_fd = -1;
    bool m_group_signals_supported = false;
    mutable std::mutex m_mutex;
    bool m_wait_owned = false;
};

} // namespace vnm_terminal::internal

#endif
