#include <vnm_process_custody/process_spawn.h>
#include "process_spawn_common_posix.h"

#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace vnm::process_custody {

namespace {

std::uint64_t process_birth_token(pid_t pid)
{
    struct proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) || info.pbi_pid != pid) {
        return 0;
    }
    return info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec;
}

enum class Observation { EXIT, EXIT_STATUS, EXECUTION };

Native_handle open_occurrence_observer(std::int64_t process_id, std::uint64_t birth_token,
    Observation observation, std::string* error)
{
    if (process_id <= 0 || process_id > std::numeric_limits<pid_t>::max() || birth_token == 0 ||
        process_birth_token((pid_t)process_id) != birth_token)
    {
        if (error) {
            *error = "original process occurrence is no longer observable";
        }
        return k_invalid_handle;
    }
    const int observer = kqueue();
    struct kevent event;
    EV_SET(&event, (pid_t)process_id, EVFILT_PROC,
        EV_ADD | EV_ENABLE | (observation == Observation::EXIT_STATUS ? EV_CLEAR : 0),
        NOTE_EXIT | (observation == Observation::EXIT_STATUS ? NOTE_EXITSTATUS : 0) |
            (observation == Observation::EXECUTION ? NOTE_EXEC : 0), 0, nullptr);
    if (observer < 0 || fcntl(observer, F_SETFD, FD_CLOEXEC) != 0 ||
        kevent(observer, &event, 1, nullptr, 0, nullptr) != 0 ||
        process_birth_token((pid_t)process_id) != birth_token)
    {
        if (error) {
            *error = "original process observation registration failed or the occurrence changed";
        }
        close_native_handle(observer);
        return k_invalid_handle;
    }
    return observer;
}

bool observe_execution(int observer, pid_t pid, std::chrono::steady_clock::time_point deadline)
{
    while (true) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            return false;
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
        const timespec timeout{seconds.count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds).count()};
        struct kevent event{};
        const int count = kevent(observer, nullptr, 0, &event, 1, &timeout);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return count == 1 && (event.flags & EV_ERROR) == 0 && event.filter == EVFILT_PROC &&
            event.ident == static_cast<std::uintptr_t>(pid) && (event.fflags & NOTE_EXEC) != 0;
    }
}

bool cloexec_pipe(int descriptors[2])
{
    if (pipe(descriptors) != 0) {
        return false;
    }
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0)
    {
        const int error = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        errno = error;
        return false;
    }
    return true;
}

// What the child reports through the error pipe when it cannot reach exec.
// Everything the child runs before execve is async-signal-safe and touches
// no memory the parent did not prepare, so a failing step is one of these.
enum class Child_step : std::uint32_t
{
    SETSID = 1,
    SIGNALS,
    MOVE_ERROR_PIPE,
    DUPLICATE_SOURCE,
    PLACE_DESCRIPTOR,
    CLOSE_UNLISTED,
    CHDIR,
    EXECVE,
    CONTROLLING_TERMINAL,
    PROCESS_GROUP,
    FOREGROUND_TERMINAL,
};

const char* step_name(Child_step step)
{
    switch (step) {
        case Child_step::SETSID:           return "setsid";
        case Child_step::SIGNALS:          return "signal reset";
        case Child_step::MOVE_ERROR_PIPE:  return "error pipe relocation";
        case Child_step::DUPLICATE_SOURCE: return "descriptor duplication";
        case Child_step::PLACE_DESCRIPTOR: return "dup2";
        case Child_step::CLOSE_UNLISTED:   return "closing an unlisted descriptor";
        case Child_step::CHDIR:            return "chdir";
        case Child_step::EXECVE:           return "execve";
        case Child_step::CONTROLLING_TERMINAL: return "controlling terminal";
        case Child_step::PROCESS_GROUP:       return "process group";
        case Child_step::FOREGROUND_TERMINAL: return "foreground terminal";
        default:                           return "unknown step";
    }
}

struct Child_failure
{
    std::uint32_t step  = 0;
    std::int32_t  error = 0;
};

