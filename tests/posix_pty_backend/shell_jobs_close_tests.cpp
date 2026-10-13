#include "helpers/test_check.h"
#include "native_receipt_capture.h"
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

    bool exited_with(int code)
    {
        const std::lock_guard lock(m_mutex);
        return m_exit && m_exit->reason == term::Terminal_exit_reason::EXITED && m_exit->exit_code == code;
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

enum class Closure { TERMINATE, DESTRUCTION, SHELL_EXIT };

bool test_shell_jobs_close(Closure closure)
{
    const bool shell_first = closure == Closure::SHELL_EXIT;
    const char* label = shell_first ? "shell-first" : closure == Closure::DESTRUCTION ? "destruction" : "terminate";
    std::fprintf(stderr, "PHASE %s BEGIN fixture=%d\n", label, ::getpid());
    Capture capture;
    auto backend = term::make_posix_pty_backend();
    term::Terminal_launch_config config;
    config.argv = {QStringLiteral("/bin/zsh"), QStringLiteral("-f"), QStringLiteral("-i"),
        QStringLiteral("+o"), QStringLiteral("zle")};
    config.working_directory = QStringLiteral("/tmp");
    config.initial_grid_size = term::terminal_grid_size_t{24, 120};
    const auto receipts = vnm_terminal::test_helpers::retain_native_receipts(config);
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
    if (shell_first) {
        backend->write(QByteArrayLiteral(
            "/bin/sh -c 'trap \"\" HUP; printf \"\\nJOB_BG:%s\\n\" $$; exec /bin/sleep 20' & "
            "/bin/sh -c 'trap \"\" HUP; printf \"\\nJOB_FG:%s\\n\" $$; exec /bin/sleep 20' &\n"));
    }
    else {
        backend->write(QByteArrayLiteral(
            "/bin/sleep 20 & b=$!; printf '\\nJOB_BG:%s\\n' $b; "
            "/bin/sleep 20 & f=$!; printf '\\nJOB_FG:%s\\n' $f; fg %+\n"));
    }
    const pid_t background = capture.wait_for_pid(QByteArrayLiteral("\nJOB_BG:"));
    const pid_t foreground = capture.wait_for_pid(QByteArrayLiteral("\nJOB_FG:"));
    if (!check(background > 1 && foreground > 1 && background != foreground &&
        foreground != shell && background != shell &&
        wait_for_foreground(shell, shell_first ? shell : foreground),
        "ordinary shell jobs are live with the expected foreground owner"))
    {
        return false;
    }
    const std::array<pid_t, 3> pids{shell, foreground, background};
    std::fprintf(stderr, "PHASE %s READY shell=%d foreground=%d background=%d\n",
        label, shell, foreground, background);
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
    if (closure == Closure::DESTRUCTION) {
        backend.reset();
    }
    else
    if (shell_first) {
        ok &= check(backend->write(QByteArrayLiteral("setopt no_check_jobs; exit 23\n")).code ==
            term::Terminal_backend_result_code::ACCEPTED, "original shell accepts its own exit request");
        ok &= check(capture.wait_for_exit() && capture.exited_with(23),
            "shell exits with its requested status while HUP-ignoring jobs require cleanup");
    }
    else {
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
            "ordinary interactive termination is accepted");
        ok &= check(capture.wait_for_exit(), "original shell exits without a backend error");
    }
    std::fprintf(stderr, "PHASE %s CLOSE_RETURNED\n", label);
    ok &= check(observers.wait_for_all(pids),
        "closure ends the original shell and its ordinary foreground and background jobs");
    backend.reset();
    ok &= check(receipts->helper_exited(vnm::process_custody::Darwin_pty_owner_exit::CONFIRMED),
        "exact admitted helper confirms the original terminal's group cleanup");
    ok &= check(receipts->wait_for_receipts(3), "original root exit reaches the independent receipt consumer");
    const auto retained = receipts->receipts();
    ok &= check(retained.size() == 3 &&
        retained[0].kind == vnm_terminal::Terminal_native_receipt_kind::HELPER_PRE_START &&
        retained[1].kind == vnm_terminal::Terminal_native_receipt_kind::ROOT_BORN &&
        retained[2].kind == vnm_terminal::Terminal_native_receipt_kind::ROOT_ENDED &&
        retained[1].root_pid == static_cast<std::uint32_t>(shell) &&
        retained[2].root_pid == retained[1].root_pid &&
        retained[2].root_birth_token == retained[1].root_birth_token,
        "pre-START observation, original root birth and exact root exit are retained in order");
    std::fprintf(stderr, "PHASE %s END result=%d\n", label, ok);
    return ok;
}

} // namespace

int main()
{
    bool ok = test_shell_jobs_close(Closure::TERMINATE);
    ok &= test_shell_jobs_close(Closure::DESTRUCTION);
    ok &= test_shell_jobs_close(Closure::SHELL_EXIT);
    return ok ? 0 : 1;
}
