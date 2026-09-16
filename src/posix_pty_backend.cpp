#include "vnm_terminal/internal/posix_pty_backend.h"

#if defined(__linux__) || defined(__APPLE__)

#include "native_backend_io_core.h"
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <util.h>
#else
#include <pty.h>
#endif

namespace vnm_terminal::internal {

namespace {

constexpr std::chrono::milliseconds k_exit_output_drain_timeout(250);
// Upper bound on how long an I/O thread blocks in a single poll() before it
// re-checks its stop flags. poll() on a PTY master is unreliable on macOS once
// the child has exited while a descendant still holds the slave open (the
// master is neither readable/writable nor hung up, and the wake-pipe write may
// not interrupt the poll), so the I/O threads must not wait on it indefinitely
// or teardown deadlocks joining them. The reader bounds its master POLLIN poll
// with this; the writer never polls the master at all and instead waits on its
// wake pipe with this bound (see wait_for_write_capacity). The cost is one idle
// wake-up per interval per thread when the terminal is silent.
constexpr std::chrono::milliseconds k_native_master_poll_interval(100);
constexpr int k_waitpid_failure_exit_code = -1;

QString posix_error_message(QStringView context, int code)
{
    return QStringLiteral("%1: %2")
        .arg(context.toString(), QString::fromLocal8Bit(std::strerror(code)));
}

bool size_fits_pty(terminal_grid_size_t grid_size)
{
    return
        is_valid_grid_size(grid_size)                                &&
        grid_size.rows <= std::numeric_limits<unsigned short>::max() &&
        grid_size.columns <= std::numeric_limits<unsigned short>::max();
}

winsize winsize_from_grid_size(terminal_grid_size_t grid_size)
{
    winsize size{};
    size.ws_row = static_cast<unsigned short>(grid_size.rows);
    size.ws_col = static_cast<unsigned short>(grid_size.columns);
    return size;
}

class Unique_fd
{
public:
    Unique_fd() = default;

    explicit Unique_fd(int value)
    :
        m_fd(value)
    {}

    ~Unique_fd()
    {
        reset();
    }

    Unique_fd(const Unique_fd&)            = delete;
    Unique_fd& operator=(const Unique_fd&) = delete;

    Unique_fd(Unique_fd&& other) noexcept
    :
        m_fd(std::exchange(other.m_fd, -1))
    {}

    Unique_fd& operator=(Unique_fd&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }

        return *this;
    }

    int get() const
    {
        return m_fd;
    }

    int release()
    {
        return std::exchange(m_fd, -1);
    }

    void reset(int value = -1)
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }

        m_fd = value;
    }

    explicit operator bool() const
    {
        return m_fd >= 0;
    }

private:
    int m_fd = -1;
};

struct Queued_write
{
    QByteArray                 bytes;
};

struct Signal_targets
{
    std::array<pid_t, 2>       process_groups{};
    std::size_t                count = 0U;
};

struct Native_launch_data
{
    std::vector<QByteArray>    argv_bytes;
    std::vector<char*>         argv_ptrs;
    std::vector<QByteArray>    env_bytes;
    std::vector<char*>         env_ptrs;
    QByteArray                 executable;
    QByteArray                 working_directory;
};

bool string_list_has_nul(const QStringList& values)
{
    for (const QString& value : values) {
        if (value.contains(QChar(u'\0'))) {
            return true;
        }
    }

    return false;
}

std::optional<QByteArray> environment_value(
    const QProcessEnvironment& environment,
    const QString&             name)
{
    if (!environment.contains(name)) {
        return std::nullopt;
    }

    return environment.value(name).toLocal8Bit();
}

std::optional<QByteArray> resolved_executable(
    const QByteArray&          executable,
    const QProcessEnvironment& environment,
    const QString&             working_directory)
{
    const auto usable_candidate = [&working_directory](QByteArray candidate)
        -> std::optional<QByteArray> {
        QString candidate_text = QFile::decodeName(candidate);
        if (!QDir::isAbsolutePath(candidate_text)) {
            candidate_text = QDir(working_directory).absoluteFilePath(candidate_text);
        }
        candidate = QFile::encodeName(QDir::cleanPath(candidate_text));
        if (::access(candidate.constData(), X_OK) == 0) {
            return candidate;
        }
        return std::nullopt;
    };

    if (executable.contains('/')) {
        return usable_candidate(executable);
    }

    QByteArray path = environment_value(environment, QStringLiteral("PATH"))
        .value_or(QByteArrayLiteral("/usr/local/bin:/usr/bin:/bin"));
    if (path.isEmpty()) {
        path = QByteArrayLiteral(".");
    }

    int start = 0;
    for (;;) {
        const int separator = path.indexOf(':', start);
        QByteArray directory =
            separator < 0 ? path.mid(start) : path.mid(start, separator - start);
        if (directory.isEmpty()) {
            directory = QByteArrayLiteral(".");
        }

        QByteArray candidate = directory;
        if (!candidate.endsWith('/')) {
            candidate.append('/');
        }
        candidate.append(executable);
        if (std::optional<QByteArray> resolved =
                usable_candidate(std::move(candidate));
            resolved.has_value())
        {
            return resolved;
        }

        if (separator < 0) {
            break;
        }

        start = separator + 1;
    }

    return std::nullopt;
}

std::optional<Native_launch_data> make_native_launch_data(
    const Terminal_effective_launch_config& config)
{
    if (string_list_has_nul(config.argv)) {
        return std::nullopt;
    }

    QStringList env_entries = config.environment.toStringList();
    if (string_list_has_nul(env_entries)) {
        return std::nullopt;
    }

    Native_launch_data data;
    data.argv_bytes.reserve(static_cast<std::size_t>(config.argv.size()));
    for (const QString& argument : config.argv) {
        data.argv_bytes.push_back(QFile::encodeName(argument));
    }

    data.argv_ptrs.reserve(data.argv_bytes.size() + 1U);
    for (QByteArray& argument : data.argv_bytes) {
        data.argv_ptrs.push_back(argument.data());
    }
    data.argv_ptrs.push_back(nullptr);

    data.env_bytes.reserve(static_cast<std::size_t>(env_entries.size()));
    for (const QString& entry : env_entries) {
        data.env_bytes.push_back(entry.toLocal8Bit());
    }

    data.env_ptrs.reserve(data.env_bytes.size() + 1U);
    for (QByteArray& entry : data.env_bytes) {
        data.env_ptrs.push_back(entry.data());
    }
    data.env_ptrs.push_back(nullptr);

    data.working_directory = QFile::encodeName(config.working_directory);
    return data;
}

int set_fd_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return errno;
    }

    return 0;
}

int set_fd_cloexec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return errno;
    }

    return 0;
}

