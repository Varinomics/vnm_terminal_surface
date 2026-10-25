#pragma once

#if defined(__linux__)

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace vnm_terminal::internal {

// Older kernels cannot signal a retained process-group identity. This bounded
// fallback retains each observed member instead: stat and pidfd_send_signal use
// the same /proc descriptor, never a checked-then-reused numeric PID. It is an
// observed-member cleanup policy, not atomic process-family containment.
//
// Construct before birth. The already-prepared stop worker owns its use until
// joined; serialized final cleanup may then continue the same obligations.
// Its directory buffer and member storage need no later heap allocation;
// inability to retain a native descriptor stays an error.
class Linux_terminal_foreground_custody final
{
public:
    static constexpr std::size_t k_batch_capacity = 256;

    Linux_terminal_foreground_custody() noexcept
    :
        m_directory(::opendir("/proc"))
    {
        if (!m_directory) {
            m_prepare_error = errno;
        }
        else if (::fcntl(::dirfd(m_directory), F_SETFD, FD_CLOEXEC) < 0) {
            m_prepare_error = errno;
            ::closedir(m_directory);
            m_directory = nullptr;
        }
    }

    ~Linux_terminal_foreground_custody()
    {
        for (auto& member : m_members) {
            retire(member);
        }
        if (m_directory) {
            ::closedir(m_directory);
        }
    }

    Linux_terminal_foreground_custody(const Linux_terminal_foreground_custody&) = delete;
    Linux_terminal_foreground_custody& operator=(const Linux_terminal_foreground_custody&) = delete;

    int capture(pid_t process_group, pid_t retained_session) noexcept
    {
        if (m_process_group > 0) {
            return EALREADY;
        }
        if (process_group <= 1 || retained_session <= 1 || process_group == ::getpgrp()) {
            return EINVAL;
        }
        m_process_group = process_group;
        m_session       = retained_session;
        return refresh();
    }

    // Retained members remain obligations even if they later change groups.
    // New admissions must still match the original group and retained session.
    // The caller keeps the session leader's native wait unconsumed throughout.
    int refresh() noexcept
    {
        m_refresh_complete   = false;
        m_unretained_members = false;
        if (m_prepare_error != 0) {
            return m_prepare_error;
        }
        if (m_process_group <= 1) {
            return EINVAL;
        }
        int result = 0;
        for (auto& member : m_members) {
            if (member.descriptor < 0) {
                continue;
            }
            process_state_t state;
            const int error = read_state(member.descriptor, state);
            if ((error == 0 && state.exited) ||
                (error != 0 && signal_descriptor(member.descriptor, 0) == ESRCH))
            {
                retire(member);
            }
            else if (error != 0 && result == 0) {
                result = error;
            }
        }

        ::rewinddir(m_directory);
        for (;;) {
            errno = 0;
            const auto* entry = ::readdir(m_directory);
            if (!entry) {
                if (result == 0) {
                    result = errno;
                }
                break;
            }
            const auto length = std::strlen(entry->d_name);
            pid_t pid = -1;
            const auto parsed = std::from_chars(entry->d_name, entry->d_name + length, pid);
            if (parsed.ec != std::errc{} || parsed.ptr != entry->d_name + length || pid <= 1) {
                continue;
            }
            const int descriptor = ::openat(::dirfd(m_directory), entry->d_name,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (descriptor < 0) {
                if (errno != ENOENT && errno != ESRCH && result == 0) {
                    result = errno;
                }
                continue;
            }
            const int error = admit(descriptor);
            if (error != 0 && result == 0) {
                result = error;
            }
        }
        m_refresh_complete = result == 0;
        return result;
    }

    // Signal each retained identity once per phase. Repeated scans can admit
    // late pipeline members without repeatedly interrupting a TERM handler.
    int signal_members(int signal_number) noexcept
    {
        if (signal_number <= 0 || signal_number >= NSIG) {
            return EINVAL;
        }
        int result = 0;
        for (auto& member : m_members) {
            if (member.descriptor < 0 || member.last_signal == signal_number) {
                continue;
            }
            const int error = signal_descriptor(member.descriptor, signal_number);
            if (error == 0) {
                member.last_signal = signal_number;
            }
            else if (error == ESRCH) {
                retire(member);
                m_refresh_complete = false; // Require another complete scan.
            }
            else if (result == 0) {
                result = error;
            }
        }
        if (result != 0) {
            m_refresh_complete = false;
        }
        return result;
    }

    bool empty() const noexcept
    {
        if (!m_refresh_complete || m_unretained_members) {
            return false;
        }
        for (const auto& member : m_members) {
            if (member.descriptor >= 0) {
                return false;
            }
        }
        return true;
    }

private:
    struct member_t
    {
        int   descriptor  = -1;
        int   last_signal = 0;
        dev_t device      = 0;
        ino_t inode       = 0;
    };

    struct process_state_t
    {
        pid_t group   = -1;
        pid_t session = -1;
        bool  exited  = false;
    };

    static void retire(member_t& member) noexcept
    {
        if (member.descriptor >= 0) {
            ::close(member.descriptor);
        }
        member = {};
    }

    static int signal_descriptor(int descriptor, int signal_number) noexcept
    {
#if defined(SYS_pidfd_send_signal)
        // Linux 5.1 accepts an open /proc/PID directory here. flags=0 targets
        // this retained process, without the Linux 6.9 process-group extension.
        return ::syscall(SYS_pidfd_send_signal, descriptor, signal_number,
            nullptr, 0U) == 0 ? 0 : errno;
#else
        (void)descriptor;
        (void)signal_number;
        return ENOTSUP;
#endif
    }

    static int read_state(int descriptor, process_state_t& state) noexcept
    {
        const int stat_descriptor = ::openat(descriptor, "stat", O_RDONLY | O_CLOEXEC);
        if (stat_descriptor < 0) {
            return errno;
        }
        char buffer[4096]{};
        ssize_t count = -1;
        do {
            count = ::read(stat_descriptor, buffer, sizeof(buffer));
        } while (count < 0 && errno == EINTR);
        const int error = count < 0 ? errno : EIO;
        ::close(stat_descriptor);
        if (count <= 0) {
            return error;
        }
        std::string_view stat(buffer, static_cast<std::size_t>(count));
        const auto comm_end = stat.rfind(')');
        if (comm_end == std::string_view::npos || comm_end + 4 >= stat.size() ||
            stat[comm_end + 1] != ' ' || stat[comm_end + 3] != ' ')
        {
            return EIO;
        }
        const char native_state = stat[comm_end + 2];
        state.exited = native_state == 'Z' || native_state == 'X' || native_state == 'x';
        stat.remove_prefix(comm_end + 4);
        pid_t fields[3]{}; // ppid, pgrp, session from this retained proc inode
        for (auto& field : fields) {
            const auto parsed = std::from_chars(stat.data(), stat.data() + stat.size(), field);
            if (parsed.ec != std::errc{} || parsed.ptr == stat.data() + stat.size() || *parsed.ptr != ' ') {
                return EIO;
            }
            stat.remove_prefix(static_cast<std::size_t>(parsed.ptr - stat.data()) + 1);
        }
        state.group   = fields[1];
        state.session = fields[2];
        return 0;
    }

    // Takes ownership of descriptor on every path. Proc inode identity, rather
    // than a numeric PID, deduplicates members across scans and PID reuse.
    int admit(int descriptor) noexcept
    {
        process_state_t state;
        int error = read_state(descriptor, state);
        if (error != 0) {
            if (signal_descriptor(descriptor, 0) == ESRCH) {
                error = 0;
            }
            ::close(descriptor);
            return error;
        }
        if (state.exited || state.group != m_process_group || state.session != m_session) {
            ::close(descriptor);
            return 0;
        }
        struct stat identity{};
        if (::fstat(descriptor, &identity) < 0) {
            error = errno;
            ::close(descriptor);
            return error;
        }
        member_t* available = nullptr;
        for (auto& member : m_members) {
            if (member.descriptor >= 0 && member.device == identity.st_dev && member.inode == identity.st_ino) {
                ::close(descriptor);
                return 0;
            }
            if (member.descriptor < 0 && !available) {
                available = &member;
            }
        }
        if (!available) {
            // Capacity bounds simultaneous retention, not the terminal's
            // group size. Later scans reuse retired slots; until then this
            // observed work prevents an empty/settled result.
            m_unretained_members = true;
            ::close(descriptor);
            return 0;
        }
        available->descriptor = descriptor;
        available->device     = identity.st_dev;
        available->inode      = identity.st_ino;
        return 0;
    }

    std::array<member_t, k_batch_capacity> m_members{};
    DIR*  m_directory          = nullptr;
    pid_t m_process_group      = -1;
    pid_t m_session            = -1;
    int   m_prepare_error      = 0;
    bool  m_refresh_complete   = false;
    bool  m_unretained_members = false;
};

} // namespace vnm_terminal::internal

#endif