[[noreturn]] void fail_child(int error_fd, Child_step step, int error)
{
    Child_failure failure;
    failure.step  = static_cast<std::uint32_t>(step);
    failure.error = error;
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&failure);
    std::size_t written = 0;
    while (written < sizeof(failure)) {
        const ssize_t count = write(error_fd, bytes + written, sizeof(failure) - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    _exit(127);
}

// Everything below runs in the forked child: no allocation, no locale, no
// stdio, only async-signal-safe calls on memory the parent prepared.
[[noreturn]] void run_child(
    const Spawn_request& request,
    char* const*         argv,
    char* const*         envp,
    const int*           sources,
    const int*           targets,
    std::size_t          count,
    int                  highest_target,
    int                  error_fd,
    int                  descriptor_limit)
{
    if (request.new_session && setsid() < 0) {
        fail_child(error_fd, Child_step::SETSID, errno);
    }
    if (request.new_process_group && setpgid(0, 0) != 0) {
        fail_child(error_fd, Child_step::PROCESS_GROUP, errno);
    }

    // The parent may ignore SIGPIPE and block SIGCHLD for its own purposes;
    // the child gets the dispositions and mask a fresh process has.
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number == SIGKILL || signal_number == SIGSTOP) {
            continue;
        }
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(signal_number, &action, nullptr);
    }
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    if (sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0) {
        fail_child(error_fd, Child_step::SIGNALS, errno);
    }

    // Move the error pipe above every target so no dup2 can land on it.
    const int relocated_error_fd = fcntl(error_fd, F_DUPFD_CLOEXEC, highest_target + 1);
    if (relocated_error_fd < 0) {
        fail_child(error_fd, Child_step::MOVE_ERROR_PIPE, errno);
    }
    close(error_fd);
    error_fd = relocated_error_fd;

    // Two phases so a map such as {4 -> 3, 3 -> 4} cannot clobber a source
    // before it is copied: every source goes to a fresh high descriptor
    // first, then each high copy is placed at its target.
    int high_copies[64];
    for (std::size_t i = 0; i < count; ++i) {
        high_copies[i] = fcntl(sources[i], F_DUPFD_CLOEXEC, highest_target + 1);
        if (high_copies[i] < 0) {
            fail_child(error_fd, Child_step::DUPLICATE_SOURCE, errno);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        // dup2 onto the target clears close-on-exec, which is the intent.
        if (dup2(high_copies[i], targets[i]) < 0) {
            fail_child(error_fd, Child_step::PLACE_DESCRIPTOR, errno);
        }
    }

    // Close every non-target descriptor so the child's table after exec is
    // exactly the inherited map. The error receipt alone survives until exec.
    if (request.controlling_terminal && ioctl(STDIN_FILENO, TIOCSCTTY, 0) != 0) {
        fail_child(error_fd, Child_step::CONTROLLING_TERMINAL, errno);
    }
    if (request.foreground_terminal) {
        // The persistent owner holds the controlling session. A newly created
        // shell group hands itself foreground before executing user code.
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTTOU);
        pid_t group = getpgrp();
        if (sigprocmask(SIG_BLOCK, &blocked, nullptr) != 0 ||
            ioctl(STDIN_FILENO, TIOCSPGRP, &group) != 0 ||
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0)
        {
            fail_child(error_fd, Child_step::FOREGROUND_TERMINAL, errno);
        }
    }
    for (int fd = 0; fd <= highest_target; ++fd) {
        bool is_target = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (targets[i] == fd) {
                is_target = true;
                break;
            }
        }
        if (!is_target && close(fd) != 0 && errno != EBADF) {
            fail_child(error_fd, Child_step::CLOSE_UNLISTED, errno);
        }
    }
    for (int fd = highest_target + 1; fd < descriptor_limit; ++fd) {
        if (fd != error_fd) {
            (void)close(fd);
        }
    }

    if (!request.working_directory.empty() && chdir(request.working_directory.c_str()) != 0) {
        fail_child(error_fd, Child_step::CHDIR, errno);
    }

    execve(request.executable.c_str(), argv, envp);
    fail_child(error_fd, Child_step::EXECVE, errno);
}

} // namespace

