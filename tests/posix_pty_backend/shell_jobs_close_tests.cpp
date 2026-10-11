#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QString>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;
using Clock = std::chrono::steady_clock;

class Capture
{
public:
    term::Terminal_backend_callbacks callbacks()
    {
        term::Terminal_backend_callbacks result;
        result.output_received = [this](QByteArray bytes) {
            const std::lock_guard lock(m_mutex);
            m_output += bytes;
            m_changed.notify_all();
        };
        result.process_exited = [this](term::Terminal_backend_exit exit) {
            const std::lock_guard lock(m_mutex);
            m_exit = exit;
            m_changed.notify_all();
        };
        result.error_reported = [this](term::Terminal_backend_error error) {
            const std::lock_guard lock(m_mutex);
            m_error = true;
            std::fprintf(stderr, "backend error: %s\n", error.message.toLocal8Bit().constData());
            m_changed.notify_all();
        };
        return result;
    }

    pid_t wait_for_pid(const QByteArray& marker)
    {
        std::unique_lock lock(m_mutex);
        pid_t result = -1;
        m_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            const auto offset = m_output.indexOf(marker);
            if (offset >= 0) {
                const auto first = offset + marker.size();
                const auto end = m_output.indexOf('\n', first);
                if (end >= 0) {
                    bool valid = false;
                    const int value = m_output.mid(first, end - first).trimmed().toInt(&valid);
                    if (valid && value > 1) {
                        result = value;
                    }
                }
            }
            return result > 1 || m_exit.has_value() || m_error;
        });
        if (result <= 1) {
            std::fprintf(stderr, "shell output: %s\n", m_output.constData());
        }
        return result;
    }

    bool wait_for_exit()
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(3), [&] {
            return m_exit.has_value();
        }) && !m_error;
    }

private:
    std::mutex                                m_mutex;
    std::condition_variable                   m_changed;
    QByteArray                                m_output;
    std::optional<term::Terminal_backend_exit> m_exit;
    bool                                      m_error = false;
};

// Every observation is installed before closure while the fixture's twenty-
// second jobs are alive. These kqueue registrations confer no signal authority.
class Exit_observers
{
public:
    Exit_observers() : m_fd(::kqueue()) {}
    ~Exit_observers()
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }

    bool add(pid_t pid)
    {
        struct kevent event;
        EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC,
            EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        return m_fd >= 0 && ::kevent(m_fd, &event, 1, nullptr, 0, nullptr) == 0;
    }

    bool still_running()
    {
        struct kevent event;
        const timespec immediate{0, 0};
        return ::kevent(m_fd, nullptr, 0, &event, 1, &immediate) == 0;
    }

    bool wait_for_all(const std::array<pid_t, 3>& pids)
    {
        std::array<bool, 3> exited{};
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            struct kevent event;
            const timespec interval{0, 100000000};
            const int count = ::kevent(m_fd, nullptr, 0, &event, 1, &interval);
            if (count < 0 || (count == 1 && (event.flags & EV_ERROR) != 0)) {
                return false;
            }
            if (count == 1 && (event.fflags & NOTE_EXIT) != 0) {
                for (std::size_t i = 0; i < pids.size(); ++i) {
                    if (event.ident == static_cast<uintptr_t>(pids[i])) {
                        exited[i] = true;
                    }
                }
            }
            if (exited[0] && exited[1] && exited[2]) {
                return true;
            }
        }
        std::fprintf(stderr, "exit observations shell=%d foreground=%d background=%d\n",
            exited[0], exited[1], exited[2]);
        return false;
    }

private:
    int m_fd;
};

bool wait_for_foreground(pid_t shell, pid_t foreground)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    do {
        kinfo_proc info{};
        std::size_t size = sizeof(info);
        int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, shell};
        if (::sysctl(query, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info) &&
            info.kp_eproc.e_tpgid == foreground)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (Clock::now() < deadline);
    return false;
}

bool test_shell_jobs_close(bool destroy)
{
    std::fprintf(stderr, "PHASE %s BEGIN fixture=%d\n", destroy ? "destruction" : "terminate", ::getpid());
    const auto prior_unconfirmed = term::Native_backend_cleanup_reservation::unconfirmed_tasks();
    Capture capture;
    auto backend = term::make_posix_pty_backend();
    term::Terminal_launch_config config;
    config.argv = {QStringLiteral("/bin/zsh"), QStringLiteral("-f"), QStringLiteral("-i"),
        QStringLiteral("+o"), QStringLiteral("zle")};
    config.working_directory = QStringLiteral("/tmp");
    config.initial_grid_size = term::terminal_grid_size_t{24, 120};
    if (!check(backend->start(config, capture.callbacks()).code ==
        term::Terminal_backend_result_code::ACCEPTED, "interactive zsh starts"))
    {
        return false;
    }
    backend->write(QByteArrayLiteral("stty -echo; printf '\\nSHELL_READY:%s\\n' $$\n"));
    const pid_t shell = capture.wait_for_pid(QByteArrayLiteral("\nSHELL_READY:"));
    if (!check(shell > 1, "interactive shell reports readiness")) {
        return false;
    }
    backend->write(QByteArrayLiteral(
        "/bin/sleep 20 & b=$!; printf '\\nJOB_BG:%s\\n' $b; "
        "/bin/sleep 20 & f=$!; printf '\\nJOB_FG:%s\\n' $f; fg %+\n"));
    const pid_t background = capture.wait_for_pid(QByteArrayLiteral("\nJOB_BG:"));
    const pid_t foreground = capture.wait_for_pid(QByteArrayLiteral("\nJOB_FG:"));
    if (!check(background > 1 && foreground > 1 && background != foreground &&
        foreground != shell && background != shell && wait_for_foreground(shell, foreground),
        "ordinary foreground and background jobs use distinct groups"))
    {
        return false;
    }
    const std::array<pid_t, 3> pids{shell, foreground, background};
    std::fprintf(stderr, "PHASE %s READY shell=%d foreground=%d background=%d\n",
        destroy ? "destruction" : "terminate", shell, foreground, background);
    Exit_observers observers;
    for (const pid_t pid : pids) {
        if (!check(observers.add(pid), "exact live process exit observer registers")) {
            return false;
        }
    }
    if (!check(observers.still_running(), "all original processes are alive before closure")) {
        return false;
    }
    bool ok = true;
    if (destroy) {
        backend.reset();
    }
    else {
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
            "ordinary interactive termination is accepted");
        ok &= check(capture.wait_for_exit(), "original shell exits without a backend error");
    }
    std::fprintf(stderr, "PHASE %s CLOSE_RETURNED\n", destroy ? "destruction" : "terminate");
    ok &= check(observers.wait_for_all(pids),
        "closure ends the original shell and its ordinary foreground and background jobs");
    backend.reset();
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (term::Native_backend_cleanup_reservation::unconfirmed_tasks() == prior_unconfirmed &&
        Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ok &= check(term::Native_backend_cleanup_reservation::unconfirmed_tasks() == prior_unconfirmed + 1,
        "observed fixture exits do not fabricate complete terminal-wide cleanup coverage");
    std::fprintf(stderr, "PHASE %s END result=%d\n", destroy ? "destruction" : "terminate", ok);
    return ok;
}

} // namespace

int main()
{
    bool ok = test_shell_jobs_close(false);
    ok &= test_shell_jobs_close(true);
    return ok ? 0 : 1;
}
