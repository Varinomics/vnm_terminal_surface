#include "posix_terminal_process_owner.h"

#include "posix_process_group_custody.h"
#if defined(__linux__)
#include "linux_terminal_foreground_custody.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <util.h>
#else
#include <pty.h>
#include <utmp.h>
#include <sys/syscall.h>
#endif

namespace vnm_terminal::internal {
namespace {

constexpr std::chrono::seconds k_exec_confirmation_timeout(10);
constexpr std::chrono::milliseconds k_natural_exit_grace(250);
constexpr std::chrono::milliseconds k_natural_exit_kill(500);
constexpr std::chrono::milliseconds k_cleanup_observation_interval(10);
#if defined(__APPLE__)
constexpr std::chrono::milliseconds k_exit_flow_check_interval(250);
#endif

class Owned_descriptor
{
public:
    explicit Owned_descriptor(int descriptor = -1) noexcept : m_descriptor(descriptor) {}
    ~Owned_descriptor() { reset(); }
    Owned_descriptor(const Owned_descriptor&) = delete;
    Owned_descriptor& operator=(const Owned_descriptor&) = delete;
    Owned_descriptor(Owned_descriptor&& other) noexcept : m_descriptor(other.release()) {}
    Owned_descriptor& operator=(Owned_descriptor&& other) noexcept
    {
        reset(other.release());
        return *this;
    }

    int get() const noexcept { return m_descriptor; }
    int release() noexcept { return std::exchange(m_descriptor, -1); }
    void reset(int descriptor = -1) noexcept
    {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
        m_descriptor = descriptor;
    }

private:
    int m_descriptor = -1;
};

bool has_nul(const std::string& value)
{
    return value.find('\0') != std::string::npos;
}

std::string native_error(const char* operation, int error)
{
    return std::string(operation) + ": " + std::strerror(error);
}

bool elevate_descriptor(Owned_descriptor& descriptor, int minimum)
{
    const int duplicate = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, minimum);
    if (duplicate < 0) {
        return false;
    }
    descriptor.reset(duplicate);
    return true;
}

[[noreturn]] void reject_child(int descriptor, int error) noexcept
{
    const auto* bytes = reinterpret_cast<const char*>(&error);
    std::size_t written = 0;
    while (written < sizeof(error)) {
        const auto count = ::write(descriptor, bytes + written, sizeof(error) - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        }
        else if (count < 0 && errno == EINTR) {
            continue;
        }
        else {
            break;
        }
    }
    ::_exit(127);
}

int close_descriptor(int descriptor) noexcept
{
    while (::close(descriptor) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return errno == EBADF ? 0 : errno;
    }
    return 0;
}

#if defined(__linux__)
// Linux getdents64 records have a fixed prefix followed by a NUL-terminated name.
struct descriptor_directory_entry_t
{
    std::uint64_t inode;
    std::int64_t offset;
    unsigned short length;
    unsigned char type;
    char name[1];
};

int close_descriptors_from_directory(const std::vector<int>& kept) noexcept
{
    // Open in the child: a /proc/self/fd directory opened before fork would
    // enumerate the parent's live table, not this child's inherited table.
    Owned_descriptor directory(::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) {
        return errno;
    }
    char buffer[4096];
    constexpr auto name_offset = offsetof(descriptor_directory_entry_t, name);
    for (;;) {
        const auto bytes = ::syscall(SYS_getdents64, directory.get(), buffer, sizeof(buffer));
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (bytes == 0) {
            return close_descriptor(directory.release());
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(bytes)) {
            const auto remaining = static_cast<std::size_t>(bytes) - offset;
            if (remaining <= name_offset) {
                return EIO;
            }
            unsigned short length = 0;
            std::memcpy(&length,
                buffer + offset + offsetof(descriptor_directory_entry_t, length), sizeof(length));
            if (length <= name_offset || length > remaining) {
                return EIO;
            }
            const char* name = buffer + offset + name_offset;
            const auto name_capacity = length - name_offset;
            int descriptor = 0;
            std::size_t digit = 0;
            while (digit < name_capacity && name[digit] >= '0' && name[digit] <= '9') {
                const int value = name[digit] - '0';
                if (descriptor > (INT_MAX - value) / 10) {
                    return EOVERFLOW;
                }
                descriptor = descriptor * 10 + value;
                ++digit;
            }
            if (digit == name_capacity) {
                return EIO;
            }
            if (digit != 0 && name[digit] == '\0' && descriptor > 2 &&
                descriptor != directory.get() && !std::binary_search(kept.begin(), kept.end(), descriptor))
            {
                const int error = close_descriptor(descriptor);
                if (error != 0) {
                    return error;
                }
            }
            offset += length;
        }
    }
}
#endif

// Storage, descriptor copies and the sorted keep list are prepared before fork.
// The child uses only native operations between login_tty and execve.
class Prepared_launch
{
public:
    std::vector<char*> argv;
    std::vector<char*> environment;
    std::vector<Owned_descriptor> inherited;
    std::vector<int> kept_descriptors;
#if defined(__APPLE__)
    std::vector<proc_fdinfo> descriptor_batch;
#endif

