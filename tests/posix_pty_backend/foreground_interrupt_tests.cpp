#include "helpers/test_check.h"
#include "native_receipt_capture.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string_view>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

// The fixture owns and waits for one job in a different foreground group.
// Alarms bound fixture failures without signalling any discovered PID/group.
int run_shell()
{
    ::signal(SIGTTOU, SIG_IGN);
    ::alarm(12);
    int ready[2];
    if (::pipe(ready) != 0) {
        return 2;
    }
    const pid_t job = ::fork();
    if (job < 0) {
        return 3;
    }
    if (job == 0) {
        ::close(ready[1]);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGALRM, SIG_DFL);
        ::alarm(8);
        char admission;
        if (::setpgid(0, 0) != 0 || ::read(ready[0], &admission, 1) != 1 ||
            ::tcgetpgrp(STDIN_FILENO) != ::getpgrp() || ::getpgrp() == ::getppid())
        {
            ::_exit(4);
        }
        ::close(ready[0]);
        constexpr char marker[] = "FOREGROUND_JOB_READY\n";
        if (::write(STDOUT_FILENO, marker, sizeof(marker) - 1) != sizeof(marker) - 1) {
            ::_exit(5);
        }
        for (;;) {
            ::pause();
        }
    }

    ::close(ready[0]);
    const char admission = 'g';
    const bool admitted = ::setpgid(job, job) == 0 &&
        ::tcsetpgrp(STDIN_FILENO, job) == 0 && ::write(ready[1], &admission, 1) == 1;
    ::close(ready[1]);
    if (!admitted) {
        ::kill(job, SIGKILL); // The original, unreaped direct child only.
    }
    int status = 0;
    pid_t reaped;
    do {
        reaped = ::waitpid(job, &status, 0);
    }
    while (reaped < 0 && errno == EINTR);
    if (!admitted || reaped != job || !WIFSIGNALED(status) || WTERMSIG(status) != SIGINT ||
        ::tcsetpgrp(STDIN_FILENO, ::getpgrp()) != 0)
    {
        return 6;
    }
    constexpr char marker[] = "FOREGROUND_JOB_INTERRUPTED\n";
    if (::write(STDOUT_FILENO, marker, sizeof(marker) - 1) != sizeof(marker) - 1) {
        return 7;
    }
    char command;
    const bool quit = ::read(STDIN_FILENO, &command, 1) == 1 && command == 'q';
    ::alarm(0);
    return quit ? 0 : 8;
}

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

    bool wait_for_output(const QByteArray& marker)
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return m_output.contains(marker) || m_exit.has_value();
        });
        return m_output.contains(marker);
    }

    bool shell_running()
    {
        const std::lock_guard lock(m_mutex);
        return !m_exit.has_value();
    }

    bool wait_for_normal_exit()
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return m_exit.has_value();
        }) && !m_error && m_exit->reason == term::Terminal_exit_reason::EXITED &&
            m_exit->exit_code == 0;
    }

private:
    std::mutex                                 m_mutex;
    std::condition_variable                    m_changed;
    QByteArray                                 m_output;
    std::optional<term::Terminal_backend_exit> m_exit;
    bool                                       m_error = false;
};

bool test_foreground_interrupt(const QString& executable)
{
    Capture capture;
    auto backend = term::make_posix_pty_backend();
    term::Terminal_launch_config config;
    config.argv = {executable, QStringLiteral("--shell")};
    config.working_directory = QFileInfo(executable).absolutePath();
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
#if defined(__APPLE__)
    const auto receipts = vnm_terminal::test_helpers::retain_native_receipts(config);
#endif
    if (!check(backend->start(config, capture.callbacks()).code ==
        term::Terminal_backend_result_code::ACCEPTED, "foreground interrupt fixture starts"))
    {
        return false;
    }
    if (!check(capture.wait_for_output(QByteArrayLiteral("FOREGROUND_JOB_READY")),
        "job owns a foreground group distinct from its shell"))
    {
        return false;
    }
    const auto interrupted = backend->interrupt();
    bool ok = check(interrupted.code == term::Terminal_backend_result_code::ACCEPTED,
        "backend interrupt accepts an ordinary foreground job");
    if (!ok) {
        // Clean up the baseline failure through the terminal's input discipline.
        // This does not turn the rejected explicit interrupt into a passing test.
        backend->write(QByteArray(1, '\003'));
    }
    ok &= check(capture.wait_for_output(QByteArrayLiteral("FOREGROUND_JOB_INTERRUPTED")),
        "original shell reaps its foreground job with SIGINT");
    ok &= check(capture.shell_running(), "interrupt preserves the original shell");
    ok &= check(backend->write(QByteArrayLiteral("q\n")).code ==
        term::Terminal_backend_result_code::ACCEPTED, "original shell still accepts input");
    ok &= check(capture.wait_for_normal_exit(), "shell exits normally on explicit request");
#if defined(__APPLE__)
    backend.reset();
    ok &= check(receipts->helper_exited(vnm::process_custody::Darwin_pty_owner_exit::CONFIRMED),
        "exact retained helper confirms cleanup after foreground interruption");
#endif
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--shell") {
        return run_shell();
    }
    if (argc != 1) {
        return 2;
    }
    return test_foreground_interrupt(QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath()) ? 0 : 1;
}
