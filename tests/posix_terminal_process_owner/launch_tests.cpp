#include "launch_syscalls.h"
#undef open
#undef posix_openpt
#undef login_tty
#undef fork
#if defined(__APPLE__)
#undef pipe
#undef kevent
#undef sysctl
#else
#undef pipe2
#endif

#include "posix_terminal_process_owner.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace terminal = vnm_terminal;

constexpr int k_control_descriptor = 64;

enum class Probe
{
    NONE, MASTER, SLAVE, PIPE, SIGNAL, FORK_FAILURE,
    PIPE_EXEC_EVENT, PIPE_EXITED, PIPE_REJECTED, PIPE_SIGNAL,
};
Probe probe = Probe::NONE;

bool inherited_startup_probe(Probe kind)
{
#if defined(__APPLE__)
    return kind == Probe::PIPE || kind == Probe::PIPE_EXEC_EVENT || kind == Probe::PIPE_EXITED ||
        kind == Probe::PIPE_REJECTED || kind == Probe::PIPE_SIGNAL;
#else
    (void)kind;
    return false;
#endif
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool transfer(int descriptor, void* data, std::size_t size, bool writing, int timeout_ms = 3000)
{
    auto* bytes = static_cast<char*>(data);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (size != 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0) {
            return false;
        }
        pollfd ready{descriptor, (short)(writing ? POLLOUT : POLLIN), 0};
        const int result = ::poll(&ready, 1, (int)remaining);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        const auto count = writing ? ::write(descriptor, bytes, size) : ::read(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size  -= (std::size_t)count;
    }
    return true;
}

class Pipe
{
public:
    Pipe()
    {
        require(::pipe(m_descriptors) == 0, "prepare fixture pipe before concurrent spawning");
        for (int descriptor : m_descriptors) {
            require(::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0, "prepare fixture pipe flags");
        }
    }

    ~Pipe()
    {
        ::close(m_descriptors[0]);
        ::close(m_descriptors[1]);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    int reader() const { return m_descriptors[0]; }
    int writer() const { return m_descriptors[1]; }

private:
    int m_descriptors[2] = {-1, -1};
};

struct Barrier
{
    std::mutex mutex;
    std::condition_variable changed;
    bool captured = false;
    bool released = false;
    bool launch_finished = false;
    pid_t foreign_child = -1;
    int first     = -1;
    int second    = -1;
    int flags     =  0;
};

Barrier* descriptor_barrier = nullptr;
int signal_ready_writer    = -1;
int signal_release_reader  = -1;
int signal_handler_writer  = -1;
int exec_release_reader    = -1;
int exec_release_writer    = -1;

void capture_descriptor(int first, int second = -1)
{
    auto& barrier = *descriptor_barrier;
    std::unique_lock lock(barrier.mutex);
    barrier.first    = first;
    barrier.second   = second;
    barrier.flags    = ::fcntl(first, F_GETFD);
    barrier.captured = true;
    barrier.changed.notify_all();
    require(barrier.changed.wait_for(lock, 5s, [&barrier] { return barrier.released; }),
        "independent embedding fork/exec returns within bound");
}

terminal::terminal_process_launch_t launch_request(const std::string& executable, Pipe& control)
{
    terminal::terminal_process_launch_t request;
    request.executable  = executable;
    request.argv        = {executable, "--terminal-child"};
    request.environment = {"PATH=/usr/bin:/bin", "TERM=xterm-256color"};
    request.inherited   = {{control.reader(), k_control_descriptor}};
    request.rows        = 24;
    request.columns     = 80;
    return request;
}

bool masks_match(const sigset_t& first, const sigset_t& second)
{
    for (int signal = 1; signal < NSIG; ++signal) {
        if (sigismember(&first, signal) != sigismember(&second, signal)) {
            return false;
        }
    }
    return true;
}

void finish_owner(terminal::Terminal_process_owner& owner, pid_t child)
{
    siginfo_t held{};
    require(::waitid(P_PID, (id_t)child, &held, WEXITED | WNOWAIT | WNOHANG) == 0 && held.si_pid == child,
        "root wait remains held until finish");
    owner.finish();
    require(owner.confirmed(), "native cleanup is confirmed");
    errno = 0;
    require(::waitid(P_PID, (id_t)child, &held, WEXITED | WNOWAIT | WNOHANG) < 0 && errno == ECHILD,
        "owner consumes its sole wait");
}

void descriptor_test(const std::string& executable, Probe kind)
{
    const bool inherited_startup = inherited_startup_probe(kind);
    const bool rejected = kind == Probe::PIPE_REJECTED || kind == Probe::PIPE_SIGNAL;
    sigset_t blocked;
    sigset_t original;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    require(::pthread_sigmask(SIG_BLOCK, &blocked, &original) == 0, "set nonempty parent mask");
    sigset_t before;
    (void)::pthread_sigmask(SIG_SETMASK, nullptr, &before);
    Pipe control;
    Pipe foreign_control;
    Pipe foreign_events;
    Pipe exec_release;
    exec_release_reader = exec_release.reader();
    exec_release_writer = exec_release.writer();
    Barrier barrier;
    descriptor_barrier = &barrier;
    probe              = kind;
    bool foreign_ok    = false;
    std::string foreign_error;
    auto owner = terminal::internal::make_posix_terminal_process_owner();
    std::thread foreign([
            &barrier, &foreign_control, &foreign_events, &foreign_ok, &foreign_error, &executable,
            inherited_startup
        ]
        {
            try {
                std::unique_lock lock(barrier.mutex);
                require(barrier.changed.wait_for(lock, 5s, [&barrier] { return barrier.captured; }),
                    "owner reaches native descriptor creation");
                const std::vector<std::string> storage{executable, "--foreign-child",
                    std::to_string(barrier.first), std::to_string(barrier.second),
                    std::to_string(foreign_control.reader()), std::to_string(foreign_events.writer())};
                std::vector<char*> arguments;
                for (const auto& argument : storage) {
                    arguments.push_back(const_cast<char*>(argument.c_str()));
                }
                arguments.push_back(nullptr);
                const pid_t child = ::fork();
                require(child >= 0, "independent embedding fork");
                if (child == 0) {
                    (void)::fcntl(foreign_control.reader(), F_SETFD, 0);
                    (void)::fcntl(foreign_events.writer(), F_SETFD, 0);
                    ::execv(executable.c_str(), arguments.data());
                    ::_exit(111);
                }
                char report = 0;
                const bool reported = transfer(foreign_events.reader(), &report, 1, false);
                if (inherited_startup) {
                    barrier.foreign_child = child;
                    barrier.released = true;
                    barrier.changed.notify_all();
                    (void)barrier.changed.wait_for(lock, 20s, [&barrier] { return barrier.launch_finished; });
                }
                char release = 'R';
                (void)transfer(foreign_control.writer(), &release, 1, true);
                int status = 0;
                pid_t waited;
                do {
                    waited = ::waitpid(child, &status, 0);
                } while (waited < 0 && errno == EINTR);
                foreign_ok = reported && report == (inherited_startup ? 'W' : 'C') && waited == child &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0;
                barrier.released = true;
                barrier.changed.notify_all();
            }
            catch (const std::exception& error) {
                foreign_error = error.what();
                const std::lock_guard lock(barrier.mutex);
                barrier.released = true;
                barrier.changed.notify_all();
            }
        });

    terminal::terminal_process_launch_result_t launched;
    bool foreign_still_running = false;
    const auto launch_started = Clock::now();
    try {
        auto request = launch_request(executable, control);
        if (kind == Probe::PIPE_EXITED) {
            request.argv[1] = "--terminal-immediate-child";
        }
        if (kind == Probe::PIPE_REJECTED) {
            request.executable = "/unavailable/terminal-owner-launch-fixture";
        }
        launched = owner->launch(request);
        const std::lock_guard lock(barrier.mutex);
        if (inherited_startup && barrier.foreign_child > 0) {
            siginfo_t observed{};
            foreign_still_running = ::waitid(P_PID, (id_t)barrier.foreign_child, &observed,
                WEXITED | WNOWAIT | WNOHANG) == 0 && observed.si_pid == 0;
        }
        barrier.launch_finished = true;
        barrier.changed.notify_all();
    }
    catch (...) {
        {
            const std::lock_guard lock(barrier.mutex);
            barrier.launch_finished = true;
            barrier.changed.notify_all();
        }
        foreign.join();
        throw;
    }
    const auto launch_duration = Clock::now() - launch_started;
    foreign.join();
    probe = Probe::NONE;
    descriptor_barrier = nullptr;
    sigset_t after;
    (void)::pthread_sigmask(SIG_SETMASK, nullptr, &after);
    (void)::pthread_sigmask(SIG_SETMASK, &original, nullptr);
    require(masks_match(before, after), "successful fork restores the exact nonempty parent mask");
    require(launched.accepted == !rejected, "native image replacement decides terminal launch acceptance");
    if (launched.accepted && kind != Probe::PIPE_EXITED) {
        char release = 'R';
        require(transfer(control.writer(), &release, 1, true), "release terminal child cooperatively");
    }
    const auto exited = owner->wait_root();
    if (launched.master_descriptor >= 0) {
        ::close(launched.master_descriptor);
    }
    finish_owner(*owner, (pid_t)launched.root_process_id);
    if (kind == Probe::PIPE_REJECTED) {
        require(exited && exited->exit_code == 127 && launched.start_outcome_determinate &&
            launched.error == std::string("terminal exec: ") + std::strerror(ENOENT),
            "complete child errno establishes rejected exec with its actual cause");
    }
    else
    if (kind == Probe::PIPE_SIGNAL) {
        require(exited && exited->signal == SIGKILL && !launched.start_outcome_determinate,
            "pre-exec death cannot establish successful image replacement");
    }
    else {
        require(exited && exited->exit_code == 0 && exited->signal == 0, "terminal child exits normally");
    }
    if (inherited_startup) {
        require(foreign_error.empty() && foreign_ok && foreign_still_running && launch_duration < 4s,
            "launch completes while an independent exec still holds the startup writer");
        std::printf("inherited_writer case=%d launch_ms=%lld accepted=%d determinate=%d held_foreign=1\n",
            (int)kind, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(launch_duration).count(),
            launched.accepted, launched.start_outcome_determinate);
    }
    else {
        require(foreign_error.empty() && foreign_ok && (barrier.flags & FD_CLOEXEC) != 0,
            "descriptor is close-on-exec at creation and absent from independent exec");
    }
}

void inherited_signal_handler(int)
{
    (void)::write(signal_handler_writer, "H", 1);
}

void signal_test(const std::string& executable)
{
    Pipe control;
    Pipe ready;
    Pipe release;
    Pipe handler_events;
    signal_ready_writer   = ready.writer();
    signal_release_reader = release.reader();
    signal_handler_writer = handler_events.writer();
    struct sigaction action{};
    struct sigaction original{};
    action.sa_handler = inherited_signal_handler;
    sigemptyset(&action.sa_mask);
    require(::sigaction(SIGUSR1, &action, &original) == 0, "install embedding handler in isolated fixture");
    sigset_t before;
    require(::pthread_sigmask(SIG_SETMASK, nullptr, &before) == 0, "read launching thread mask");
    probe = Probe::SIGNAL;
    bool delivered = false;
    auto owner = terminal::internal::make_posix_terminal_process_owner();
    std::thread signaler([&ready, &release, &delivered] {
        pid_t child = -1;
        if (transfer(ready.reader(), &child, sizeof(child), false)) {
            siginfo_t held{};
            if (::waitid(P_PID, (id_t)child, &held, WEXITED | WNOWAIT | WNOHANG) == 0) {
                delivered = ::kill(child, SIGUSR1) == 0;
            }
        }
        char command = 'R';
        (void)transfer(release.writer(), &command, 1, true);
    });
    terminal::terminal_process_launch_result_t launched;
    try {
        launched = owner->launch(launch_request(executable, control));
    }
    catch (...) {
        signaler.join();
        throw;
    }
    signaler.join();
    probe = Probe::NONE;
    sigset_t after;
    require(::pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0 && masks_match(before, after),
        "successful fork restores the exact parent mask");
    require(::sigaction(SIGUSR1, &original, nullptr) == 0, "restore isolated embedding handler");
    require(launched.native_dispatch_occurred && delivered, "signal targets the exact owned pre-exec child");
    const auto exited = owner->wait_root();
    if (launched.master_descriptor >= 0) {
        ::close(launched.master_descriptor);
    }
    finish_owner(*owner, (pid_t)launched.root_process_id);
    char marker = 0;
    require(!transfer(handler_events.reader(), &marker, 1, false, 50),
        "inherited host handler never runs in the launch child");
    require(exited && exited->signal == SIGUSR1,
        "pending signal takes its restored default action before exec");
}

void fork_failure_test(const std::string& executable)
{
    Pipe control;
    sigset_t blocked;
    sigset_t original;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    require(::pthread_sigmask(SIG_BLOCK, &blocked, &original) == 0, "set nonempty parent mask");
    sigset_t before;
    (void)::pthread_sigmask(SIG_SETMASK, nullptr, &before);
    probe = Probe::FORK_FAILURE;
    auto owner = terminal::internal::make_posix_terminal_process_owner();
    const auto launched = owner->launch(launch_request(executable, control));
    probe = Probe::NONE;
    sigset_t after;
    (void)::pthread_sigmask(SIG_SETMASK, nullptr, &after);
    (void)::pthread_sigmask(SIG_SETMASK, &original, nullptr);
    owner->finish();
    require(!launched.native_dispatch_occurred && !launched.accepted && owner->confirmed(),
        "failed fork creates no native ownership obligation");
    require(masks_match(before, after), "failed fork restores the exact nonempty parent mask");
    require(launched.error == std::string("fork terminal: ") + std::strerror(EAGAIN),
        "mask restoration preserves the actual fork errno");
}

} // namespace

extern "C" int owner_test_open(const char* path, int flags)
{
    const int descriptor = ::open(path, flags);
    if (descriptor >= 0 && probe == Probe::SLAVE && (flags & O_NOCTTY) != 0) {
        capture_descriptor(descriptor);
    }
    return descriptor;
}

extern "C" int owner_test_posix_openpt(int flags)
{
    const int descriptor = ::posix_openpt(flags);
    if (descriptor >= 0 && probe == Probe::MASTER) {
        capture_descriptor(descriptor);
    }
    return descriptor;
}

#if defined(__APPLE__)
extern "C" int owner_test_pipe(int descriptors[2])
{
    const int result = ::pipe(descriptors);
    if (result == 0 && inherited_startup_probe(probe)) {
        capture_descriptor(descriptors[0], descriptors[1]);
    }
    return result;
}

extern "C" int owner_test_kevent(int queue, const struct kevent* changes, int change_count,
    struct kevent* events, int event_count, const struct timespec* timeout)
{
    if (probe == Probe::PIPE_EXITED || probe == Probe::PIPE_REJECTED) {
        for (int index = 0; index < change_count; ++index) {
            if (changes[index].filter == EVFILT_PROC && (changes[index].flags & EV_ADD) != 0) {
                siginfo_t observed{};
                if (::waitid(P_PID, (id_t)changes[index].ident, &observed, WEXITED | WNOWAIT) < 0) {
                    return -1;
                }
            }
        }
    }
    return ::kevent(queue, changes, change_count, events, event_count, timeout);
}

extern "C" int owner_test_sysctl(int* name, u_int length, void* data, size_t* size,
    void* replacement, size_t replacement_size)
{
    const int result = ::sysctl(name, length, data, size, replacement, replacement_size);
    if (probe == Probe::PIPE_EXEC_EVENT && length == 4 && name[0] == CTL_KERN &&
        name[1] == KERN_PROC && name[2] == KERN_PROC_PID)
    {
        char release = 'R';
        if (!transfer(exec_release_writer, &release, 1, true)) {
            errno = EIO;
            return -1;
        }
    }
    return result;
}
#else
extern "C" int owner_test_pipe2(int descriptors[2], int flags)
{
    const int result = ::pipe2(descriptors, flags);
    if (result == 0 && probe == Probe::PIPE) {
        capture_descriptor(descriptors[0], descriptors[1]);
    }
    return result;
}
#endif

extern "C" pid_t owner_test_fork()
{
    if (probe == Probe::FORK_FAILURE) {
        errno = EAGAIN;
        return -1;
    }
    return ::fork();
}

extern "C" int owner_test_login_tty(int descriptor)
{
    if (probe == Probe::PIPE_SIGNAL) {
        ::kill(::getpid(), SIGKILL);
        ::_exit(114);
    }
    if (probe == Probe::PIPE_EXEC_EVENT) {
        char command = 0;
        if (!transfer(exec_release_reader, &command, 1, false)) {
            ::_exit(115);
        }
    }
    if (probe == Probe::SIGNAL) {
        pid_t child = ::getpid();
        if (!transfer(signal_ready_writer, &child, sizeof(child), true)) {
            ::_exit(112);
        }
        char command = 0;
        if (!transfer(signal_release_reader, &command, 1, false)) {
            ::_exit(113);
        }
    }
    return ::login_tty(descriptor);
}

int main(int argc, char** argv)
{
    ::alarm(25);
    if (argc == 2 && std::strcmp(argv[1], "--terminal-immediate-child") == 0) {
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--terminal-child") == 0) {
        char release = 0;
        sigset_t mask;
        if (::pthread_sigmask(SIG_SETMASK, nullptr, &mask) != 0 || sigismember(&mask, SIGUSR2) != 0) {
            return 3;
        }
        return transfer(k_control_descriptor, &release, 1, false) && release == 'R' ? 0 : 4;
    }
    if (argc == 6 && std::strcmp(argv[1], "--foreign-child") == 0) {
        const int first  = std::atoi(argv[2]);
        const int second = std::atoi(argv[3]);
        const bool closed = ::fcntl(first, F_GETFD) < 0 && errno == EBADF &&
            (second < 0 || (::fcntl(second, F_GETFD) < 0 && errno == EBADF));
        const bool writer_retained = second >= 0 && ::fcntl(second, F_GETFD) >= 0;
        char report = closed ? 'C' : writer_retained ? 'W' : 'L';
        char release = 0;
        return transfer(std::atoi(argv[5]), &report, 1, true) &&
            transfer(std::atoi(argv[4]), &release, 1, false, 20000) && release == 'R' ? 0 : 5;
    }
    try {
        require(argc == 2, "one native launch case is required");
        const std::string executable = argv[0];
        if (std::strcmp(argv[1], "master") == 0) {
            descriptor_test(executable, Probe::MASTER);
        }
        else
        if (std::strcmp(argv[1], "slave") == 0) {
            descriptor_test(executable, Probe::SLAVE);
        }
        else
        if (std::strcmp(argv[1], "pipe") == 0) {
            descriptor_test(executable, Probe::PIPE);
        }
        else
        if (std::strcmp(argv[1], "pipe_exec_event") == 0) {
            descriptor_test(executable, Probe::PIPE_EXEC_EVENT);
        }
        else
        if (std::strcmp(argv[1], "pipe_exited") == 0) {
            descriptor_test(executable, Probe::PIPE_EXITED);
        }
        else
        if (std::strcmp(argv[1], "pipe_rejected") == 0) {
            descriptor_test(executable, Probe::PIPE_REJECTED);
        }
        else
        if (std::strcmp(argv[1], "pipe_signal") == 0) {
            descriptor_test(executable, Probe::PIPE_SIGNAL);
        }
        else
        if (std::strcmp(argv[1], "signal") == 0) {
            signal_test(executable);
        }
        else
        if (std::strcmp(argv[1], "fork_failure") == 0) {
            fork_failure_test(executable);
        }
        else {
            return 2;
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