    int close_unlisted_descriptors() noexcept
    {
#if defined(__APPLE__)
        const auto capacity = static_cast<int>(descriptor_batch.size() * sizeof(proc_fdinfo));
        for (;;) {
            const int bytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0,
                descriptor_batch.data(), capacity);
            if (bytes <= 0) {
                return errno != 0 ? errno : EIO;
            }
            if (bytes > capacity || bytes % sizeof(proc_fdinfo) != 0) {
                return EIO;
            }
            bool closed = false;
            for (std::size_t index = 0; index < bytes / sizeof(proc_fdinfo); ++index) {
                const int descriptor = descriptor_batch[index].proc_fd;
                if (descriptor > 2 &&
                    !std::binary_search(kept_descriptors.begin(), kept_descriptors.end(), descriptor))
                {
                    const int error = close_descriptor(descriptor);
                    if (error != 0) {
                        return error;
                    }
                    closed = true;
                }
            }
            if (bytes < capacity) {
                return 0;
            }
            // The preallocated batch exceeds the entire keep list. A full
            // batch must remove entries before listing the remaining table.
            if (!closed) {
                return EIO;
            }
        }
#else
#if defined(SYS_close_range)
        unsigned int first = 3;
        for (int descriptor : kept_descriptors) {
            const auto next = static_cast<unsigned int>(descriptor);
            if (first < next && ::syscall(SYS_close_range, first, next - 1, 0U) < 0) {
                return close_descriptors_from_directory(kept_descriptors);
            }
            first = next + 1;
        }
        // Limits govern new allocations, not already-open descriptors. The
        // final gap must cover the complete descriptor number space.
        if (::syscall(SYS_close_range, first, UINT_MAX, 0U) == 0) {
            return 0;
        }
#endif
        return close_descriptors_from_directory(kept_descriptors);
#endif
    }

    [[noreturn]] void execute(
        const terminal_process_launch_t& request,
        int                             slave,
        int                             error_descriptor) noexcept
    {
        if (::login_tty(slave) < 0) {
            reject_child(error_descriptor, errno);
        }
        for (std::size_t index = 0; index < inherited.size(); ++index) {
            if (::dup2(inherited[index].get(), request.inherited[index].target) < 0) {
                reject_child(error_descriptor, errno);
            }
        }
        const int close_error = close_unlisted_descriptors();
        if (close_error != 0) {
            reject_child(error_descriptor, close_error);
        }
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int signal = 1; signal < NSIG; ++signal) {
            if (signal != SIGKILL && signal != SIGSTOP &&
                ::sigaction(signal, &action, nullptr) < 0 && errno != EINVAL)
            {
                reject_child(error_descriptor, errno);
            }
        }
        sigset_t signals;
        sigemptyset(&signals);
        if (::sigprocmask(SIG_SETMASK, &signals, nullptr) < 0) {
            reject_child(error_descriptor, errno);
        }
        if (!request.working_directory.empty() && ::chdir(request.working_directory.c_str()) < 0) {
            reject_child(error_descriptor, errno);
        }
        ::execve(request.executable.c_str(), argv.data(), environment.data());
        reject_child(error_descriptor, errno);
    }
};

struct exec_confirmation_t
{
    int error = 0;
    bool child_rejected = false;
};

exec_confirmation_t confirm_exec(int descriptor) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + k_exec_confirmation_timeout;
    int child_error = 0;
    std::size_t received = 0;
    auto* bytes = reinterpret_cast<char*>(&child_error);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            return {ETIMEDOUT, false};
        }
        pollfd entry{descriptor, POLLIN, 0};
        const int result = ::poll(&entry, 1, static_cast<int>(remaining));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {errno, false};
        }
        if (result == 0) {
            continue;
        }
        const auto count = ::read(descriptor, bytes + received, sizeof(child_error) - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            if (received == sizeof(child_error)) {
                return {child_error != 0 ? child_error : EIO, child_error != 0};
            }
        }
        else if (count == 0) {
            return {received == 0 ? 0 : EIO, false};
        }
        else if (errno != EINTR && errno != EAGAIN) {
            return {errno, false};
        }
    }
}

