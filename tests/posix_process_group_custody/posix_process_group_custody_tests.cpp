#if defined(__linux__) && defined(VNM_TERMINAL_TEST_NO_GROUP_PIDFD)
#include <sys/syscall.h>
#undef SYS_pidfd_send_signal
#endif

#include "posix_process_group_custody.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace {
using vnm_terminal::internal::Posix_process_group_custody;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Every teardown target is still an owned child, or a retained group reference.
// EOF is a cooperative fallback; no fixture ever signals a remembered PID after
// transferring its consuming wait to another owner.
std::vector<int> fixture_control_writers;

class Owned_child final
{
public:
    Owned_child()
    {
        int control[2] = {-1, -1};
        int ready[2] = {-1, -1};
        require(::pipe(control) == 0, "control pipe");
        if (::pipe(ready) != 0) {
            ::close(control[0]);
            ::close(control[1]);
            throw std::runtime_error("ready pipe");
        }
        m_pid = ::fork();
        if (m_pid == 0) {
            for (const int inherited : fixture_control_writers) {
                ::close(inherited);
            }
            ::close(control[1]);
            ::close(ready[0]);
            const char started = ::setsid() > 0 ? '1' : '0';
            (void)::write(ready[1], &started, 1);
            ::close(ready[1]);
            char command = 0;
            while (::read(control[0], &command, 1) < 0 && errno == EINTR) {}
            ::_exit(started == '1' ? 23 : 124);
        }
        ::close(control[0]);
        ::close(ready[1]);
        if (m_pid < 0) {
            ::close(control[1]);
            ::close(ready[0]);
            throw std::runtime_error("fork");
        }
        m_control = control[1];
        fixture_control_writers.push_back(m_control);
        m_custody.adopt_owned_child(m_pid);
        char started = 0;
        ssize_t count = -1;
        do {
            count = ::read(ready[0], &started, 1);
        } while (count < 0 && errno == EINTR);
        ::close(ready[0]);
        if (count != 1 || started != '1') {
            release();
            int status = 0;
            (void)m_custody.reap(&status);
            throw std::runtime_error("child session handshake");
        }
    }

    ~Owned_child()
    {
        release();
        int status = 0;
        (void)m_custody.reap(&status);
    }

    void release() noexcept
    {
        if (m_control >= 0) {
            std::erase(fixture_control_writers, m_control);
            ::close(m_control);
            m_control = -1;
        }
    }
    pid_t pid() const noexcept { return m_pid; }
    Posix_process_group_custody& custody() noexcept { return m_custody; }

private:
    pid_t m_pid = -1;
    int m_control = -1;
    Posix_process_group_custody m_custody{-1};
};

void owned_root_wait_and_signal_test()
{
    Owned_child child;
    require(child.custody().signal_group(0) == 0, "owned group is signalable");
    child.release();
    require(child.custody().observe_exit() == 0, "observe exit without consuming wait");
    siginfo_t still_waitable{};
    require(::waitid(P_PID, static_cast<id_t>(child.pid()), &still_waitable,
        WEXITED | WNOWAIT | WNOHANG) == 0 && still_waitable.si_pid == child.pid(),
        "non-consuming observation retains native child identity");
    int status = 0;
    require(child.custody().reap(&status) == child.pid(), "sole consuming wait");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 23, "actual native exit status");
    require(child.custody().signal_root(SIGKILL) == ESTALE,
        "retired numeric root cannot be signalled");
    const int late_group = child.custody().signal_group(SIGKILL);
    require(late_group == ESRCH || late_group == ESTALE,
        "late group dispatch cannot acquire a new numeric identity");
    require(child.custody().reap(&status) == -1 && errno == ECHILD,
        "consuming wait is not duplicated");
}

void foreground_identity_test()
{
    Owned_child first;
    Owned_child foreign;
    require(!Posix_process_group_custody::capture_foreground(foreign.pid(), first.pid()),
        "foreign session must not become a signal target");
#if defined(__linux__)
    const auto target = Posix_process_group_custody::capture_foreground(first.pid(), first.pid());
    if (!target) {
        std::cout << "SKIP retained foreground group: kernel lacks process-group pidfd support\n";
        return;
    }
    require(target->signal_group(0) == 0, "exact foreground session admitted");
    first.release();
    require(first.custody().observe_exit() == 0, "foreground root observation");
    int status = 0;
    require(first.custody().reap(&status) == first.pid(), "foreground root collection");
    require(target->signal_group(SIGKILL) == ESRCH,
        "captured foreground reference stays bound after root collection");
    require(foreign.custody().signal_group(0) == 0, "unrelated group remains alive");
#else
    require(!Posix_process_group_custody::capture_foreground(first.pid(), first.pid()),
        "unsupported foreground identity must remain unconfirmed");
#endif
}

void concurrent_reap_and_signal_test()
{
    for (int iteration = 0; iteration != 30; ++iteration) {
        Owned_child child;
        child.release();
        require(child.custody().observe_exit() == 0, "concurrent root observation");
        int signal_result = 0;
        std::thread signaler([&] { signal_result = child.custody().signal_root(SIGKILL); });
        int status = 0;
        const auto collected = child.custody().reap(&status);
        signaler.join();
        require(collected == child.pid(), "concurrent single consuming wait");
        require(signal_result == 0 || signal_result == ESRCH || signal_result == ESTALE,
            "signal has either retained authority or a retired result");
        require(child.custody().signal_root(SIGKILL) == ESTALE, "no post-reap root fallback");
    }
}

void lost_wait_authority_test()
{
    Owned_child child;
    child.release();
    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(child.pid(), &status, 0);
    } while (result < 0 && errno == EINTR);
    require(result == child.pid(), "injected competing consuming wait");
    require(child.custody().observe_exit() == ECHILD, "lost wait authority is explicit");
    require(child.custody().signal_root(SIGKILL) == ESTALE,
        "ECHILD does not preserve numeric fallback");
}
} // namespace

int main()
{
    try {
        std::cout << "RUN owned root\n" << std::flush;
        owned_root_wait_and_signal_test();
        std::cout << "RUN foreground identity\n" << std::flush;
        foreground_identity_test();
        std::cout << "RUN concurrent wait\n" << std::flush;
        concurrent_reap_and_signal_test();
        std::cout << "RUN lost wait\n" << std::flush;
        lost_wait_authority_test();
        std::cout << "PASS POSIX retained group identity, non-consuming observation, sole wait, "
            "foreign-session rejection and concurrent retirement\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << " (errno=" << errno << ")\n";
        return 1;
    }
}
