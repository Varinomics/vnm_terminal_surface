#include <vnm_process_custody/process_spawn.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/sched.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace vnm::process_custody {

namespace {

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

    if (in_request.inherited.size() > 64) {
        result.error = "spawn: more than 64 inherited descriptors";
        return result;
    }

    // Everything the child needs is materialized here, before fork. Typed
    // inherited arguments resolve from the same descriptor table that dup2
    // applies, so argv cannot name the parent's descriptor by accident.
    std::vector<std::string> resolved_arguments;
    resolved_arguments.reserve(in_request.argv.size());
    for (const Spawn_argument& argument : in_request.argv) {
        if (const auto* text = std::get_if<std::string>(&argument.value)) {
            resolved_arguments.push_back(*text);
            continue;
        }
        const std::size_t index = std::get<Inherited_reference>(argument.value).inherited_index;
        if (index >= in_request.inherited.size()) {
            result.error = "spawn: inherited argv reference is out of range";
            return result;
        }
        resolved_arguments.push_back(std::to_string(in_request.inherited[index].child_number));
    }

    std::vector<char*> argv;
    argv.reserve(resolved_arguments.size() + 1);
    for (const std::string& argument : resolved_arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<char*> envp;
    envp.reserve(in_request.environment.size() + 1);
    for (const std::string& entry : in_request.environment) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    int sources[64];
    int targets[64];
    std::vector<Native_handle> child_handles;
    child_handles.reserve(in_request.inherited.size());
    int highest_target = 2;   // stdio positions are always part of the range that gets tidied
    for (std::size_t i = 0; i < in_request.inherited.size(); ++i) {
        const Inherited_descriptor& descriptor = in_request.inherited[i];
        if (descriptor.parent_handle < 0 || descriptor.child_number < 0 ||
            descriptor.child_number > std::numeric_limits<int>::max() - 65) {
            result.error = "spawn: inherited descriptor entry is not a valid descriptor/number pair";
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (targets[j] == descriptor.child_number) {
                result.error = "spawn: two inherited descriptors share child number " +
                    std::to_string(descriptor.child_number);
                return result;
            }
        }
        sources[i]     = descriptor.parent_handle;
        targets[i]     = descriptor.child_number;
        child_handles.push_back(descriptor.child_number);
        highest_target = std::max(highest_target, descriptor.child_number);
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
            argv.data(),
            envp.data(),
            sources,
            targets,
            in_request.inherited.size(),
            highest_target,
            error_pipe[1]);
    }

    close(error_pipe[1]);
    close(admission_pipe[0]);

    // The pidfd is opened before anything could reap the child: this process
    // has not returned to a wait loop yet, so the pid cannot be recycled.
    const int pidfd = posix_pidfd_open(pid);
    const int pidfd_error = errno;
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
    result.inherited_handles = std::move(child_handles);
    return result;
}

Inherited_reference inherited_argument(std::size_t in_inherited_index)
{
    return Inherited_reference{in_inherited_index};
}

std::vector<Inherited_descriptor> inherit_standard_streams()
{
    std::vector<Inherited_descriptor> streams;
    for (int fd = 0; fd <= 2; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            streams.push_back(Inherited_descriptor{fd, fd});
        }
    }
    return streams;
}

bool prepare_parent_process(std::string* out_error)
{
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) != 0) {
        *out_error = std::string("sigaction(SIGPIPE): ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace vnm::process_custody