class Posix_terminal_process_owner final : public Terminal_process_owner
{
public:
    ~Posix_terminal_process_owner() override { finish(); }

    terminal_process_launch_result_t launch(const terminal_process_launch_t& request) override;

    bool stop(std::chrono::milliseconds graceful_interval,
        std::chrono::milliseconds kill_interval, std::string* error) noexcept override
    {
        try {
            const std::lock_guard lock(m_stop_mutex);
            if (!m_stop_requested) {
                m_stop_requested = true;
                m_grace = std::max(graceful_interval, std::chrono::milliseconds::zero());
                m_kill = std::max(kill_interval, std::chrono::milliseconds::zero());
            }
            m_stop_cv.notify_all();
            return true;
        }
        catch (...) {
            // Even a failed request stays committed. The prestarted worker
            // can still apply its zero-grace default without another request.
            m_stop_requested.store(true);
            m_stop_cv.notify_all();
            try {
                if (error) {
                    *error = "cannot commit terminal stop request";
                }
            }
            catch (...) {}
            return false;
        }
    }

    std::optional<terminal_process_exit_t> wait_root() override
    {
        if (m_pid <= 1) {
            return {};
        }
        siginfo_t status{};
        const int error = m_root.observe_exit(&status);
        if (error != 0) {
            record_cleanup_error(error);
            return {};
        }
        terminal_process_exit_t result;
        if (status.si_code == CLD_EXITED) {
            result.exit_code = status.si_status;
        }
        else if (status.si_code == CLD_KILLED || status.si_code == CLD_DUMPED) {
            result.signal = status.si_status;
        }
        else {
            record_cleanup_error(EIO);
            return {};
        }
        m_root_observed.store(true);
        {
            const std::lock_guard lock(m_stop_mutex);
            if (!m_stop_requested) {
                m_stop_requested = true;
                m_grace = k_natural_exit_grace;
                m_kill = k_natural_exit_kill;
            }
        }
        m_stop_cv.notify_all();
        // Keep the backend reader live while retained groups handle TERM and
        // retire. Only then may root observation begin the finite output drain.
        {
            std::unique_lock lock(m_stop_mutex);
            m_stop_cv.wait(lock, [this] { return m_cleanup_finished; });
        }
        return result;
    }

    void finish() override
    {
        const std::lock_guard finish_lock(m_finish_mutex);
        if (m_finished) {
            return;
        }
        {
            const std::lock_guard lock(m_stop_mutex);
            m_finish_requested = true;
            m_stop_requested = true;
        }
        m_stop_cv.notify_all();
        if (m_stop_thread.joinable()) {
            m_stop_thread.join();
        }
#if defined(__linux__)
        // A bounded stop phase is not a settlement receipt. Keep the pinned
        // session and endpoints while later batches or transient observations
        // remain outstanding. This runs on the retained cleanup worker, never
        // on the public stop call; a later positive observation can settle it.
        while (m_foreground_attempted) {
            refresh_foreground(SIGKILL);
            if (m_foreground_fallback.empty()) {
                break;
            }
            if (m_foreground_retained) {
                (void)m_foreground.signal_group(SIGKILL);
            }
            std::this_thread::sleep_for(k_cleanup_observation_interval);
        }
#endif
        bool reaped = true;
        if (m_pid > 1) {
            int status = 0;
            reaped = m_root.reap(&status) == m_pid;
        }
        // The backend has retired its accepted output before this call. These
        // endpoints stay owned through cleanup and the backend's finite drain.
        m_slave.reset();
        m_master.reset();
        m_finished = true;
        // This confirms our sole child wait and retained cleanup operations.
        // It does not claim containment of escaped or other process groups.
        m_confirmed.store(reaped && m_cleanup_error.load() == 0);
    }

