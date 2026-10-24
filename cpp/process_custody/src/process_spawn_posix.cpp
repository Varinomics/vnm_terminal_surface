#include <vnm_process_custody/process_spawn.h>
#include "process_spawn_common_posix.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/sched.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

namespace vnm::process_custody {

namespace {

bool join_cpu_accounting_cgroup(int directory, pid_t pid)
{
    const int descriptor = openat(directory, "cgroup.procs", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    char text[32];
    const auto parsed = std::to_chars(text, text + sizeof(text) - 1, pid);
    if (parsed.ec != std::errc()) {
        close(descriptor);
        return false;
    }
    *parsed.ptr = '\n';
    const auto size = static_cast<std::size_t>(parsed.ptr - text + 1);
    ssize_t written;
    do {
        written = write(descriptor, text, size);
    }
    while (written < 0 && errno == EINTR);
    close(descriptor);
    return written == static_cast<ssize_t>(size);
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
    CLOSE_RANGE,
    CHDIR,
    EXECVE,
    CONTROLLING_TERMINAL,
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
        case Child_step::CLOSE_RANGE:      return "close_range";
        case Child_step::CHDIR:            return "chdir";
        case Child_step::EXECVE:           return "execve";
        case Child_step::CONTROLLING_TERMINAL: return "controlling terminal";
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
    int                  error_fd)
{
    if (request.new_session && setsid() < 0) {
        fail_child(error_fd, Child_step::SETSID, errno);
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

    // Descriptors within the target range that are not targets are closed
    // outright; everything above is marked close-on-exec (the high copies and
    // the error pipe already are), so exec is the point where the child's
    // table shrinks to exactly the inherited list.
    if (request.controlling_terminal && ioctl(STDIN_FILENO, TIOCSCTTY, 0) != 0) {
        fail_child(error_fd, Child_step::CONTROLLING_TERMINAL, errno);
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
    if (close_range(static_cast<unsigned int>(highest_target + 1), ~0U, CLOSE_RANGE_CLOEXEC) != 0) {
        fail_child(error_fd, Child_step::CLOSE_RANGE, errno);
    }

    if (!request.working_directory.empty() && chdir(request.working_directory.c_str()) != 0) {
        fail_child(error_fd, Child_step::CHDIR, errno);
    }

    execve(request.executable.c_str(), argv, envp);
    fail_child(error_fd, Child_step::EXECVE, errno);
}

} // namespace

Spawn_result spawn(const Spawn_request& in_request)
{
    Spawn_result result;

    detail::Prepared_spawn prepared;
    if (!detail::prepare_spawn(in_request, prepared, result.error)) {
        return result;
    }

    int error_pipe[2] = {-1, -1};
    if (pipe2(error_pipe, O_CLOEXEC) != 0) {
        result.error = std::string("spawn: pipe2: ") + std::strerror(errno);
        return result;
    }

    int admission_pipe[2] = {-1, -1};
    if (pipe2(admission_pipe, O_CLOEXEC) != 0) {
        result.error = std::string("spawn: admission pipe: ") + std::strerror(errno);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return result;
    }
    pid_t pid = -1;
    if (in_request.generation_cgroup >= 0) {
#if defined(SYS_clone3) && defined(CLONE_INTO_CGROUP)
        // There is no fork-then-migrate window. A refused containment admission
        // creates no child and must never fall back to an uncontained fork.
        struct clone_args arguments{};
        arguments.flags = CLONE_INTO_CGROUP;
        arguments.exit_signal = SIGCHLD;
        arguments.cgroup = static_cast<std::uint64_t>(in_request.generation_cgroup);
        pid = static_cast<pid_t>(syscall(SYS_clone3, &arguments, sizeof(arguments)));
#else
        errno = ENOTSUP;
#endif
    }
    else {
        pid = fork();
    }
    if (pid < 0) {
        result.error = std::string(in_request.generation_cgroup >= 0 ?
            "spawn: atomic generation clone3: " : "spawn: fork: ") + std::strerror(errno);
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
            error_pipe[1]);
    }

    close(error_pipe[1]);
    close(admission_pipe[0]);

    // The pidfd is opened before anything could reap the child: this process
    // has not returned to a wait loop yet, so the pid cannot be recycled.
    const int pidfd = posix_pidfd_open(pid);
    const int pidfd_error = errno;
    // The child cannot execute or fork before admission. Optional accounting
    // may use migration here without weakening atomic generation containment.
    const bool cpu_accounting_admitted = pidfd >= 0 && in_request.generation_cgroup < 0 &&
        in_request.cpu_accounting_cgroup >= 0 &&
        join_cpu_accounting_cgroup(in_request.cpu_accounting_cgroup, pid);
    const bool admitted = pidfd >= 0 && (!in_request.admit_execution || in_request.admit_execution());
    ssize_t released = -1;
    if (admitted) {
        do { released = write(admission_pipe[1], "A", 1); } while (released < 0 && errno == EINTR);
    }
    close(admission_pipe[1]);
    if (!admitted || released != 1) {
        // Still our unreaped child: no workload instruction passed the gate.
        (void)kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (pidfd >= 0) close(pidfd);
        close(error_pipe[0]);
        result.error = pidfd < 0 ? std::string("spawn: pidfd_open: ") + std::strerror(pidfd_error) :
            "spawn: execution admission cancelled";
        return result;
    }

    Child_failure failure;
    std::uint8_t* bytes  = reinterpret_cast<std::uint8_t*>(&failure);
    std::size_t   filled = 0;
    while (filled < sizeof(failure)) {
        const ssize_t count = read(error_pipe[0], bytes + filled, sizeof(failure) - filled);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 0) {
            break;
        }
        filled += static_cast<std::size_t>(count);
    }
    close(error_pipe[0]);

    if (filled != 0) {
        // The child never reached exec and has exited (or is about to); it is
        // this process's direct child and no other owner waits it.
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (pidfd >= 0) {
            close(pidfd);
        }
        if (filled == sizeof(failure)) {
            result.error = std::string("spawn: ") + step_name(static_cast<Child_step>(failure.step)) +
                " failed in the child: " + std::strerror(failure.error);
        }
        else {
            result.error = "spawn: the child reported a truncated failure record";
        }
        return result;
    }

    if (pidfd < 0) {
        // Without a pidfd the caller cannot own the child the way the
        // contract requires; the child is ours to stop.
        result.error = std::string("spawn: pidfd_open: ") + std::strerror(errno);
        (void)kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return result;
    }

    result.ok             = true;
    result.process_id     = pid;
    result.process_handle = pidfd;
    result.cpu_accounting_admitted = cpu_accounting_admitted;
    result.inherited_handles = std::move(prepared.child_handles);
    return result;
}

} // namespace vnm::process_custody