int create_pipe(Unique_fd& read_end, Unique_fd& write_end, bool nonblocking)
{
    int fds[2] = {-1, -1};
    if (::pipe(fds) < 0) {
        return errno;
    }

    read_end.reset(fds[0]);
    write_end.reset(fds[1]);

    int result = set_fd_cloexec(read_end.get());
    if (result != 0) {
        return result;
    }

    result = set_fd_cloexec(write_end.get());
    if (result != 0) {
        return result;
    }

    if (nonblocking) {
        result = set_fd_nonblocking(read_end.get());
        if (result != 0) {
            return result;
        }

        result = set_fd_nonblocking(write_end.get());
        if (result != 0) {
            return result;
        }
    }

    return 0;
}

bool pipe_read_exact(int fd, int& value)
{
    char*       out    = reinterpret_cast<char*>(&value);
    std::size_t offset = 0U;
    while (offset < sizeof(value)) {
        const ssize_t count = ::read(fd, out + offset, sizeof(value) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }

        if (count == 0) {
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

[[noreturn]] void pipe_write_errno_and_exit(int fd, int code)
{
    const char* bytes  = reinterpret_cast<const char*>(&code);
    std::size_t offset = 0U;
    while (offset < sizeof(code)) {
        const ssize_t count = ::write(fd, bytes + offset, sizeof(code) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    _exit(127);
}

[[noreturn]] void exec_child(
    const Native_launch_data&              data,
    int                                    startup_error_write)
{
    if (!data.working_directory.isEmpty() &&
        ::chdir(data.working_directory.constData()) < 0)
    {
        pipe_write_errno_and_exit(startup_error_write, errno);
    }

    ::execve(data.executable.constData(), data.argv_ptrs.data(), data.env_ptrs.data());
    pipe_write_errno_and_exit(startup_error_write, errno);
}

Terminal_exit_reason exit_reason_from_exit_info(
    const siginfo_t&                       exit_info,
    std::optional<Terminal_exit_reason>    override_reason)
{
    if (override_reason.has_value()) {
        return *override_reason;
    }

    if (exit_info.si_code == CLD_KILLED || exit_info.si_code == CLD_DUMPED) {
        if (exit_info.si_status == SIGINT) {
            return Terminal_exit_reason::INTERRUPTED;
        }

        return Terminal_exit_reason::TERMINATED;
    }

    return Terminal_exit_reason::EXITED;
}

int exit_code_from_exit_info(const siginfo_t& exit_info)
{
    if (exit_info.si_code == CLD_EXITED) {
        return exit_info.si_status;
    }

    if (exit_info.si_code == CLD_KILLED || exit_info.si_code == CLD_DUMPED) {
        return 128 + exit_info.si_status;
    }

    return 1;
}

int waitpid_nointr(pid_t pid, int* status, int options)
{
    for (;;) {
        const pid_t result = ::waitpid(pid, status, options);
        if (result >= 0 || errno != EINTR) {
            return static_cast<int>(result);
        }
    }
}

// waitid() reports the same termination as waitpid() but can leave the status
// waitable (WNOWAIT), which is what keeps the child's PID -- and therefore the
// number of the process group it leads -- reserved for this backend after it
// has exited. The siginfo_t is cleared first because a caller reads si_code and
// si_status even when the call reports an error.
int waitid_nointr(pid_t pid, siginfo_t& exit_info, int options)
{
    for (;;) {
        exit_info = siginfo_t{};
        const int result = ::waitid(P_PID, static_cast<id_t>(pid), &exit_info, options);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

// Reap a child that failed PTY setup before the session became live. The signal
// target depends on the child's lifecycle stage, which is why the two call sites
// differ: before exec is confirmed the child may not yet have run forkpty's
// login_tty -> setsid (or the explicit setpgid in the child branch), so its
// process group is not reliably its own pid -- signal the single child pid. Once
// exec is confirmed the child is its own session/process-group leader, so signal
// the whole group (-pid) to also reap any descendants it spawned.
void terminate_unstarted_child(pid_t child_pid, bool process_group_established)
{
    const pid_t signal_target = process_group_established ? -child_pid : child_pid;
    ::kill(signal_target, SIGKILL);
    waitpid_nointr(child_pid, nullptr, 0);
}

std::chrono::milliseconds bounded_sleep_interval(
    std::chrono::milliseconds remaining)
{
    constexpr std::chrono::milliseconds k_poll_interval(10);
    return std::min(k_poll_interval, remaining);
}

int poll_timeout_until(std::optional<std::chrono::steady_clock::time_point> deadline)
{
    if (!deadline.has_value()) {
        return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
        return 0;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(),
        std::numeric_limits<int>::max()));
}

void add_signal_target(Signal_targets& targets, pid_t process_group)
{
    if (process_group <= 0) {
        return;
    }

    for (std::size_t i = 0U; i < targets.count; ++i) {
        if (targets.process_groups[i] == process_group) {
            return;
        }
    }

    if (targets.count < targets.process_groups.size()) {
        targets.process_groups[targets.count++] = process_group;
    }
}

Signal_targets process_signal_targets(pid_t child_process_group, int master)
{
    Signal_targets targets;
    add_signal_target(targets, child_process_group);

    if (master >= 0) {
        const pid_t foreground_pgid = ::tcgetpgrp(master);
        if (foreground_pgid > 0) {
            add_signal_target(targets, foreground_pgid);
        }
    }

    return targets;
}

Signal_targets shutdown_signal_targets(
    pid_t  child_process_group,
    int    master,
    bool   root_exited,
    bool   reader_finished)
{
    if (root_exited && reader_finished) {
        return {};
    }

    return process_signal_targets(child_process_group, master);
}

#if defined(__APPLE__)
std::optional<bool> macos_process_group_has_unterminated_members(pid_t process_group);
#endif

std::optional<int> send_signal_to_targets(const Signal_targets& targets, int signal_number)
{
    const pid_t own_process_group = ::getpgrp();
    int first_error = 0;
    for (std::size_t i = 0U; i < targets.count; ++i) {
        const pid_t process_group = targets.process_groups[i];
        // Never signal our own process group: if the child's process group was
        // captured before the child established its own session/group (a forkpty
        // + setsid race), it could equal the parent's group, and kill(-pgid)
        // would deliver SIGKILL to this process too. Such a target is never a
        // descendant we need to reap.
        if (process_group <= 1 || process_group == own_process_group) {
            continue;
        }

        if (::kill(-process_group, signal_number) == 0) {
            continue;
        }

        const int signal_error = errno;
        if (signal_error == ESRCH) {
            continue;
        }

#if defined(__APPLE__)
        // A process can commit to exit between the pre-signal activity check
        // and kill(). macOS may then reject the group signal with EPERM even
        // though the group has no member that can still execute. Preserve a
        // genuine permission failure, but treat that completed lifecycle race
        // like ESRCH.
        if (signal_error == EPERM) {
            const std::optional<bool> active_members =
                macos_process_group_has_unterminated_members(process_group);
            if (active_members.has_value() && !*active_members) {
                continue;
            }
        }
#endif

        if (first_error == 0) {
            first_error = signal_error;
        }
    }

    if (first_error != 0) {
        return first_error;
    }

    return std::nullopt;
}

#if defined(__APPLE__)
std::optional<bool> macos_process_group_has_unterminated_members(pid_t process_group)
{
    int mib[4] = {
        CTL_KERN,
        KERN_PROC,
        KERN_PROC_PGRP,
        static_cast<int>(process_group),
    };

    constexpr int k_process_snapshot_attempts = 3;
    for (int attempt = 0; attempt < k_process_snapshot_attempts; ++attempt) {
        std::size_t byte_count = 0U;
        if (::sysctl(mib, 4U, nullptr, &byte_count, nullptr, 0U) < 0) {
            return std::nullopt;
        }
        if (byte_count == 0U) {
            return false;
        }

        const std::size_t process_capacity =
            (byte_count + sizeof(kinfo_proc) - 1U) / sizeof(kinfo_proc);
        std::vector<kinfo_proc> processes(process_capacity);
        byte_count = processes.size() * sizeof(kinfo_proc);
        if (::sysctl(mib, 4U, processes.data(), &byte_count, nullptr, 0U) == 0) {
            const std::size_t process_count = byte_count / sizeof(kinfo_proc);
            for (std::size_t i = 0U; i < process_count; ++i) {
                const extern_proc& process = processes[i].kp_proc;
                if (process.p_stat != SZOMB && (process.p_flag & P_WEXIT) == 0) {
                    return true;
                }
            }
            return false;
        }
        if (errno != ENOMEM) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}
#endif

}

class Posix_pty_backend::Impl
{
public:
    ~Impl()
    {
        shutdown();
    }

    Terminal_backend_result start(
        const Terminal_launch_config&  config,
        Terminal_backend_callbacks     callbacks)
    {
        Native_backend_start_gate start_gate{
            m_mutex,
            m_running,
            m_start_attempted,
            m_start_in_progress,
        };
        Native_backend_start_precheck precheck = validate_native_backend_start_preconditions(
            config,
            callbacks,
            start_gate,
            QStringLiteral("POSIX PTY"));
        if (is_backend_rejection(precheck.result)) {
            return precheck.result;
        }

        Terminal_effective_launch_config effective_config =
            std::move(*precheck.effective_config);

        const auto reject_start = [&] (
            Terminal_backend_error_code code,
            QString                     message,
            bool                        native_dispatch_occurred = false) {
            return reject_native_backend_start_attempt(
                callbacks,
                start_gate,
                code,
                std::move(message),
                native_dispatch_occurred);
        };

        if (!size_fits_pty(effective_config.initial_grid_size)) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                    QStringLiteral("initial terminal size is outside the PTY range"));
        }

        std::optional<Native_launch_data> native_launch =
            make_native_launch_data(effective_config);
        if (!native_launch.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                    QStringLiteral("launch argv/environment is invalid"));
        }

        const std::optional<QByteArray> executable = resolved_executable(
            native_launch->argv_bytes.front(),
            effective_config.environment,
            effective_config.working_directory);
        if (!executable.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    QStringLiteral("executable lookup failed in the final environment"));
        }
        native_launch->executable = *executable;

        Unique_fd startup_error_read;
        Unique_fd startup_error_write;
        int pipe_result = create_pipe(startup_error_read, startup_error_write, false);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe startup error"), pipe_result));
        }

        Unique_fd read_wake_read;
        Unique_fd read_wake_write;
        pipe_result = create_pipe(read_wake_read, read_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend read wake"), pipe_result));
        }

        Unique_fd write_wake_read;
        Unique_fd write_wake_write;
        pipe_result = create_pipe(write_wake_read, write_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend write wake"), pipe_result));
        }

        winsize initial_winsize =
            winsize_from_grid_size(effective_config.initial_grid_size);
        int master_fd = -1;
        const pid_t child_pid =
            ::forkpty(&master_fd, nullptr, nullptr, &initial_winsize);
        if (child_pid < 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("forkpty"), errno));
        }

        if (child_pid == 0) {
            startup_error_read.reset();
            // POSIX session/process-group setup: the child always becomes its own
            // session/process-group leader. forkpty's login_tty already calls
            // setsid(), which creates a new session and a new process group with the
            // child as leader. This setpgid(0, 0) is therefore best-effort and
            // non-essential: it is a defensive no-op-or-reassert of the group setsid
            // just established, kept only to make signal targeting (e.g.
            // kill(-pgid, SIGINT) for the foreground group) obviously correct to a
            // reader. The result is intentionally unchecked and is harmless if it
            // fails, since the group already exists and the child is about to exec.
            (void)::setpgid(0, 0);
            exec_child(*native_launch, startup_error_write.get());
        }

        startup_error_write.reset();
        Unique_fd master(master_fd);

        const int cloexec_result = set_fd_cloexec(master.get());
        if (cloexec_result != 0) {
            // Exec not yet confirmed: the child's own process group may not exist.
            terminate_unstarted_child(child_pid, /*process_group_established=*/false);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("fcntl PTY close-on-exec"), cloexec_result),
                    true);
        }

        int child_errno = 0;
        if (pipe_read_exact(startup_error_read.get(), child_errno)) {
            waitpid_nointr(child_pid, nullptr, 0);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("execve"), child_errno),
                    true);
        }
        startup_error_read.reset();

        const int nonblocking_result = set_fd_nonblocking(master.get());
        if (nonblocking_result != 0) {
            // Exec confirmed (startup-error pipe closed): the child is its own
            // process-group leader, so reap the whole group.
            terminate_unstarted_child(child_pid, /*process_group_established=*/true);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("fcntl PTY nonblocking"), nonblocking_result),
                    true);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_callbacks                          = std::move(callbacks);
            m_master                             = std::move(master);
            m_read_wake_read                     = std::move(read_wake_read);
            m_read_wake_write                    = std::move(read_wake_write);
            m_write_wake_read                    = std::move(write_wake_read);
            m_write_wake_write                   = std::move(write_wake_write);
            m_child_pid                          = child_pid;
            // The child makes itself a session/process-group leader (forkpty's
            // login_tty -> setsid, plus the explicit setpgid(0, 0) above), so
            // its process group is its own pid. Use that rather than a racy
            // getpgid(child_pid) read, which can observe the parent's group
            // before the child has run setsid.
            m_child_process_group                = child_pid;
            m_running                            = true;
            m_start_attempted                    = true;
            m_start_in_progress                  = false;
            m_stopping                           = false;
            m_process_stopping                   = false;
            m_output_paused                      = false;
            m_exit_reported                      = false;
            m_reader_finished                    = false;
            m_writer_failed                      = false;
            m_startup_aborted                    = false;
            m_root_exited                        = false;
            m_root_consumed                      = false;
            m_paused_output_delivery_in_progress = false;
            m_termination_policy                 = effective_config.termination_policy;
            m_paused_output_limits =
                derive_native_backend_output_delivery_limits(
                    effective_config.output_delivery_limits);
            m_exit_reason_override.reset();
            m_queued_write_bytes = 0U;
            m_write_queue.clear();
        }

        Terminal_backend_result worker_result = start_native_backend_workers(
            call_state(),
            m_reader_thread,
            m_writer_thread,
            m_wait_thread,
            [this] {
                read_loop();
            },
            [this] {
                write_loop();
            },
            [this] {
                wait_loop();
            },
            [this](const std::exception& error) {
                const QString message =
                    QStringLiteral("POSIX PTY worker thread startup failed: %1")
                        .arg(QString::fromLocal8Bit(error.what()));
                report_native_backend_error_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    Terminal_backend_error_code::START_FAILED,
                    message);
                shutdown();
                return backend_reject(Terminal_backend_error_code::START_FAILED, message);
            });
        worker_result.native_dispatch_occurred = true;
        return worker_result;
    }

    Terminal_backend_result write(QByteArray bytes)
    {
        if (bytes.isEmpty()) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY write requires bytes"));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_process_stopping || m_stopping || m_writer_failed || !m_master) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY backend is not writable"));
        }

        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        if (!native_backend_write_queue_can_accept(m_queued_write_bytes, byte_count)) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY write queue limit reached"));
        }

        add_native_backend_queued_write_bytes(m_queued_write_bytes, byte_count);
        m_write_queue.push_back({std::move(bytes)});
        m_write_cv.notify_one();
        return backend_accept();
    }

    Terminal_backend_result resize(Terminal_backend_resize_request request)
    {
        if (!size_fits_pty(request.grid_size)) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("POSIX PTY resize requires a positive unsigned-short grid"));
        }

        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::RESIZE_FAILED,
                        QStringLiteral("POSIX PTY resize requires a running process"));
            }

            master = m_master.get();
        }

        winsize size = winsize_from_grid_size(request.grid_size);
        if (::ioctl(master, TIOCSWINSZ, &size) < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    posix_error_message(QStringLiteral("TIOCSWINSZ"), errno));
        }

        return backend_accept();
    }

    Terminal_backend_result set_output_paused(bool paused)
    {
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        bool should_wake_reader = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_process_stopping || m_stopping) {
                m_output_paused = false;
                should_wake_reader = true;
            }
            else {
                m_output_paused = paused;
                should_wake_reader = !paused;
            }

            if (!m_output_paused &&
                !m_paused_output_delivery_in_progress &&
                !m_paused_output.isEmpty())
            {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }

        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);

        if (should_wake_reader && !paused_output_delivery_started) {
            m_output_cv.notify_all();
            wake_io_threads();
        }

        return backend_accept();
    }

    Terminal_backend_result interrupt()
    {
        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("POSIX PTY interrupt requires a running process"));
            }

            master = m_master.get();
        }

        const pid_t foreground_pgid = ::tcgetpgrp(master);
        if (foreground_pgid < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("tcgetpgrp"), errno));
        }
        if (foreground_pgid == 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("POSIX PTY has no foreground process group"));
        }

        int interrupt_error = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("POSIX PTY interrupt requires a running process"));
            }

            if (::kill(-foreground_pgid, SIGINT) < 0) {
                interrupt_error = errno;
            }
        }

        if (interrupt_error != 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("SIGINT"), interrupt_error));
        }

        return backend_accept();
    }

    Terminal_backend_result terminate()
    {
        pid_t child_process_group = -1;
        int master                = -1;
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        Terminal_termination_policy policy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        QStringLiteral("POSIX PTY terminate requires a running process"));
            }

            child_process_group = m_child_process_group;
            master              = m_master.get();
            policy              = m_termination_policy;

            m_process_stopping     = true;
            m_output_paused        = false;
            m_exit_reason_override = Terminal_exit_reason::TERMINATED;
            if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
        }

        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        return start_termination_escalation(
            process_signal_targets(child_process_group, master),
            policy);
    }

    Terminal_backend_result start_termination_escalation(
        Signal_targets                 targets,
        Terminal_termination_policy    policy)
    {
        return start_native_backend_termination_escalation(
            call_state(),
            m_termination_thread,
            [this, targets, policy] {
                termination_escalation_loop(targets, policy);
            },
            [this, targets](const std::exception& error) {
                const QString message =
                    QStringLiteral("POSIX PTY termination escalation worker failed: %1")
                        .arg(QString::fromLocal8Bit(error.what()));
                const std::optional<int> kill_error = signal_owned_targets(targets, SIGKILL);
                if (kill_error.has_value()) {
                    return
                        backend_reject(
                            Terminal_backend_error_code::TERMINATE_FAILED,
                            posix_error_message(QStringLiteral("SIGKILL"), *kill_error));
                }

                return backend_reject(Terminal_backend_error_code::TERMINATE_FAILED, message);
            });
    }

    void shutdown()
    {
        pid_t child_process_group = -1;
        int master                = -1;
        Signal_targets targets;
        bool root_exited          = false;
        bool reader_finished      = false;
        bool close_master         = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            const bool defer_master_close        = m_post_exit_process_group_cleanup_pending;
            m_shutdown_started                   = true;
            m_stopping                           = true;
            m_process_stopping                   = true;
            m_output_paused                      = false;
            m_paused_output_delivery_in_progress = false;
            m_callbacks                          = {};
            child_process_group                  = m_child_process_group;
            root_exited                          = m_root_exited;
            reader_finished                      = m_reader_finished;
            if (m_master) {
                master = m_master.get();
                targets = shutdown_signal_targets(
                    child_process_group,
                    master,
                    root_exited,
                    reader_finished);
                if (defer_master_close) {
                    m_close_master_pending = true;
                }
                else {
                    master = m_master.release();
                    close_master = true;
                }
            }
            else {
                targets = shutdown_signal_targets(
                    child_process_group,
                    master,
                    root_exited,
                    reader_finished);
            }
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        (void)signal_owned_targets(targets, SIGKILL);

        // The signal above targets process groups (kill(-pgid)). During the
        // brief window after forkpty() but before the child has established its
        // own session/process group, kill(-pgid) fails with ESRCH -- which
        // send_signal_to_targets() ignores -- so the still-running child is not
        // killed. The wait thread is then stuck forever in its blocking wait on
        // the child and join_native_backend_threads() below deadlocks, hanging
        // teardown. (The race is timing-dependent and was observed on macOS.)
        // Signal the child directly, which always reaches it, so the wait
        // returns and the wait thread can be joined. Reaching it by PID is
        // exact only while the status is unconsumed, which signal_owned_root()
        // decides under the mutex the consuming wait also takes; a snapshot
        // taken above would already be stale by now.
        signal_owned_root(SIGKILL);

        // In ordinary shutdown, close the master before
        // join_native_backend_threads() so a child blocked writing to the slave
        // is released. If the wait thread still needs the master for post-exit
        // cleanup, that path owns the pending close instead.
        if (close_master) {
            ::close(master);
        }

        join_native_backend_threads(
            m_reader_thread,
            m_writer_thread,
            m_wait_thread,
            m_termination_thread);
        consume_root_status();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_master.reset();
        m_read_wake_read.reset();
        m_read_wake_write.reset();
        m_write_wake_read.reset();
        m_write_wake_write.reset();
        m_write_queue.clear();
        m_queued_write_bytes  = 0U;
        m_child_process_group = -1;
        m_running             = false;
    }

    native_backend_call_state_t call_state()
    {
        return native_backend_call_state_t{
            m_mutex,
            m_public_call_cv,
            m_public_call_depth,
            m_worker_thread_ids,
            m_startup_aborted,
        };
    }

    // The on-last-call claim for the shared public-call guard: claims the master
    // descriptor whose close shutdown() deferred while a public call was in flight,
    // and returns the close to run after the backend mutex is released.
    auto deferred_master_close_claim()
    {
        return [this] {
            const int master_to_close = take_pending_master_close_if_ready_locked();
            return [master_to_close] {
                if (master_to_close >= 0) {
                    ::close(master_to_close);
                }
            };
        };
    }

    void request_shutdown_without_cleanup()
    {
        pid_t child_process_group = -1;
        int master                = -1;
        Signal_targets targets;
        bool root_exited          = false;
        bool reader_finished      = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            m_shutdown_started                   = true;
            m_stopping                           = true;
            m_process_stopping                   = true;
            m_output_paused                      = false;
            m_paused_output_delivery_in_progress = false;
            m_callbacks                          = {};
            child_process_group                  = m_child_process_group;
            root_exited                          = m_root_exited;
            reader_finished                      = m_reader_finished;
            if (m_master) {
                master = m_master.get();
            }
            targets = shutdown_signal_targets(
                child_process_group,
                master,
                root_exited,
                reader_finished);
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        (void)signal_owned_targets(targets, SIGKILL);
        signal_owned_root(SIGKILL);
    }