    bool confirmed() const noexcept override { return m_confirmed.load(); }

private:
#if defined(__APPLE__)
    void resume_exiting_output() noexcept
    {
        if (m_root_observed.load()) {
            return;
        }
        int queued = 0;
        if (::ioctl(m_master.get(), TIOCOUTQ, &queued) != 0 || queued <= 0) {
            return;
        }
        int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, m_pid};
        kinfo_proc process{};
        std::size_t bytes = sizeof(process);
        if (::sysctl(query, 4, &process, &bytes, nullptr, 0) != 0 ||
            bytes != sizeof(process) || process.kp_proc.p_pid != m_pid ||
            (process.kp_proc.p_flag & P_WEXIT) == 0)
        {
            return;
        }
        // XNU can wait for flow-stopped output before waitid can observe exit.
        // The retained wait pins this child, and P_WEXIT establishes committed
        // exit. Never resume a live child's intentionally stopped output.
        if (::ioctl(m_master.get(), TIOCSTART) != 0 && errno != EIO && errno != ENXIO) {
            record_cleanup_error(errno);
        }
    }
#endif

    void record_cleanup_error(int error) noexcept
    {
        if (error != 0 && error != ESRCH) {
            int expected = 0;
            m_cleanup_error.compare_exchange_strong(expected, error);
        }
    }

    void signal_cleanup(int signal) noexcept
    {
        const pid_t foreground = ::tcgetpgrp(m_master.get());
#if defined(__APPLE__)
        if (foreground > 1 && ::ioctl(m_master.get(), TIOCSIG, signal) < 0) {
            record_cleanup_error(errno);
        }
#else
        if (!m_foreground_attempted && foreground > 1 && foreground != m_pid) {
            m_foreground_attempted = true;
            (void)m_foreground_fallback.capture(foreground, m_pid);
            m_foreground_retained = m_foreground.adopt_foreground_group(foreground, m_pid);
            m_foreground_fallback_active = !m_foreground_retained;
        }
        if (m_foreground_retained) {
            record_cleanup_error(m_foreground.signal_group(signal));
        }
        refresh_foreground(signal);
#endif
        record_cleanup_error(m_root.signal_group(signal));
        // A failed start can precede login_tty's setsid. The exact unreaped
        // child remains signalable even while its new process group is absent.
        record_cleanup_error(m_root.signal_root(signal));
    }

#if defined(__linux__)
    void refresh_foreground(int signal) noexcept
    {
        if (m_foreground_attempted) {
            (void)m_foreground_fallback.refresh();
            if (m_foreground_fallback_active) {
                (void)m_foreground_fallback.signal_members(signal);
            }
        }
    }
#endif

    void wait_stop_interval(std::chrono::milliseconds interval, int signal)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::time_point::max() - now);
        const auto deadline = interval >= remaining
            ? std::chrono::steady_clock::time_point::max() : now + interval;
        while (std::chrono::steady_clock::now() < deadline) {
#if defined(__APPLE__)
            (void)signal;
            resume_exiting_output();
#else
            refresh_foreground(signal);
#endif
            std::unique_lock lock(m_stop_mutex);
            m_stop_cv.wait_until(lock, std::min(deadline,
                std::chrono::steady_clock::now() + k_cleanup_observation_interval));
        }
    }

    void stop_loop() noexcept
    {
        std::chrono::milliseconds grace{};
        std::chrono::milliseconds kill{};
        {
            std::unique_lock lock(m_stop_mutex);
            while (!m_finish_requested && !(m_birth_ready && m_stop_requested)) {
#if defined(__APPLE__)
                if (m_birth_ready) {
                    m_stop_cv.wait_for(lock, k_exit_flow_check_interval);
                    lock.unlock();
                    resume_exiting_output();
                    lock.lock();
                }
                else {
                    m_stop_cv.wait(lock);
                }
#else
                m_stop_cv.wait(lock);
#endif
            }
            if (!m_birth_ready) {
                return;
            }
            grace = m_grace;
            kill = m_kill;
        }
        signal_cleanup(SIGTERM);
        wait_stop_interval(grace, SIGTERM);
        signal_cleanup(SIGKILL);
        wait_stop_interval(kill, SIGKILL);
#if defined(__linux__)
        if (m_foreground_attempted) {
            (void)m_foreground_fallback.refresh();
        }
#else
        // Explicit stop can reach XNU's ttywait after the grace interval. Keep
        // the same bounded-cadence nudge alive until exact root observation.
        std::unique_lock lock(m_stop_mutex);
        while (!m_root_observed.load() && !m_finish_requested) {
            m_stop_cv.wait_for(lock, k_exit_flow_check_interval);
            lock.unlock();
            resume_exiting_output();
            lock.lock();
        }
        lock.unlock();
#endif
        {
            const std::lock_guard lock(m_stop_mutex);
            m_cleanup_finished = true;
        }
        m_stop_cv.notify_all();
    }

    Posix_process_group_custody m_root{-1};
    Posix_process_group_custody m_foreground{-1};
