#pragma once

#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace vnm_terminal::test_helpers {

struct foreground_cleanup_identity_t
{
    pid_t root;
    pid_t foreground;
    pid_t session;
    pid_t observed_foreground;
    pid_t owner;
};

inline bool read_foreground_control(int fd, void* bytes, std::size_t size)
{
    pollfd ready{fd, POLLIN, 0};
    int result;
    do {
        result = ::poll(&ready, 1, 10000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
        return false;
    }
    ssize_t count;
    do {
        count = ::read(fd, bytes, size);
    } while (count < 0 && errno == EINTR);
    return count == static_cast<ssize_t>(size);
}

// Exec enters this mode before any backend/Qt worker exists. The job ignores
// terminal hangup and exits only on command-pipe EOF or native termination.
inline int run_foreground_cleanup_shell(int control, int report)
{
    if (::signal(SIGHUP, SIG_IGN) == SIG_ERR || ::signal(SIGTTOU, SIG_IGN) == SIG_ERR) {
        return 110;
    }
    int go[2];
    int retire[2];
    if (::pipe(go) != 0 || ::pipe(retire) != 0) {
        return 111;
    }
    const pid_t job = ::fork();
    if (job < 0) {
        return 112;
    }
    if (job == 0) {
        ::close(go[1]);
        ::close(retire[0]);
        if (::setpgid(0, 0) != 0) {
            ::_exit(113);
        }
        char command = 0;
        if (!read_foreground_control(go[0], &command, 1) || command != 'g') {
            ::_exit(114);
        }
        ::close(go[0]);
        constexpr char marker[] = "FOREGROUND_CLEANUP_READY\n";
        if (::write(STDOUT_FILENO, marker, sizeof(marker) - 1) != sizeof(marker) - 1) {
            ::_exit(115);
        }
        for (;;) {
            ssize_t count;
            do {
                count = ::read(control, &command, 1);
            } while (count < 0 && errno == EINTR);
            if (count == 0) {
                ::_exit(23);
            }
            if (count == 1 && command == 'r') {
                if (::write(retire[1], "r", 1) != 1) {
                    ::_exit(120);
                }
                continue;
            }
            if (count != 1 || command != 'p' || ::write(report, "a", 1) != 1) {
                ::_exit(116);
            }
        }
    }
    ::close(go[0]);
    ::close(control);
    if (::setpgid(job, job) != 0 || ::tcsetpgrp(STDIN_FILENO, job) != 0) {
        ::close(go[1]);
        return 117;
    }
    const foreground_cleanup_identity_t identity{
        ::getpid(), job, ::tcgetsid(STDIN_FILENO), ::tcgetpgrp(STDIN_FILENO), ::getppid()};
    if (::write(report, &identity, sizeof(identity)) != sizeof(identity) ||
        ::write(go[1], "g", 1) != 1)
    {
        ::close(go[1]);
        return 118;
    }
    ::close(go[1]);
    ::close(report);
    ::close(retire[1]);
    char retire_command = 0;
    ssize_t retired;
    do {
        retired = ::read(retire[0], &retire_command, 1);
    } while (retired < 0 && errno == EINTR);
    ::close(retire[0]);
    if (retired == 1 && retire_command == 'r') {
        return 23; // Real orphan adoption, while the foreground job stays alive.
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(job, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == job ? 0 : 119;
}

class Foreground_fixture_pipe
{
public:
    ~Foreground_fixture_pipe() { close_read(); close_write(); }
    bool open()
    {
        if (::pipe(m_fds) != 0) {
            return false;
        }
        return ::fcntl(m_fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
               ::fcntl(m_fds[1], F_SETFD, FD_CLOEXEC) == 0;
    }
    int read_end() const { return m_fds[0]; }
    int write_end() const { return m_fds[1]; }
    void close_read() { close_end(m_fds[0]); }
    void close_write() { close_end(m_fds[1]); }

private:
    static void close_end(int& fd)
    {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    int m_fds[2]{-1, -1};
};

enum class Foreground_cleanup_case { NORMAL, ADOPTED, HOST_EOF, OWNER_LOSS };

#if defined(__linux__)
// The controller owns only its directly forked test host. Early fixture
// failures must not leave that host paused with an open owner-control socket.
class Foreground_fixture_host
{
public:
    ~Foreground_fixture_host() { (void)terminate_and_collect(); }

    bool adopt(pid_t child)
    {
        m_child = child;
        m_reference = static_cast<int>(::syscall(SYS_pidfd_open, child, 0));
        return m_reference >= 0;
    }

    bool terminate_and_collect()
    {
        if (m_child <= 0) {
            return true;
        }
        if (m_reference >= 0) {
            (void)::syscall(SYS_pidfd_send_signal, m_reference, SIGKILL, nullptr, 0);
        }
        else {
            // No other fixture path waits this child. A non-consuming wait
            // confirms the still-owned occurrence before this numeric signal.
            siginfo_t info{};
            int observed;
            do {
                observed = ::waitid(P_PID, static_cast<id_t>(m_child), &info,
                    WEXITED | WNOHANG | WNOWAIT);
            } while (observed < 0 && errno == EINTR);
            if (observed == 0) {
                (void)::kill(m_child, SIGKILL);
            }
        }
        pid_t waited;
        do {
            waited = ::waitpid(m_child, nullptr, 0);
        } while (waited < 0 && errno == EINTR);
        const bool collected = waited == m_child;
        m_child = -1;
        if (m_reference >= 0) {
            ::close(m_reference);
            m_reference = -1;
        }
        return collected;
    }

private:
    pid_t m_child = -1;
    int m_reference = -1;
};
#endif

inline bool check_foreground_cleanup(const QString& executable,
    Foreground_cleanup_case scenario = Foreground_cleanup_case::NORMAL)
{
#if defined(__linux__)
    using Cleanup = internal::Native_backend_cleanup_reservation;
    using Clock = std::chrono::steady_clock;
    constexpr auto timeout = std::chrono::seconds(10);
    std::cerr << "FOREGROUND_CLEANUP uid=" << ::getuid() << " euid=" << ::geteuid()
              << " scenario=" << static_cast<int>(scenario) << '\n';
    if (!check(::geteuid() != 0, "foreground gate runs as an ordinary non-root UID")) {
        return false;
    }
    // This mode is a dedicated process. Subreaping lets it collect its fixture
    // job after the backend collects the shell, without competing for that wait.
    if (!check(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "fixture subreaper containment")) {
        return false;
    }
    struct sigaction ignore{};
    struct sigaction previous{};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    if (!check(::sigaction(SIGPIPE, &ignore, &previous) == 0, "fixture pipe error handling")) {
        return false;
    }
    struct Restore_signal
    {
        struct sigaction previous;
        ~Restore_signal() { ::sigaction(SIGPIPE, &previous, nullptr); }
    } restore{previous};

    Foreground_fixture_pipe control;
    Foreground_fixture_pipe report;
    if (!check(control.open() && report.open(), "foreground fixture control pipes") ||
        !check(::fcntl(control.read_end(), F_SETFD, 0) == 0 &&
               ::fcntl(report.write_end(), F_SETFD, 0) == 0, "fixture exec pipe inheritance"))
    {
        return false;
    }
    struct Output
    {
        std::mutex mutex;
        std::condition_variable changed;
        QByteArray bytes;
    };
    auto output = std::make_shared<Output>();
    internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [output](QByteArray bytes) {
        const std::lock_guard lock(output->mutex);
        output->bytes += bytes;
        output->changed.notify_all();
    };
    callbacks.process_exited = [](internal::Terminal_backend_exit) {};
    callbacks.error_reported = [](internal::Terminal_backend_error error) {
        std::cerr << "FOREGROUND_CLEANUP backend_error=" << error.message.toStdString() << '\n';
    };
    internal::Terminal_launch_config config;
    config.argv = {executable, QStringLiteral("--foreground-cleanup-shell"),
        QString::number(control.read_end()), QString::number(report.write_end())};
    config.working_directory = QFileInfo(executable).absolutePath();
    config.initial_grid_size = internal::terminal_grid_size_t{24, 80};
    const bool host_eof = scenario == Foreground_cleanup_case::HOST_EOF;
    Foreground_fixture_host host_owner;
    pid_t host = -1;
    if (host_eof) {
        // This dedicated CLI mode has not created any backend/Qt threads yet.
        host = ::fork();
        if (!check(host >= 0, "disposable backend host fork")) {
            return false;
        }
        if (host > 0 && !check(host_owner.adopt(host), "retain exact disposable host")) {
            return false;
        }
    }
    std::unique_ptr<internal::Terminal_backend> backend;
    bool started = true;
    if (!host_eof || host == 0) {
        backend = internal::make_posix_pty_backend();
        started = backend->start(config, std::move(callbacks)).code ==
            internal::Terminal_backend_result_code::ACCEPTED;
    }
    if (host_eof && host == 0) {
        control.close_read();
        control.close_write();
        report.close_read();
        report.close_write();
        if (!started) {
            backend.reset();
            Cleanup::wait_until_idle(Clock::now() + timeout);
            ::_exit(121);
        }
        for (;;) {
            ::pause(); // Controller kills this exact host without facade cleanup.
        }
    }
    control.close_read();
    report.close_write();
    bool ok = check(started,
        "actual PTY foreground fixture starts");
    foreground_cleanup_identity_t identity{};
    const bool identified = read_foreground_control(report.read_end(), &identity, sizeof(identity));
    ok &= check(identified && identity.root > 1 && identity.foreground > 1 &&
        identity.root != identity.foreground && identity.session == identity.root &&
        identity.observed_foreground == identity.foreground, "real separate PTY foreground identity");
    if (!ok) {
        control.close_write();
        backend.reset();
        Cleanup::wait_until_idle(Clock::now() + timeout);
        return false;
    }
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, identity.foreground, 0));
    ok &= check(pidfd >= 0, "retain the exact fixture process for liveness observation");
    if (!host_eof) {
        std::unique_lock lock(output->mutex);
        ok &= check(output->changed.wait_for(lock, timeout, [&] {
            return output->bytes.contains("FOREGROUND_CLEANUP_READY");
        }), "foreground job writes through the actual PTY");
    }
    if (host_eof) {
        char acknowledgement = 0;
        ok &= check(::write(control.write_end(), "p", 1) == 1 &&
            read_foreground_control(report.read_end(), &acknowledgement, 1) && acknowledgement == 'a',
            "host EOF fixture foreground ready before crash");
        ok &= check(host_owner.terminate_and_collect(), "crashed host collected by containment");
        pollfd ended{pidfd, POLLIN, 0};
        ok &= check(::poll(&ended, 1, 10000) == 1 && (ended.revents & POLLIN) != 0,
            "control EOF causes surviving helper to terminate foreground");
    }
    const bool owner_loss = scenario == Foreground_cleanup_case::OWNER_LOSS;
    const auto unconfirmed_before = Cleanup::unconfirmed_tasks();
    if (scenario == Foreground_cleanup_case::ADOPTED) {
        ok &= check(::write(control.write_end(), "r", 1) == 1,
            "fixture root exits while foreground job requires adoption");
        pollfd ended{pidfd, POLLIN, 0};
        ok &= check(::poll(&ended, 1, 10000) == 1 && (ended.revents & POLLIN) != 0,
            "dedicated owner terminates adopted foreground before facade destruction");
    }
    if (owner_loss) {
        // The shell reports its original owner, which is still alive while
        // this fixture holds the root open. Signal only a retained descriptor.
        const int owner_fd = static_cast<int>(::syscall(SYS_pidfd_open, identity.owner, 0));
        ok &= check(owner_fd >= 0, "retain fixture owner identity before catastrophe");
        if (owner_fd >= 0) {
            ok &= check(::syscall(SYS_pidfd_send_signal, owner_fd, SIGKILL, nullptr, 0) == 0,
                "contained loss of the last dedicated owner");
            ::close(owner_fd);
        }
    }
    backend.reset();
    const bool settled = Cleanup::wait_until_idle(Clock::now() + timeout);
    bool alive = false;
    if (::write(control.write_end(), "p", 1) == 1) {
        char acknowledgement = 0;
        alive = read_foreground_control(report.read_end(), &acknowledgement, 1) && acknowledgement == 'a';
    }
    pollfd process{pidfd, POLLIN, 0};
    const int observed = pidfd >= 0 ? ::poll(&process, 1, 0) : -1;
    const bool exited = observed == 1 && (process.revents & POLLIN) != 0;
    std::cerr << "FOREGROUND_CLEANUP root=" << identity.root
              << " foreground=" << identity.foreground << " session=" << identity.session
              << " registry_settled=" << settled << " foreground_ack=" << alive
              << " pidfd_poll=" << observed << " foreground_exited=" << exited << '\n';
    ok &= check(alive || exited,
        "foreground liveness observation is conclusive");
    ok &= check(!settled || !alive,
        "cleanup registry must not settle while the original foreground job survives");
    if (owner_loss) {
        ok &= check(!settled && Cleanup::unconfirmed_tasks() == unconfirmed_before + 1,
            "owner loss leaves a disposed unconfirmed outcome instead of fake settlement");
    }
    else {
        ok &= check(settled && exited && !alive,
            "normal close must terminate the foreground job and settle before fixture fallback");
    }

    // Cooperative EOF remains independent of the PTY, shell, and product's
    // group-signal capability. Never signal a remembered numeric process group.
    control.close_write();
    if (!owner_loss) {
        ok &= check(Cleanup::wait_until_idle(Clock::now() + timeout),
            "native owner settles after fixture cooperative exit");
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(identity.foreground, &status, 0);
    } while (waited < 0 && errno == EINTR);
    ok &= check(waited == identity.foreground || (waited < 0 && errno == ECHILD),
        "fixture foreground consuming wait completes or belonged to its shell");
    if (pidfd >= 0) {
        ::close(pidfd);
    }
    if (owner_loss) {
        // After the owner dies this test subreaper, not the backend, adopts
        // the root. Cooperative EOF lets it finish without numeric signalling.
        do {
            waited = ::waitpid(identity.root, &status, 0);
        } while (waited < 0 && errno == EINTR);
        ok &= check(waited == identity.root || (waited < 0 && errno == ECHILD),
            "catastrophe fixture root collected by containment");
        std::cerr << "FOREGROUND_CLEANUP unconfirmed_records=" << Cleanup::unconfirmed_tasks()
                  << " fixture_cooperative_cleanup=1\n";
    }
    if (host_eof) {
        do {
            waited = ::waitpid(identity.owner, &status, 0);
        } while (waited < 0 && errno == EINTR);
        ok &= check(waited == identity.owner && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "orphaned cleanup helper settles and is collected by containment");
        std::cerr << "FOREGROUND_CLEANUP host_collected=1 helper_collected="
                  << (waited == identity.owner) << '\n';
    }
    return ok;
#else
    (void)executable;
    return check(false, "foreground cleanup containment gate requires Linux");
#endif
}

} // namespace vnm_terminal::test_helpers