private:
    int take_pending_master_close_if_ready_locked()
    {
        if (!m_close_master_pending ||
            m_public_call_depth != 0U ||
            m_post_exit_process_group_cleanup_pending)
        {
            return -1;
        }

        m_close_master_pending = false;
        return m_master.release();
    }

    native_backend_exit_publication_state_t exit_publication_state()
    {
        return native_backend_exit_publication_state_t{
            m_mutex,
            m_output_cv,
            m_write_cv,
            m_callbacks,
            m_exit_reported,
            m_running,
            m_stopping,
            m_output_paused,
        };
    }

    void report_exit_once(const siginfo_t& exit_info)
    {
        std::optional<Terminal_exit_reason> override_reason;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            override_reason = m_exit_reason_override;
        }

        report_exit_once(
            exit_reason_from_exit_info(exit_info, override_reason),
            exit_code_from_exit_info(exit_info));
    }

    void report_exit_once(Terminal_exit_reason reason, int exit_code)
    {
        report_native_backend_exit_once(
            exit_publication_state(),
            reason,
            exit_code,
            [](Terminal_exit_reason resolved_reason) {
                // This backend pre-resolves the reported reason from the wait status
                // before the once-section (see the wait-status overload above), so the
                // shared skeleton takes it as given. The Windows backend resolves its
                // reason inside the once-section instead. The two placements are not
                // interchangeable and neither is shared.
                return resolved_reason;
            },
            [this] {
                // Claims this backend's extra stop flag under the once-lock, after the
                // shared stop flags are set; the returned action wakes the I/O threads
                // after the output and write waiters are woken. Windows has neither
                // and passes the empty claim.
                m_process_stopping = true;
                return [this] {
                    wake_io_threads();
                };
            });
    }

    bool take_paused_output_for_delivery_locked(QByteArray& paused_output)
    {
        return
            take_native_backend_paused_output_for_delivery_locked(
                m_paused_output,
                paused_output,
                m_paused_output_delivery_in_progress,
                m_paused_output_limits.delivery_chunk_bytes);
    }

    void finish_paused_output_delivery(bool delivery_started)
    {
        finish_native_backend_paused_output_delivery(
            m_mutex,
            m_output_cv,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            m_paused_output_limits.delivery_chunk_bytes,
            delivery_started,
            [this] {
                // This backend drains through m_process_stopping || m_stopping and
                // wakes its I/O threads when the drain finishes. The Windows backend
                // drains through m_stopping || m_shutdown_started instead and has no
                // I/O threads to wake. The two stop predicates are not
                // interchangeable and neither is the shared default.
                return m_process_stopping || m_stopping;
            },
            [this](QByteArray bytes) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(bytes));
            },
            [this] {
                wake_io_threads();
            });
    }

    void drain_paused_output_before_exit_report()
    {
        drain_native_backend_paused_output_before_exit_report(
            m_mutex,
            m_output_cv,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            m_paused_output_limits.delivery_chunk_bytes,
            [this] {
                return m_process_stopping || m_stopping;
            },
            [this](QByteArray bytes) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(bytes));
            },
            [this] {
                wake_io_threads();
            });
    }

    void deliver_or_buffer_output(QByteArray bytes)
    {
        const qsizetype bytes_size = bytes.size();
        deliver_or_buffer_native_backend_output(
            m_mutex,
            m_output_cv,
            m_callbacks,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            std::move(bytes),
            [this, bytes_size] {
                return
                    m_paused_output_limits.delivery_chunk_bytes > 0 &&
                    m_paused_output.size() <=
                        m_paused_output_limits.high_watermark_bytes &&
                    bytes_size <=
                        m_paused_output_limits.high_watermark_bytes -
                            m_paused_output.size();
            },
            [this] {
                return m_stopping;
            });
    }

    // Block until the exit-drain window (exit_time + k_exit_output_drain_timeout)
    // elapses, returning early if shutdown begins. Uses m_output_cv, which
    // shutdown notifies after setting m_stopping, so teardown is never delayed.
    void wait_out_exit_drain_window(std::chrono::steady_clock::time_point exit_time)
    {
        const auto deadline = exit_time + k_exit_output_drain_timeout;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_output_cv.wait_until(lock, deadline, [&] {
            return m_stopping;
        });
    }

    // Consume the root child's exit status, which is what finally releases its
    // PID and the number of the process group it leads. Both coordinates are
    // retired under the mutex before the wait runs, so a signalling path that
    // wins the mutex first either finds them owned -- and the kernel still
    // cannot have given them away -- or finds them retired and sends nothing.
    void consume_root_status()
    {
        pid_t child_pid = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_root_consumed || m_child_pid <= 0) {
                return;
            }

            m_root_consumed       = true;
            child_pid             = m_child_pid;
            m_child_pid           = -1;
            m_child_process_group = -1;
        }

        // A failing wait leaves the status unconsumed, but the coordinates stay
        // retired and are never signalled again: a backend that has stopped
        // being able to wait for its child can no longer tell it from a later
        // holder of the same number.
        (void)waitpid_nointr(child_pid, nullptr, 0);
    }

    // A signal that names a PID or a process group by number reaches exactly
    // this terminal's family only while the root child's status is unconsumed:
    // the root leads the group, so the kernel keeps both its PID and the group
    // number reserved until that status is taken. Every such signal, and every
    // liveness probe that names the same numbers, therefore asks here, under
    // the mutex that consume_root_status() takes before it waits.
    std::optional<int> signal_owned_targets(
        const Signal_targets&  targets,
        int                    signal_number)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_root_consumed) {
            return std::nullopt;
        }

        return send_signal_to_targets(targets, signal_number);
    }

    void signal_owned_root(int signal_number)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_root_consumed || m_child_pid <= 0) {
            return;
        }

        ::kill(m_child_pid, signal_number);
    }

    void signal_owned_child_process_group(int signal_number)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_root_consumed) {
            return;
        }

        (void)send_signal_to_targets(
            process_signal_targets(m_child_process_group, m_master.get()),
            signal_number);
    }

    bool signal_target_active(pid_t process_group)
    {
        if (process_group <= 0) {
            return false;
        }

#if defined(__APPLE__)
        const std::optional<bool> active_members =
            macos_process_group_has_unterminated_members(process_group);
        if (active_members.has_value()) {
            return *active_members;
        }
#endif

        if (::kill(-process_group, 0) == 0) {
            return true;
        }

        return errno == EPERM;
    }

    bool signal_targets_active(const Signal_targets& targets)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_root_consumed) {
            // The numbers these targets hold have been released, so only a
            // stranger that reused one could answer the probe. Nothing this
            // backend is entitled to reach is still reachable through them.
            return false;
        }

        for (std::size_t i = 0U; i < targets.count; ++i) {
            if (signal_target_active(targets.process_groups[i])) {
                return true;
            }
        }

        return false;
    }

    bool wait_for_signal_targets_exit_observed(
        const Signal_targets&      targets,
        std::chrono::milliseconds  interval)
    {
        const auto deadline = std::chrono::steady_clock::now() + interval;
        do {
            if (!signal_targets_active(targets)) {
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }

            std::this_thread::sleep_for(bounded_sleep_interval(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        while (true);
    }

    void wake_io_threads()
    {
        int read_wake_write = -1;
        int write_wake_write = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            read_wake_write = m_read_wake_write.get();
            write_wake_write = m_write_wake_write.get();
        }

        const unsigned char byte = 1U;
        if (read_wake_write >= 0) {
            const ssize_t result = ::write(read_wake_write, &byte, sizeof(byte));
            (void)result;
        }

        if (write_wake_write >= 0) {
            const ssize_t result = ::write(write_wake_write, &byte, sizeof(byte));
            (void)result;
        }
    }

    void drain_wake_pipe(int wake_read)
    {
        unsigned char buffer[64];
        for (;;) {
            const ssize_t count = ::read(wake_read, buffer, sizeof(buffer));
            if (count > 0) {
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            break;
        }
    }

    bool write_stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping || m_process_stopping;
    }

    void read_loop()
    {
        std::vector<char> buffer(k_native_backend_output_read_chunk_bytes);
        std::optional<std::chrono::steady_clock::time_point> final_drain_deadline;

        for (;;) {
            int         master        = -1;
            int         wake_read     = -1;
            std::size_t bytes_to_read = buffer.size();
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // Keep draining the master while output is paused (buffering
                // app-side) until the paused buffer reaches the high-water mark;
                // only then park for backpressure. This stops a child that emits
                // a final line and exits while paused from blocking forever in
                // write() on macOS. The paused high-water mark is derived from
                // the owning session's callback queue hard/high-water limits.
                const auto reader_can_proceed = [&] {
                    const bool direct_delivery_available =
                        !m_output_paused && m_paused_output.isEmpty();
                    const bool paused_buffer_room_available =
                        m_paused_output.size() <
                            m_paused_output_limits.high_watermark_bytes;
                    return
                        m_stopping ||
                        (!m_paused_output_delivery_in_progress &&
                         (direct_delivery_available || paused_buffer_room_available));
                };
                m_output_cv.wait(lock, reader_can_proceed);

                if (m_stopping) {
                    break;
                }

                if (m_root_exited && !final_drain_deadline.has_value()) {
                    final_drain_deadline =
                        std::chrono::steady_clock::now() + k_exit_output_drain_timeout;
                }

                master = m_master.get();
                wake_read = m_read_wake_read.get();
                bytes_to_read =
                    static_cast<std::size_t>(m_paused_output_limits.read_chunk_bytes);
                if ((m_output_paused || !m_paused_output.isEmpty()) &&
                    m_paused_output.size() <
                        m_paused_output_limits.high_watermark_bytes)
                {
                    const qsizetype paused_output_room =
                        m_paused_output_limits.high_watermark_bytes -
                            m_paused_output.size();
                    bytes_to_read = static_cast<std::size_t>(std::min<qsizetype>(
                        static_cast<qsizetype>(bytes_to_read),
                        paused_output_room));
                }
            }

            pollfd fds[2] = {
                { master, POLLIN | POLLHUP | POLLERR, 0 },
                { wake_read, POLLIN, 0 },
            };

            if (final_drain_deadline.has_value() &&
                std::chrono::steady_clock::now() >= *final_drain_deadline)
            {
                break;
            }

            // Bound the wait: when a drain deadline is armed it caps the poll;
            // otherwise fall back to the heartbeat so the reader re-checks its
            // stop flags rather than trusting the wake pipe to interrupt a poll
            // on the PTY master (unreliable on macOS). A heartbeat tick with no
            // deadline just means no master/wake activity yet -- keep waiting.
            const int poll_timeout = final_drain_deadline.has_value()
                ? poll_timeout_until(final_drain_deadline)
                : static_cast<int>(k_native_master_poll_interval.count());
            const int poll_result = ::poll(fds, 2, poll_timeout);
            if (poll_result == 0) {
                if (!final_drain_deadline.has_value()) {
                    continue;
                }
                break;
            }

            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (!is_native_backend_stopping(m_mutex, m_stopping)) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("poll PTY output"), errno));
                }
                break;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                drain_wake_pipe(wake_read);
                if (is_native_backend_stopping(m_mutex, m_stopping)) {
                    break;
                }
            }

            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }

            const ssize_t bytes_read = ::read(master, buffer.data(), bytes_to_read);
            if (bytes_read < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                if (errno != EIO && !is_native_backend_stopping(m_mutex, m_stopping)) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("PTY output read"), errno));
                }
                break;
            }

            if (bytes_read == 0) {
                break;
            }

            deliver_or_buffer_output(QByteArray(
                buffer.data(),
                static_cast<qsizetype>(bytes_read)));
        }

        mark_native_backend_reader_finished(
            m_mutex,
            m_reader_cv,
            m_reader_finished);
    }

    void write_loop()
    {
        for (;;) {
            Queued_write write;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_write_cv.wait(lock, [&] {
                    return m_stopping || m_process_stopping || !m_write_queue.empty();
                });

                if (m_stopping || m_process_stopping) {
                    return;
                }

                write = std::move(m_write_queue.front());
                m_write_queue.pop_front();
                remove_native_backend_queued_write_bytes(
                    m_queued_write_bytes,
                    static_cast<std::size_t>(write.bytes.size()));
            }

            if (!write_all(write.bytes)) {
                mark_writer_failed();
                return;
            }
        }
    }

    void mark_writer_failed()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping && !m_process_stopping) {
            m_writer_failed = true;
        }
    }

    // Wait, bounded by a heartbeat, before retrying a write that returned
    // EAGAIN. This deliberately polls ONLY the wake pipe (an ordinary pipe) and
    // never the PTY master: poll() on a PTY master is unreliable on macOS -- a
    // POLLOUT poll there could be wedged neither by the wake-pipe write nor by a
    // finite timeout once the child has exited while a descendant still holds
    // the slave open, deadlocking the writer thread and teardown. The retried
    // non-blocking write() is itself the authoritative writability test, so we
    // only need a bounded, reliably-interruptible wait here. Returns false when
    // the writer should stop.
    bool wait_for_write_capacity(int wake_read)
    {
        pollfd fds[1] = {
            { wake_read, POLLIN, 0 },
        };

        const int poll_result = ::poll(
            fds,
            1,
            static_cast<int>(k_native_master_poll_interval.count()));
        if (poll_result < 0) {
            if (errno == EINTR) {
                return !write_stopping();
            }

            if (!write_stopping()) {
                report_native_backend_error_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    Terminal_backend_error_code::WRITE_FAILED,
                    posix_error_message(QStringLiteral("poll PTY write wake"), errno));
            }
            return false;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            drain_wake_pipe(wake_read);
        }

        return !write_stopping();
    }

    bool write_all(const QByteArray& bytes)
    {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            int master = -1;
            int wake_read = -1;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || m_process_stopping || !m_master) {
                    return false;
                }

                master = m_master.get();
                wake_read = m_write_wake_read.get();
            }

            // Write first: the PTY master is non-blocking, so write() is itself
            // the writability test. Only wait when it reports EAGAIN, and wait
            // on the wake pipe rather than polling the master for POLLOUT (see
            // wait_for_write_capacity). This keeps the writer off the unreliable
            // macOS PTY-master POLLOUT path entirely.
            const qsizetype remaining = bytes.size() - offset;
            const ssize_t count = ::write(
                master,
                bytes.constData() + offset,
                static_cast<std::size_t>(remaining));
            if (count > 0) {
                offset += static_cast<qsizetype>(count);
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!wait_for_write_capacity(wake_read)) {
                    return false;
                }
                continue;
            }

            if (count < 0) {
                if (errno != EIO && !write_stopping()) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::WRITE_FAILED,
                        posix_error_message(QStringLiteral("PTY input write"), errno));
                }
                return false;
            }

            // count == 0: the write made no progress.
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("PTY input write made no progress"));
            return false;
        }

        return true;
    }

    void wait_loop()
    {
        pid_t child_pid = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            child_pid = m_child_pid;
        }

        if (child_pid <= 0) {
            return;
        }

        // Observe the exit without consuming it. The status stays waitable, so
        // the root's PID -- and the number of the process group it leads -- stay
        // reserved for this backend until the descendant cleanup below has
        // finished naming them. consume_root_status() at the end of this
        // function is what releases them.
        siginfo_t exit_info{};
        const int observed = waitid_nointr(child_pid, exit_info, WEXITED | WNOWAIT);
        const int observe_errno = errno;
        const auto exit_time = std::chrono::steady_clock::now();
        if (observed != 0) {
            const int wait_error = observe_errno;
            QByteArray paused_output;
            bool paused_output_delivery_started = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // ECHILD means the status is already gone: something outside
                // this backend consumed it, so both coordinates are retired
                // and nothing may name them again.
                m_root_exited   = wait_error == ECHILD;
                m_root_consumed = m_root_exited;
                if (m_root_consumed) {
                    m_child_pid           = -1;
                    m_child_process_group = -1;
                }
                m_process_stopping = true;
                m_output_paused = false;
                m_write_queue.clear();
                m_queued_write_bytes = 0U;
                if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                    paused_output_delivery_started =
                        take_paused_output_for_delivery_locked(paused_output);
                }
            }
            if (!paused_output.isEmpty()) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(paused_output));
            }
            finish_paused_output_delivery(paused_output_delivery_started);
            m_output_cv.notify_all();
            m_write_cv.notify_all();
            wake_io_threads();
            wait_for_native_backend_reader_finished(
                m_mutex,
                m_reader_cv,
                m_reader_finished);
            drain_paused_output_before_exit_report();
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("waitpid"), wait_error));
            report_exit_once(
                Terminal_exit_reason::TERMINATED,
                k_waitpid_failure_exit_code);
            return;
        }

        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_root_exited      = true;
            m_process_stopping = true;
            m_output_paused    = false;
            m_post_exit_process_group_cleanup_pending = true;
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
            if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }
        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        wait_for_native_backend_reader_finished(
            m_mutex,
            m_reader_cv,
            m_reader_finished);
        // Honor a consistent exit-drain window before finalizing the exit, even
        // when the reader finished early. On Linux the master stays open while a
        // descendant holds the slave, so the reader drains for the full
        // k_exit_output_drain_timeout; on macOS the master EOFs the instant the
        // session-leader child exits, so the reader finishes immediately. Without
        // this wait the exit would be reported and leftover descendants reaped
        // within microseconds on macOS -- before a consumer could observe the
        // descendant alive -- diverging from Linux. Wait out the remainder of the
        // drain window (interruptibly, so shutdown is not delayed).
        wait_out_exit_drain_window(exit_time);
        drain_paused_output_before_exit_report();
        // Report the child's exit BEFORE reaping leftover descendants. The
        // descendant must remain observable as alive until the backend reports
        // the direct child's exit (the contract a consumer relies on); the
        // process-absent check that follows exit reporting tolerates the kill
        // landing slightly later. Killing first would race the descendant dead
        // before the exit is reported -- which on macOS (immediate EOF, no drain
        // wait) happens fast enough to be observed.
        report_exit_once(exit_info);
        kill_child_process_group_before_consuming_root();
        consume_root_status();
    }

    void kill_child_process_group_before_consuming_root()
    {
        // Kill any leftover members of the child's process group (e.g. a
        // backgrounded descendant still holding the PTY slave open) once the
        // direct child has exited and the exit drain has finished. This must NOT
        // be gated on the reader having timed out: that gate was a proxy for
        // "a descendant kept the slave open so the master never EOFed", which
        // holds on Linux but not on macOS, where the master EOFs as soon as the
        // session-leader child exits even with a descendant alive. So the reader
        // finishes via EOF (timed_out == false) and the descendant would leak.
        // kill(-pgid) is best effort and ignores ESRCH, so a clean exit with no
        // descendants is a harmless no-op.
        //
        // The group is named by the exited root's PID, and the caller has not
        // consumed that status yet, so the number still belongs to this
        // terminal's family and cannot have been handed to an unrelated group.
        // Consuming it first and signalling afterwards would name whatever had
        // since taken the number.
        signal_owned_child_process_group(SIGKILL);

        int master_to_close = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_post_exit_process_group_cleanup_pending = false;
            master_to_close = take_pending_master_close_if_ready_locked();
        }

        if (master_to_close >= 0) {
            ::close(master_to_close);
        }
    }

    void termination_escalation_loop(
        Signal_targets                 targets,
        Terminal_termination_policy    policy)
    {
        if (!signal_targets_active(targets)) {
            return;
        }

        const std::optional<int> term_error = signal_owned_targets(targets, SIGTERM);
        if (term_error.has_value()) {
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("SIGTERM"), *term_error));
            return;
        }

        if (wait_for_signal_targets_exit_observed(targets, policy.graceful_interval)) {
            return;
        }

        const std::optional<int> kill_error = signal_owned_targets(targets, SIGKILL);
        if (kill_error.has_value()) {
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("SIGKILL"), *kill_error));
            return;
        }

        if (wait_for_signal_targets_exit_observed(targets, policy.kill_interval)) {
            return;
        }

        report_native_backend_error_with_snapshot(
            m_mutex,
            m_callbacks,
            Terminal_backend_error_code::TERMINATE_FAILED,
            QStringLiteral("POSIX PTY process remained active after forced termination"));
    }

    std::mutex                          m_mutex;
    std::condition_variable             m_output_cv;
    std::condition_variable             m_write_cv;
    std::condition_variable             m_reader_cv;
    std::condition_variable             m_public_call_cv;
    Terminal_backend_callbacks          m_callbacks;
    Unique_fd                           m_master;
    Unique_fd                           m_read_wake_read;
    Unique_fd                           m_read_wake_write;
    Unique_fd                           m_write_wake_read;
    Unique_fd                           m_write_wake_write;
    std::thread                         m_reader_thread;
    std::thread                         m_writer_thread;
    std::thread                         m_wait_thread;
    std::thread                         m_termination_thread;
    std::set<std::thread::id>           m_worker_thread_ids;
    QByteArray                          m_paused_output;
    std::deque<Queued_write>            m_write_queue;
    std::optional<Terminal_exit_reason> m_exit_reason_override;
    Terminal_termination_policy         m_termination_policy;
    native_backend_output_delivery_limits_t
                                        m_paused_output_limits =
                                            derive_native_backend_output_delivery_limits(
                                                std::nullopt);
    std::size_t                         m_queued_write_bytes = 0U;
    std::size_t                         m_public_call_depth = 0U;
    pid_t                               m_child_pid = -1;
    pid_t                               m_child_process_group = -1;
    bool                                m_running = false;
    bool                                m_start_attempted = false;
    bool                                m_start_in_progress = false;
    bool                                m_startup_aborted = false;
    bool                                m_process_stopping = false;
    bool                                m_stopping = false;
    bool                                m_output_paused = false;
    bool                                m_paused_output_delivery_in_progress = false;
    bool                                m_exit_reported = false;
    bool                                m_shutdown_started = false;
    bool                                m_close_master_pending = false;
    bool                                m_post_exit_process_group_cleanup_pending = false;
    bool                                m_reader_finished = false;
    bool                                m_writer_failed = false;
    // The root child's termination has been observed; its status may or may not
    // have been taken yet. Only m_root_consumed retires the PID and process-group
    // numbers this backend signals by.
    bool                                m_root_exited = false;
    bool                                m_root_consumed = false;
};

