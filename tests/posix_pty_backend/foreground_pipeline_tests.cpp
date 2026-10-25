#include "foreground_cleanup_checks.h"

#include <string_view>
#include <vector>

namespace {

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::Foreground_fixture_pipe;
using vnm_terminal::test_helpers::read_foreground_control;

volatile sig_atomic_t termination_requested = 0;

void request_termination(int)
{
    termination_requested = 1;
}

struct pipeline_identity_t
{
    pid_t root;
    pid_t leader;
    pid_t member;
    pid_t session;
    pid_t foreground;
};

struct pipeline_member_identity_t
{
    pid_t process;
    pid_t group;
    pid_t session;
};

bool control_closed(int control)
{
    pollfd state{control, POLLIN | POLLHUP, 0};
    const int result = ::poll(&state, 1, 25);
    if (result < 0 && errno == EINTR) {
        return false;
    }
    if (result < 0 || (state.revents & POLLNVAL) != 0) {
        return true;
    }
    if (result == 0) {
        return false;
    }
    char command = 0;
    const auto count = ::read(control, &command, 1);
    return count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN);
}

[[noreturn]] void wait_for_fixture_release(int control)
{
    while (!control_closed(control)) {
    }
    ::_exit(0);
}

int run_pipeline_shell(int control, int report)
{
    ::alarm(20);
    (void)::signal(SIGHUP, SIG_IGN);
    (void)::signal(SIGTERM, SIG_IGN);
    (void)::signal(SIGTTOU, SIG_IGN);
    Foreground_fixture_pipe prepared;
    Foreground_fixture_pipe leader_ended;
    if (!prepared.open() || !leader_ended.open()) {
        return 2;
    }
    const pid_t leader = ::fork();
    if (leader < 0) {
        return 3;
    }
    if (leader == 0) {
        prepared.close_read();
        leader_ended.close_read();
        leader_ended.close_write();
        (void)::signal(SIGTERM, SIG_DFL);
        if (::setpgid(0, 0) != 0 || ::write(prepared.write_end(), "L", 1) != 1) {
            ::_exit(4);
        }
        prepared.close_write();
        ::close(report);
        wait_for_fixture_release(control);
    }
    if (::setpgid(leader, leader) != 0) {
        return 5;
    }
    const pid_t member = ::fork();
    if (member < 0) {
        return 6;
    }
    if (member == 0) {
        prepared.close_read();
        leader_ended.close_write();
        (void)::signal(SIGTERM, request_termination);
        if (::setpgid(0, leader) != 0 || ::write(prepared.write_end(), "M", 1) != 1) {
            ::_exit(7);
        }
        prepared.close_write();
        while (!termination_requested) {
            if (control_closed(control)) {
                ::_exit(0);
            }
        }
        char leader_exit = 0;
        if (!read_foreground_control(leader_ended.read_end(), &leader_exit, 1) || leader_exit != 'E') {
            ::_exit(8);
        }
        leader_ended.close_read();
        const pid_t late_member = ::fork();
        if (late_member < 0) {
            ::_exit(9);
        }
        if (late_member == 0) {
            (void)::signal(SIGTERM, SIG_IGN);
            const pipeline_member_identity_t identity{::getpid(), ::getpgrp(), ::getsid(0)};
            if (::write(report, &identity, sizeof(identity)) != sizeof(identity)) {
                ::_exit(10);
            }
            ::close(report);
            wait_for_fixture_release(control);
        }
        ::close(report);
        wait_for_fixture_release(control);
    }
    prepared.close_write();
    leader_ended.close_read();
    if (::setpgid(member, leader) != 0 || ::tcsetpgrp(STDIN_FILENO, leader) != 0) {
        return 11;
    }
    char first = 0;
    char second = 0;
    if (!read_foreground_control(prepared.read_end(), &first, 1) ||
        !read_foreground_control(prepared.read_end(), &second, 1) ||
        !((first == 'L' && second == 'M') || (first == 'M' && second == 'L')))
    {
        return 12;
    }
    prepared.close_read();
    const pipeline_identity_t identity{
        ::getpid(), leader, member, ::tcgetsid(STDIN_FILENO), ::tcgetpgrp(STDIN_FILENO)};
    constexpr char marker[] = "FOREGROUND_PIPELINE_READY\n";
    if (::write(report, &identity, sizeof(identity)) != sizeof(identity) ||
        ::write(STDOUT_FILENO, marker, sizeof(marker) - 1) != sizeof(marker) - 1)
    {
        return 13;
    }
    ::close(report);
    bool announced_exit = false;
    while (!control_closed(control)) {
        if (announced_exit) {
            continue;
        }
        siginfo_t status{};
        if (::waitid(P_PID, static_cast<id_t>(leader), &status, WEXITED | WNOHANG | WNOWAIT) == 0 &&
            status.si_pid == leader)
        {
            if (::write(leader_ended.write_end(), "E", 1) != 1) {
                return 14;
            }
            leader_ended.close_write();
            announced_exit = true;
        }
    }
    return 0;
}

class Observed_pipeline_members
{
public:
    ~Observed_pipeline_members()
    {
        for (const auto& member : m_members) {
            ::close(member.observer);
        }
    }

    bool retain(pid_t process)
    {
        const int observer = static_cast<int>(::syscall(SYS_pidfd_open, process, 0));
        if (!check(observer >= 0, "retain an exact foreground pipeline member before checking termination")) {
            return false;
        }
        m_members.push_back({process, observer});
        return true;
    }

    bool all_ended() const
    {
        bool ended = m_members.size() == 3;
        for (const auto& member : m_members) {
            pollfd state{member.observer, POLLIN, 0};
            const bool ready = ::poll(&state, 1, 0) == 1 && (state.revents & POLLIN) != 0;
            std::cerr << "PIPELINE_MEMBER pid=" << member.process << " ended=" << ready << '\n';
            ended &= ready;
        }
        return ended;
    }

