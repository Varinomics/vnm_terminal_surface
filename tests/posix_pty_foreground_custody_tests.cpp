#if defined(__linux__) && defined(VNM_TERMINAL_TEST_NO_GROUP_PIDFD)
#include <sys/syscall.h>
#undef SYS_pidfd_send_signal
#endif
#include "posix_process_group_custody.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <vector>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {
using vnm_terminal::internal::Posix_process_group_custody;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Every shell is an unreaped child of this test; each shell is the sole waiter
// of its foreground job. Cooperative EOF cleans up even an unsupported capture.
std::vector<int> controls;

class Pty_shell
{
public:
    Pty_shell()
    {
        int command[2]{-1, -1};
        int result[2]{-1, -1};
        require(::pipe(command) == 0, "shell command pipe");
        if (::pipe(result) != 0) {
            ::close(command[0]); ::close(command[1]);
            throw std::runtime_error("shell result pipe");
        }
        // Reserve bookkeeping before birth; adoption performs no allocation.
        controls.reserve(controls.size() + 1);
        m_pid = ::forkpty(&m_master, nullptr, nullptr, nullptr);
        if (m_pid == 0) {
            for (int fd : controls) ::close(fd);
            ::close(command[1]); ::close(result[0]);
            run_shell(command[0], result[1]);
        }
        ::close(command[0]); ::close(result[1]);
        if (m_pid < 0) {
            ::close(command[1]); ::close(result[0]);
            throw std::runtime_error("forkpty");
        }
        m_control = command[1]; m_result = result[0];
        controls.push_back(m_control);
        m_owner.adopt_owned_child(m_pid);
        if (!receive(&m_foreground, sizeof(m_foreground)) || m_foreground <= 1) {
            finish();
            throw std::runtime_error("actual PTY foreground handshake");
        }
    }

    ~Pty_shell() { finish(); }

    void finish() noexcept
    {
        if (m_control >= 0) {
            std::erase(controls, m_control);
            ::close(m_control);
            m_control = -1;
        }
        int status = 0;
        (void)m_owner.reap(&status);
        if (m_master >= 0) { ::close(m_master); m_master = -1; }
        if (m_result >= 0) { ::close(m_result); m_result = -1; }
    }

    int finish_job()
    {
        std::erase(controls, m_control);
        ::close(m_control);
        m_control = -1;
        int status = 0;
        require(receive(&status, sizeof(status)), "foreground consuming-wait result");
        return status;
    }

    void read_terminal_marker()
    {
        std::string text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (text.find("REAL_PTY_FOREGROUND") == std::string::npos &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd ready{m_master, POLLIN, 0};
            if (::poll(&ready, 1, 100) > 0) {
                char bytes[128]{};
                const auto count = ::read(m_master, bytes, sizeof(bytes));
                if (count > 0) text.append(bytes, static_cast<std::size_t>(count));
            }
        }
        require(text.find("REAL_PTY_FOREGROUND") != std::string::npos, "real foreground PTY bytes");
    }

    pid_t pid() const { return m_pid; }
    pid_t foreground() const { return m_foreground; }
    int master() const { return m_master; }
    Posix_process_group_custody& owner() { return m_owner; }

private:
    static void run_shell(int control, int result)
    {
        (void)::signal(SIGHUP, SIG_IGN);
        (void)::signal(SIGTTOU, SIG_IGN);
        int job_control[2];
        if (::pipe(job_control) != 0) ::_exit(110);
        const pid_t job = ::fork();
        if (job < 0) ::_exit(111);
        if (job == 0) {
            ::close(job_control[1]); ::close(control); ::close(result);
            if (::setpgid(0, 0) != 0) ::_exit(112);
            char command = 0;
            ssize_t count;
            do { count = ::read(job_control[0], &command, 1); } while (count < 0 && errno == EINTR);
            if (count != 1 || command != 'g') ::_exit(113);
            constexpr char marker[] = "REAL_PTY_FOREGROUND\n";
            if (::write(STDOUT_FILENO, marker, sizeof(marker) - 1) != sizeof(marker) - 1) ::_exit(114);
            do { count = ::read(job_control[0], &command, 1); } while (count < 0 && errno == EINTR);
            ::_exit(count == 0 ? 23 : 115);
        }
        ::close(job_control[0]);
        if (::setpgid(job, job) != 0 || ::tcsetpgrp(STDIN_FILENO, job) != 0) ::_exit(116);
        const char go = 'g';
        if (::write(job_control[1], &go, 1) != 1 || ::write(result, &job, sizeof(job)) != sizeof(job)) ::_exit(117);
        char command = 0;
        while (::read(control, &command, 1) < 0 && errno == EINTR) {}
        ::close(job_control[1]);
        int status = 0;
        pid_t waited;
        do { waited = ::waitpid(job, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != job) ::_exit(118);
        if (::write(result, &status, sizeof(status)) != sizeof(status)) ::_exit(119);
        ::_exit(0);
    }

    bool receive(void* bytes, std::size_t count)
    {
        pollfd ready{m_result, POLLIN, 0};
        int polled;
        do { polled = ::poll(&ready, 1, 5000); } while (polled < 0 && errno == EINTR);
        if (polled <= 0) return false;
        ssize_t read;
        do { read = ::read(m_result, bytes, count); } while (read < 0 && errno == EINTR);
        return read == static_cast<ssize_t>(count);
    }

    pid_t m_pid = -1;
    pid_t m_foreground = -1;
    int m_master = -1;
    int m_control = -1;
    int m_result = -1;
    Posix_process_group_custody m_owner{-1};
};
} // namespace

int main()
{
    try {
        Pty_shell first;
        Pty_shell foreign;
        first.read_terminal_marker();
        foreign.read_terminal_marker();
        require(::tcgetsid(first.master()) == first.pid(), "actual controlling-terminal session");
        require(::tcgetpgrp(first.master()) == first.foreground(), "actual foreground process group");
        require(first.foreground() != first.pid(), "foreground job differs from shell group");
        require(!Posix_process_group_custody::capture_foreground(foreign.foreground(), first.pid()),
            "foreign PTY session cannot be captured");
        auto target = Posix_process_group_custody::capture_foreground(first.foreground(), first.pid());
        if (!target) {
            std::cout << "SKIP: actual PTY I/O passed; retained foreground signalling unsupported\n";
            return 77;
        }
        require(target->signal_group(SIGKILL) == 0, "signal exact foreground job");
        const int status = first.finish_job();
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "shell observed killed foreground job");
        first.finish(); // shell/root and job have now both been consumed
        for (int i = 0; i != 20; ++i) {
            require(target->signal_group(SIGKILL) == ESRCH, "post-reap group identity cannot alias");
            require(foreign.owner().signal_root(0) == 0, "other PTY shell is unaffected");
        }
        const int other_status = foreign.finish_job();
        require(WIFEXITED(other_status) && WEXITSTATUS(other_status) == 23, "other foreground job exited cooperatively");
        std::cout << "PASS: real forkpty, foreground job/session, exact signal, sole waits, unrelated PTY, late no-alias\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " errno=" << errno << '\n';
        return 1;
    }
}