Posix_pty_backend::Posix_pty_backend()
:
    m_impl(std::make_unique<Impl>())
{}

Posix_pty_backend::~Posix_pty_backend()
{
    // Defer when teardown is reached from a callback or while a public backend
    // call is still unwinding, so the Impl is not freed under live stack frames.
    if (m_impl && must_defer_native_backend_destruction(m_impl->call_state())) {
        Impl* impl = m_impl.release();
        defer_native_backend_shutdown_and_delete(
            impl,
            impl->call_state(),
            [impl] { impl->request_shutdown_without_cleanup(); });
    }
}

Terminal_backend_result Posix_pty_backend::start(
    const Terminal_launch_config&  config,
    Terminal_backend_callbacks     callbacks)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->start(config, std::move(callbacks));
}

Terminal_backend_result Posix_pty_backend::write(QByteArray bytes)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->write(std::move(bytes));
}

Terminal_backend_result Posix_pty_backend::resize(
    Terminal_backend_resize_request request)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->resize(request);
}

Terminal_backend_result Posix_pty_backend::set_output_paused(bool paused)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->set_output_paused(paused);
}

Terminal_backend_result Posix_pty_backend::interrupt()
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->interrupt();
}

Terminal_backend_result Posix_pty_backend::terminate()
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state(),
        impl->deferred_master_close_claim());
    return impl->terminate();
}

std::unique_ptr<Terminal_backend> make_posix_pty_backend()
{
    return std::make_unique<Posix_pty_backend>();
}

}

#endif