Native_handle open_process_observer(std::int64_t process_id, std::uint64_t birth_token, std::string* error)
{
    return open_occurrence_observer(process_id, birth_token, Observation::EXIT, error);
}

struct Native_process_status_observer::Impl
{
    Native_handle observer = k_invalid_handle;
    std::int64_t process_id = 0;
    bool attempted = false;
    Native_process_status status;
    ~Impl() { close_native_handle(observer); }
};

Native_process_status_observer::Native_process_status_observer()
:
    m_impl(std::make_unique<Impl>())
{}
Native_process_status_observer::~Native_process_status_observer() = default;
Native_process_status_observer::Native_process_status_observer(Native_process_status_observer&&) noexcept = default;
Native_process_status_observer& Native_process_status_observer::operator=(
    Native_process_status_observer&&) noexcept = default;

bool Native_process_status_observer::open(
    std::int64_t process_id, std::uint64_t birth_token, std::string* error)
{
    if (!m_impl || m_impl->attempted) {
        if (error) {
            *error = "process status observation cannot replace an original occurrence";
        }
        return false;
    }
    m_impl->attempted = true;
    m_impl->observer = open_occurrence_observer(process_id, birth_token, Observation::EXIT_STATUS, error);
    if (m_impl->observer < 0) {
        return false;
    }
    m_impl->process_id = process_id;
    m_impl->status.state = Native_process_observation::LIVE;
    return true;
}

Native_handle Native_process_status_observer::native_handle() const noexcept
{
    return m_impl ? m_impl->observer : k_invalid_handle;
}

Native_process_status Native_process_status_observer::observe() noexcept
{
    if (!m_impl || m_impl->status.state != Native_process_observation::LIVE) {
        return m_impl ? m_impl->status : Native_process_status{};
    }
    struct kevent event{};
    const timespec immediate{};
    const int count = kevent(m_impl->observer, nullptr, 0, &event, 1, &immediate);
    if (count == 0 || (count < 0 && errno == EINTR)) {
        return m_impl->status;
    }
    const bool ended = count == 1 && (event.flags & EV_ERROR) == 0 && event.filter == EVFILT_PROC &&
        event.ident == static_cast<std::uintptr_t>(m_impl->process_id) && (event.fflags & NOTE_EXIT) != 0;
    m_impl->status.state = ended ? Native_process_observation::ENDED : Native_process_observation::UNCONFIRMED;
    m_impl->status.status_valid = ended && (event.fflags & NOTE_EXITSTATUS) != 0;
    if (m_impl->status.status_valid) {
        m_impl->status.wait_status = static_cast<int>(event.data);
    }
    close_native_handle(m_impl->observer);
    m_impl->observer = k_invalid_handle;
    return m_impl->status;
}