#if defined(__linux__)
    Linux_terminal_foreground_custody m_foreground_fallback;
    bool m_foreground_fallback_active = false;
#endif
    Owned_descriptor m_master;
    Owned_descriptor m_slave;
    std::thread m_stop_thread;
    std::mutex m_stop_mutex;
    std::condition_variable m_stop_cv;
    std::mutex m_finish_mutex;
    std::chrono::milliseconds m_grace{};
    std::chrono::milliseconds m_kill{};
    pid_t m_pid = -1;
    bool m_birth_ready = false;
    std::atomic_bool m_stop_requested{false};
    bool m_finish_requested = false;
    bool m_finished = false;
    bool m_cleanup_finished = false;
    bool m_foreground_attempted = false;
    bool m_foreground_retained = false;
    std::atomic_bool m_root_observed{false};
    std::atomic_bool m_confirmed{true};
    std::atomic_int m_cleanup_error{0};
};

terminal_process_launch_result_t Posix_terminal_process_owner::launch(
    const terminal_process_launch_t& request)
{
    terminal_process_launch_result_t result;
    if (m_pid > 1 || m_finished) {
        result.error = "terminal process owner already used";
        return result;
    }
    if (request.executable.empty() || request.argv.empty() ||
        has_nul(request.executable) || has_nul(request.working_directory) ||
        request.rows <= 0 || request.rows > USHRT_MAX ||
        request.columns <= 0 || request.columns > USHRT_MAX ||
        request.pixel_width < 0 || request.pixel_width > USHRT_MAX ||
        request.pixel_height < 0 || request.pixel_height > USHRT_MAX)
    {
        result.error = "invalid terminal process launch request";
        return result;
    }
    Prepared_launch prepared;
    for (const auto& argument : request.argv) {
        if (has_nul(argument)) {
            result.error = "terminal argument contains a NUL byte";
            return result;
        }
        prepared.argv.push_back(const_cast<char*>(argument.c_str()));
    }
    prepared.argv.push_back(nullptr);
    for (const auto& variable : request.environment) {
        if (has_nul(variable)) {
            result.error = "terminal environment contains a NUL byte";
            return result;
        }
        prepared.environment.push_back(const_cast<char*>(variable.c_str()));
    }
    prepared.environment.push_back(nullptr);
    int minimum = 3;
    for (const auto& descriptor : request.inherited) {
        if (descriptor.source < 0 || descriptor.target < 3 || descriptor.target == INT_MAX) {
            result.error = "invalid inherited terminal descriptor";
            return result;
        }
        minimum = std::max(minimum, descriptor.target + 1);
        prepared.kept_descriptors.push_back(descriptor.target);
    }
    std::sort(prepared.kept_descriptors.begin(), prepared.kept_descriptors.end());
    if (std::adjacent_find(prepared.kept_descriptors.begin(), prepared.kept_descriptors.end()) !=
        prepared.kept_descriptors.end())
    {
        result.error = "duplicate inherited terminal descriptor target";
        return result;
    }
    struct sigaction child_action{};
    if (::sigaction(SIGCHLD, nullptr, &child_action) < 0 ||
        child_action.sa_handler == SIG_IGN || (child_action.sa_flags & SA_NOCLDWAIT) != 0)
    {
        result.error = "terminal process ownership requires retained child wait status";
        return result;
    }
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) < 0) {
        result.error = native_error("terminal exec pipe", errno);
        return result;
    }
    Owned_descriptor startup_read(descriptors[0]);
    Owned_descriptor startup_write(descriptors[1]);
    if (!elevate_descriptor(startup_read, minimum) || !elevate_descriptor(startup_write, minimum)) {
        result.error = native_error("retain terminal exec pipe", errno);
        return result;
    }
    prepared.kept_descriptors.push_back(startup_write.get());
    std::sort(prepared.kept_descriptors.begin(), prepared.kept_descriptors.end());