    bool collect_after_fixture_release()
    {
        bool collected = true;
        for (const auto& member : m_members) {
            pollfd state{member.observer, POLLIN, 0};
            if (::poll(&state, 1, 1000) != 1) {
                // Failure containment uses only the original retained process.
                (void)::syscall(SYS_pidfd_send_signal, member.observer, SIGKILL, nullptr, 0);
            }
            int status = 0;
            pid_t waited;
            do {
                waited = ::waitpid(member.process, &status, 0);
            } while (waited < 0 && errno == EINTR);
            collected &= check(waited == member.process,
                "fixture subreaper collects each exact original or late pipeline member");
        }
        return collected;
    }

private:
    struct member_t
    {
        pid_t process;
        int observer;
    };
    std::vector<member_t> m_members;
};

bool check_foreground_pipeline(const QString& executable)
{
    using Cleanup = vnm_terminal::internal::Native_backend_cleanup_reservation;
    constexpr auto timeout = std::chrono::seconds(10);
    if (!check(::geteuid() != 0, "foreground pipeline runs as an ordinary non-root user") ||
        !check(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "pipeline fixture retains orphan collection authority"))
    {
        return false;
    }
    (void)::signal(SIGPIPE, SIG_IGN);
    Foreground_fixture_pipe control;
    Foreground_fixture_pipe report;
    if (!check(control.open() && report.open() &&
        ::fcntl(control.read_end(), F_SETFD, 0) == 0 && ::fcntl(report.write_end(), F_SETFD, 0) == 0,
        "pipeline fixture opens its independent cooperative release and identity channels"))
    {
        return false;
    }
    std::mutex mutex;
    std::condition_variable changed;
    QByteArray output;
    bool exited = false;
    bool error = false;
    vnm_terminal::internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [&](QByteArray bytes) {
        const std::lock_guard lock(mutex);
        output += bytes;
        changed.notify_all();
    };
    callbacks.process_exited = [&](auto) {
        const std::lock_guard lock(mutex);
        exited = true;
        changed.notify_all();
    };
    callbacks.error_reported = [&](auto) {
        const std::lock_guard lock(mutex);
        error = true;
        changed.notify_all();
    };
    vnm_terminal::internal::Terminal_launch_config config;
    config.argv = {executable, QStringLiteral("--pipeline-shell"),
        QString::number(control.read_end()), QString::number(report.write_end())};
    config.working_directory = QFileInfo(executable).absolutePath();
    config.initial_grid_size = vnm_terminal::internal::terminal_grid_size_t{24, 80};
    config.termination_policy.graceful_interval = std::chrono::seconds(1);
    config.termination_policy.kill_interval = std::chrono::seconds(1);
    auto backend = vnm_terminal::test_helpers::make_default_test_backend();
    bool ok = check(backend->start(config, std::move(callbacks)).code ==
        vnm_terminal::internal::Terminal_backend_result_code::ACCEPTED,
        "default backend starts a real foreground pipeline");
    control.close_read();
    report.close_write();
    pipeline_identity_t identity{};
    ok &= check(read_foreground_control(report.read_end(), &identity, sizeof(identity)) &&
        identity.root > 1 && identity.leader > 1 && identity.member > 1 &&
        identity.root != identity.leader && identity.leader != identity.member &&
        identity.session == identity.root && identity.foreground == identity.leader,
        "two distinct original pipeline members share the terminal foreground group");
    Observed_pipeline_members members;
    if (ok) {
        ok &= members.retain(identity.leader);
        ok &= members.retain(identity.member);
    }
    if (ok) {
        std::unique_lock lock(mutex);
        ok &= check(changed.wait_for(lock, timeout, [&] {
            return output.contains("FOREGROUND_PIPELINE_READY") || exited || error;
        }) && output.contains("FOREGROUND_PIPELINE_READY") && !error,
            "foreground pipeline is ready through its ordinary native terminal");
    }
    if (ok) {
        ok &= check(backend->terminate().code == vnm_terminal::internal::Terminal_backend_result_code::ACCEPTED,
            "ordinary foreground pipeline accepts explicit graceful termination");
        pipeline_member_identity_t late{};
        ok &= check(read_foreground_control(report.read_end(), &late, sizeof(late)) &&
            late.process > 1 && late.process != identity.leader && late.process != identity.member &&
            late.group == identity.leader && late.session == identity.session,
            "a retained member forks a new foreground member after the leader exits during TERM grace");
        if (late.process > 1) {
            ok &= members.retain(late.process);
        }
        std::unique_lock lock(mutex);
        ok &= check(changed.wait_for(lock, timeout, [&] { return exited || error; }) && exited && !error,
            "default owner completes the original root after foreground pipeline escalation");
    }
    backend.reset();
    const bool settled = Cleanup::wait_until_idle(std::chrono::steady_clock::now() + timeout);
    ok &= check(members.all_ended(),
        "every original and late foreground member ends before cooperative fixture release");
    ok &= check(settled, "pipeline cleanup settles only after all foreground members end");
    control.close_write();
    ok &= members.collect_after_fixture_release();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--pipeline-shell") {
        bool control_valid = false;
        bool report_valid = false;
        const int control = QString::fromLocal8Bit(argv[2]).toInt(&control_valid);
        const int report = QString::fromLocal8Bit(argv[3]).toInt(&report_valid);
        if (!control_valid || !report_valid || control < 3 || report < 3 || control == report) {
            return 2;
        }
        return run_pipeline_shell(control, report);
    }
    if (argc != 1) {
        return 2;
    }
    ::alarm(25);
    return check_foreground_pipeline(QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath()) ? 0 : 1;
}