Spawn_result spawn(const Spawn_request& in_request)
{
    Spawn_result result;
    result.no_workload_admitted = true;

    detail::Prepared_spawn prepared;
    if (!detail::prepare_spawn(in_request, prepared, result.error)) {
        return result;
    }

    int error_pipe[2] = {-1, -1};
    if (!cloexec_pipe(error_pipe)) {
        result.error = std::string("spawn: pipe: ") + std::strerror(errno);
        return result;
    }

    int admission_pipe[2] = {-1, -1};
    if (!cloexec_pipe(admission_pipe)) {
        result.error = std::string("spawn: admission pipe: ") + std::strerror(errno);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return result;
    }
    const int descriptor_limit = getdtablesize();
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("spawn: fork: ") + std::strerror(errno);
        close(error_pipe[0]);
        close(error_pipe[1]);
        close(admission_pipe[0]);
        close(admission_pipe[1]);
        return result;
    }
    if (pid == 0) {
        close(error_pipe[0]);
        close(admission_pipe[1]);
        char admission = 0;
        ssize_t received;
        do { received = read(admission_pipe[0], &admission, 1); } while (received < 0 && errno == EINTR);
        close(admission_pipe[0]);
        if (received != 1 || admission != 'A') _exit(127);
        run_child(
            in_request,
            prepared.argv.data(),
            prepared.envp.data(),
            prepared.sources,
            prepared.targets,
            in_request.inherited.size(),
            prepared.highest_target,
            error_pipe[1],
            descriptor_limit);
    }

    close(error_pipe[1]);
    close(admission_pipe[0]);
    result.no_workload_admitted = false;

    // The sole parent registers before execution is admitted. This descriptor
    // observes the original occurrence; it never authorizes a numeric signal.
    const std::uint64_t birth_token = process_birth_token(pid);
    std::string observer_error;
    const int observer = open_process_observer(pid, birth_token, &observer_error);
    const int execution_observer = in_request.execution_deadline ?
        open_occurrence_observer(pid, birth_token, Observation::EXECUTION, &observer_error) : -1;
    const bool observed = observer >= 0 && (!in_request.execution_deadline || execution_observer >= 0);
    const bool admitted = observed && (!in_request.admit_execution || in_request.admit_execution());
    ssize_t released = -1;
    if (admitted) {
        do { released = write(admission_pipe[1], "A", 1); } while (released < 0 && errno == EINTR);
    }
    close(admission_pipe[1]);
    if (!admitted || released != 1) {
        close_native_handle(execution_observer);
        // Still our unreaped child: no workload instruction passed the gate.
        (void)kill(pid, SIGKILL);
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        result.no_workload_admitted = waited == pid;
        if (waited == pid) {
            close_native_handle(observer);
        }
        else {
            result.failed_birth = Darwin_spawn_failure{pid, birth_token, observer, false, false, 0};
        }
        close(error_pipe[0]);
        result.error = !observed ? "spawn: " + observer_error :
            "spawn: execution admission cancelled";
        return result;
    }

    Child_failure failure;
    std::uint8_t* bytes  = reinterpret_cast<std::uint8_t*>(&failure);
    std::size_t   filled = 0;
    bool pipe_complete = false;
    while (filled < sizeof(failure)) {
        if (in_request.execution_deadline) {
            const auto remaining = *in_request.execution_deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            pollfd readiness{error_pipe[0], POLLIN, 0};
            const auto wait_ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            const int ready = poll(&readiness, 1,
                static_cast<int>(std::min<std::int64_t>(wait_ms, std::numeric_limits<int>::max())));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready != 1 || (readiness.revents & (POLLIN | POLLHUP)) == 0) {
                break;
            }
        }
        const ssize_t count = read(error_pipe[0], bytes + filled, sizeof(failure) - filled);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 0) {
            pipe_complete = true;
            break;
        }
        filled += static_cast<std::size_t>(count);
    }
    close(error_pipe[0]);

    if (filled != 0) {
        close_native_handle(execution_observer);
        // The child never reached exec and has exited (or is about to); it is
        // this process's direct child and no other owner waits it.
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid, &status, in_request.execution_deadline ? WNOHANG : 0); }
        while (waited < 0 && errno == EINTR);
        result.failed_birth = Darwin_spawn_failure{
            pid, birth_token, waited == pid ? k_invalid_handle : observer, true, waited == pid, status};
        if (waited == pid) close_native_handle(observer);
        if (filled == sizeof(failure)) {
            result.error = std::string("spawn: ") + step_name(static_cast<Child_step>(failure.step)) +
                " failed in the child: " + std::strerror(failure.error);
        }
        else {
            result.error = "spawn: the child reported a truncated failure record";
        }
        return result;
    }

    if (in_request.execution_deadline) {
        result.execution_confirmed = pipe_complete &&
            observe_execution(execution_observer, pid, *in_request.execution_deadline);
        close_native_handle(execution_observer);
        if (!result.execution_confirmed) {
            // Execution may have happened. Keep the original child and exit
            // observer under the sole parent's normal cleanup protocol.
            result.failed_birth = Darwin_spawn_failure{pid, birth_token, observer, true, false, 0};
            result.error = "spawn: command execution was not confirmed";
            return result;
        }
    }

    result.ok             = true;
    result.process_id     = pid;
    result.process_handle = observer;
    result.process_birth_token = birth_token;
    result.inherited_handles = std::move(prepared.child_handles);
    return result;
}

} // namespace vnm::process_custody