#if defined(__APPLE__)
    // Repeated kernel lists close the actual child table, even when descriptors
    // exceed subsequently lowered soft and hard limits. No child heap work is
    // needed, and a partial batch cannot hide the unlisted remainder.
    constexpr std::size_t k_descriptor_batch_margin = 64;
    if (prepared.kept_descriptors.size() > INT_MAX / sizeof(proc_fdinfo) - k_descriptor_batch_margin) {
        result.error = "too many inherited terminal descriptors";
        return result;
    }
    prepared.descriptor_batch.resize(prepared.kept_descriptors.size() + k_descriptor_batch_margin);
    if (::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, prepared.descriptor_batch.data(),
            static_cast<int>(prepared.descriptor_batch.size() * sizeof(proc_fdinfo))) <= 0)
    {
        result.error = native_error("list terminal descriptors", errno != 0 ? errno : EIO);
        return result;
    }
#endif
    prepared.inherited.reserve(request.inherited.size());
    for (const auto& descriptor : request.inherited) {
        const int duplicate = ::fcntl(descriptor.source, F_DUPFD_CLOEXEC, minimum);
        if (duplicate < 0) {
            result.error = native_error("retain inherited terminal descriptor", errno);
            return result;
        }
        prepared.inherited.emplace_back(duplicate);
    }
    winsize dimensions{};
    dimensions.ws_row = static_cast<unsigned short>(request.rows);
    dimensions.ws_col = static_cast<unsigned short>(request.columns);
    dimensions.ws_xpixel = static_cast<unsigned short>(request.pixel_width);
    dimensions.ws_ypixel = static_cast<unsigned short>(request.pixel_height);
    int master_descriptor = -1;
    int slave_descriptor = -1;
    if (::openpty(&master_descriptor, &slave_descriptor, nullptr, nullptr, &dimensions) < 0) {
        result.error = native_error("openpty", errno);
        return result;
    }
    Owned_descriptor master(master_descriptor);
    Owned_descriptor slave(slave_descriptor);
    if (!elevate_descriptor(master, minimum) || !elevate_descriptor(slave, minimum)) {
        result.error = native_error("retain terminal descriptors", errno);
        return result;
    }
    Owned_descriptor presented_master(::fcntl(master.get(), F_DUPFD_CLOEXEC, minimum));
    if (presented_master.get() < 0) {
        result.error = native_error("duplicate terminal master", errno);
        return result;
    }
    const int master_flags = ::fcntl(master.get(), F_GETFL);
    if (master_flags < 0 || ::fcntl(master.get(), F_SETFL, master_flags | O_NONBLOCK) < 0) {
        result.error = native_error("set terminal nonblocking", errno);
        return result;
    }
    if (!m_stop_thread.joinable()) {
        m_stop_thread = std::thread([this] { stop_loop(); });
    }
    {
        const std::lock_guard lock(m_stop_mutex);
        if (m_stop_requested) {
            result.error = "terminal startup was cancelled";
            return result;
        }
    }
    const pid_t child = ::fork();
    if (child < 0) {
        result.error = native_error("fork terminal", errno);
        return result;
    }
    if (child == 0) {
        ::close(startup_read.get());
        ::close(master.get());
        ::close(presented_master.get());
        prepared.execute(request, slave.get(), startup_write.get());
    }
    // No allocation or throwing operation precedes custody adoption after birth.
    m_pid = child;
    m_root.adopt_owned_child(child);
    m_master = std::move(master);
    m_slave = std::move(slave);
    m_confirmed.store(false);
    {
        const std::lock_guard lock(m_stop_mutex);
        m_birth_ready = true;
    }
    m_stop_cv.notify_all();
    startup_write.reset();
    result.native_dispatch_occurred = true;
    result.root_process_id = child;
    const auto confirmation = confirm_exec(startup_read.get());
    if (confirmation.error != 0) {
        // The child's complete errno payload establishes rejected exec, not
        // completed cleanup. A timeout or channel failure establishes neither.
        // Commit cleanup before constructing the error; the backend's reserved
        // cleanup worker retains this owner and performs the eventual finish.
        (void)stop(std::chrono::milliseconds::zero(), std::chrono::milliseconds::zero(), nullptr);
        result.start_outcome_determinate = confirmation.child_rejected;
        result.error = native_error("terminal exec", confirmation.error);
        return result;
    }
    result.accepted = true;
    result.master_descriptor = presented_master.release();
    return result;
}

} // namespace

std::unique_ptr<Terminal_process_owner> make_posix_terminal_process_owner()
{
    return std::make_unique<Posix_terminal_process_owner>();
}

} // namespace vnm_terminal::internal
